// Док «єБот»: зверху — стан і кнопки (прив'язка, «Оновити пульт», кабінет,
// від'єднати), нижче — пульт: сторінка /dock сайту у вбудованому браузері OBS
// (obs-browser). Стан читається таймером раз на 0,5 с і одразу після кожного кліку.
#include "dock.hpp"
#include "browser-panel.hpp"
#include "config.hpp"
#include "core.hpp"
#include "http.hpp"
#include "pairing.hpp"
#include "relay_client.hpp"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <thread>

namespace ebot {

// QCef живе весь час роботи OBS; віджет браузера — дитина дока (його знищить OBS).
static QCef *g_cef = nullptr;
static QCefCookieManager *g_cookies = nullptr;
static QPointer<QCefWidget> g_browser;
static QPointer<QVBoxLayout> g_layout;
static QPointer<QWidget> g_root;
static QPointer<QLabel> g_note;
static int g_load_gen = 0;           // лише UI-потік
static bool g_frontend_ready = false; // FINISHED_LOADING: можна піднімати CEF
static bool g_no_browser = false;     // OBS без obs-browser — пульту нема де жити
static bool g_loaded = false;         // пульт відкрито для поточного токена
static bool g_load_failed = false;    // код не отримали — спробуємо, щойно релей підключиться
static int g_retry_attempt = 0;       // невдалі автоповтори коду поспіль
static qint64 g_retry_at = 0;         // не раніше (мс, QDateTime); 0 — без автоповтору (429)

static QString t(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static QString state_text(LinkState state)
{
	QString channel = QString::fromStdString(channel_name());
	switch (state) {
	case LinkState::Connected:
		return channel.isEmpty() ? t("State.Connected") : "@" + channel + " · " + t("State.Connected");
	case LinkState::Connecting:
		return t("State.Connecting");
	case LinkState::Offline:
		return t("State.Offline");
	case LinkState::Rejected:
		return t("State.Rejected");
	case LinkState::Replaced:
		return t("State.Replaced");
	default:
		return t("State.NoToken");
	}
}

// Кольори поточної теми OBS: пульт малюється ними й не вирізняється серед доків.
static DockTheme current_theme()
{
	QPalette p = QApplication::palette();
	auto hex = [&p](QPalette::ColorRole role) {
		QColor c = p.color(role);
		return hex_color(c.red(), c.green(), c.blue());
	};
	return {hex(QPalette::Window), hex(QPalette::Button),          hex(QPalette::WindowText),
		hex(QPalette::PlaceholderText), hex(QPalette::Mid), hex(QPalette::Highlight)};
}

static void show_note(const QString &text)
{
	if (!g_note)
		return;
	g_note->setText(text);
	g_note->setVisible(!text.isEmpty());
}

// Браузер — лише після FINISHED_LOADING: CEF піднімає obs-browser, і чекати на
// нього в post_load означало б затримати старт OBS.
static bool ensure_browser()
{
	if (g_browser)
		return true;
	if (!g_frontend_ready || !g_root || !g_layout || g_no_browser)
		return false;
	if (!g_cef) {
		g_cef = obs_browser_init_panel();
		// closeBrowser (його кличе вихід) з'явився у версії 2 — як у BrowserDock самого OBS.
		if (!g_cef || obs_browser_qcef_version() < 2) {
			g_cef = nullptr;
			g_no_browser = true;
			show_note(t("Dock.NoBrowser"));
			obs_log(LOG_WARNING, "obs-browser is unavailable: the dock stays without the control panel");
			return false;
		}
		g_cef->init_browser();
		g_cef->wait_for_browser_init();
		// Своє сховище cookies: сесія пульта не змішується з іншими браузерними
		// доками й не чіпає вхід у кабінет у звичайному браузері.
		g_cookies = g_cef->create_cookie_manager("ebot_dock", true);
	}
	g_browser = g_cef->create_widget(g_root, "about:blank", g_cookies);
	g_layout->addWidget(g_browser, 1);
	return true;
}

// Кожне відкриття — новий одноразовий код: сервер не пускає код двічі.
// manual — кнопка «Оновити пульт» чи нова тема: паузи автоповтору з нуля.
static void load_panel(bool manual = true)
{
	if (!ensure_browser())
		return;
	int gen = ++g_load_gen;
	DockTheme theme = current_theme(); // палітра — лише з UI-потоку
	g_loaded = true;
	g_load_failed = false;
	if (manual)
		g_retry_attempt = 0;
	show_note(t("Dock.Opening"));
	std::thread([gen, theme] {
		unsigned long status = 0;
		std::string code = dock_handoff_code(status);
		QMetaObject::invokeMethod(
			QCoreApplication::instance(),
			[gen, theme, code, status] {
				if (gen != g_load_gen || !g_browser)
					return;
				if (code.empty()) {
					// Повтор — з паузою 5 → 60 с, а після 429 лише вручну:
					// інакше док стукав двічі на секунду, і ліміт сервера на
					// адресу закривав пульт усім за тим самим NAT.
					g_load_failed = true;
					g_retry_at = dock_auto_retry(status)
							     ? QDateTime::currentMSecsSinceEpoch() +
								       dock_retry_delay_ms(g_retry_attempt++)
							     : 0;
					show_note(t("Dock.Failed"));
					return;
				}
				g_retry_attempt = 0;
				show_note("");
				g_browser->setURL(server_base() + dock_enter_path(code, theme));
			},
			Qt::QueuedConnection);
	}).detach();
}

// «Від'єднати» — пульт закривається і його сесія стирається з цього сховища.
static void unload_panel()
{
	g_loaded = false;
	g_load_failed = false;
	++g_load_gen;
	if (g_browser)
		g_browser->setURL("about:blank");
	if (g_cookies)
		g_cookies->DeleteCookies("", "");
	show_note("");
}

static const char *state_name(LinkState state)
{
	switch (state) {
	case LinkState::Connected:
		return "connected";
	case LinkState::Connecting:
		return "connecting";
	case LinkState::Offline:
		return "offline";
	case LinkState::Rejected:
		return "rejected (4001)";
	case LinkState::Replaced:
		return "replaced by another PC (4002)";
	default:
		return "not linked";
	}
}

// Для підтримки: версії, стан, причина останнього збою. Без токена й кодів.
static std::string diagnostics_text()
{
	RelayDiagnostics d = relay().diagnostics();
	std::string text = std::string("eBot OBS plugin ") + PLUGIN_VERSION + "\nOBS " + obs_get_version_string() +
			   "\nServer: " + server_base() + "\nChannel: " + (channel_name().empty() ? "-" : channel_name()) +
			   "\nState: " + state_name(d.state) + "\nLast error: ";
	text += d.last_error.empty() ? "-" : d.last_error + " (" + std::to_string(d.error_age_sec) + " s ago)";
	text += "\nDock: ";
	text += g_no_browser ? "no obs-browser" : (g_load_failed ? "handoff failed" : (g_loaded ? "loaded" : "idle"));
	return text + "\n";
}

void create_dock()
{
	auto *root = new QWidget();
	auto *layout = new QVBoxLayout(root);
	auto *status = new QLabel();
	status->setWordWrap(true);
	auto *code = new QLabel();
	QFont big = code->font();
	big.setPointSizeF(big.pointSizeF() > 0 ? big.pointSizeF() * 2 : 20);
	big.setBold(true);
	code->setFont(big);
	code->setTextInteractionFlags(Qt::TextSelectableByMouse);
	auto *hint = new QLabel(t("Pair.Hint"));
	hint->setWordWrap(true);
	auto *connect_btn = new QPushButton(t("Button.Connect"));
	auto *cancel_btn = new QPushButton(t("Button.Cancel"));
	auto *update = new QLabel();
	update->setOpenExternalLinks(true);
	auto *note = new QLabel();
	note->setWordWrap(true);
	note->setVisible(false);

	// Прив'язаний канал: одна компактна смуга — пульту нижче потрібне місце.
	// Оновити й від'єднати — квадратні кнопки з іконками самої теми OBS (класи
	// icon-refresh / icon-close: тема малює їх сама, світла й темна). Гліфи
	// шрифту (↻ ✕) тема не мала — кнопки були порожні (11.10.2026). Тема без
	// цих класів — стандартна іконка Qt.
	auto *linked_row = new QWidget();
	auto *row = new QHBoxLayout(linked_row);
	row->setContentsMargins(0, 0, 0, 0);
	auto icon_button = [](const char *obs_class, QStyle::StandardPixmap fallback, const char *tip_key) {
		auto *b = new QPushButton();
		b->setProperty("class", obs_class);
		b->setToolTip(t(tip_key));
		b->setAccessibleName(t(tip_key));
		b->ensurePolished();
		if (b->icon().isNull())
			b->setIcon(b->style()->standardIcon(fallback));
		b->setFixedWidth(b->sizeHint().height());
		return b;
	};
	auto *cabinet_btn = new QPushButton(t("Button.Cabinet"));
	auto *refresh_btn = icon_button("icon-refresh", QStyle::SP_BrowserReload, "Button.RefreshDock");
	// Класу «копіювати» тема OBS не має — стандартна іконка Qt.
	auto *diag_btn = icon_button("", QStyle::SP_MessageBoxInformation, "Button.CopyDiagnostics");
	auto *disconnect_btn = icon_button("icon-close", QStyle::SP_DialogCloseButton, "Button.Disconnect");
	row->addWidget(cabinet_btn, 1);
	row->addWidget(refresh_btn);
	row->addWidget(diag_btn);
	row->addWidget(disconnect_btn);
	// 4002: канал перейшов на інший ПК — повертаємо лише кнопкою.
	auto *here_btn = new QPushButton(t("Button.ConnectHere"));

	for (QWidget *w : {static_cast<QWidget *>(status), static_cast<QWidget *>(code), static_cast<QWidget *>(hint),
			   static_cast<QWidget *>(connect_btn), static_cast<QWidget *>(cancel_btn),
			   static_cast<QWidget *>(here_btn), linked_row,
			   static_cast<QWidget *>(update), static_cast<QWidget *>(note)})
		layout->addWidget(w);
	// Поки пульту нема (не прив'язано, OBS без браузера) — кнопки притиснуті догори.
	auto *filler = new QWidget();
	layout->addWidget(filler, 1);

	g_root = root;
	g_layout = layout;
	g_note = note;

	// Одна функція оновлення: таймер підхоплює зміни з інших потоків, а клік
	// оновлює док одразу — інакше кнопка «Підключити канал» лишалась на
	// пів секунди, і подвійний клік відкривав дві прив'язки.
	auto refresh = [=] {
		PairStatus pair = pair_status();
		LinkState state = relay().state();
		bool linked = state != LinkState::NoToken;
		status->setText(pair.error.empty() ? state_text(state) : QString::fromStdString(pair.error));
		code->setText(QString::fromStdString(pair.user_code));
		code->setVisible(!pair.user_code.empty());
		hint->setVisible(!pair.user_code.empty());
		connect_btn->setVisible(!pair.active && (!linked || state == LinkState::Rejected));
		here_btn->setVisible(!pair.active && state == LinkState::Replaced);
		cancel_btn->setVisible(pair.active);
		linked_row->setVisible(linked && !pair.active);
		refresh_btn->setVisible(!g_no_browser);
		QString url = QString::fromStdString(update_url());
		update->setVisible(!url.isEmpty());
		update->setText(QString("<a href=\"%1\">%2</a>").arg(url.toHtmlEscaped(), t("Update.Available")));

		bool retry_due = g_load_failed && g_retry_at && QDateTime::currentMSecsSinceEpoch() >= g_retry_at;
		if (linked && g_frontend_ready && !g_loaded)
			load_panel();
		else if (linked && retry_due && state == LinkState::Connected)
			load_panel(false);
		else if (!linked && g_loaded)
			unload_panel();
		bool panel = linked && g_browser;
		if (g_browser)
			g_browser->setVisible(panel);
		filler->setVisible(!panel);
	};

	QObject::connect(connect_btn, &QPushButton::clicked, [=] {
		pair_begin();
		refresh();
	});
	QObject::connect(cancel_btn, &QPushButton::clicked, [=] {
		pair_cancel();
		refresh();
	});
	QObject::connect(refresh_btn, &QPushButton::clicked, [] { load_panel(); });
	QObject::connect(here_btn, &QPushButton::clicked, [=] {
		relay().reconnect();
		refresh();
	});
	QObject::connect(diag_btn, &QPushButton::clicked, [] {
		QGuiApplication::clipboard()->setText(QString::fromStdString(diagnostics_text()));
		show_note(t("Dock.DiagnosticsCopied"));
		QTimer::singleShot(3000, [] {
			if (g_note && g_note->text() == t("Dock.DiagnosticsCopied"))
				show_note("");
		});
	});
	QObject::connect(cabinet_btn, &QPushButton::clicked,
			 [] { QDesktopServices::openUrl(QUrl("https://ebot.live/widgets?tab=obs")); });
	QObject::connect(disconnect_btn, &QPushButton::clicked, [=] {
		// Маленький хрестик легко зачепити, а повернути — лише новою прив'язкою.
		if (QMessageBox::question(root, t("Disconnect.ConfirmTitle"), t("Disconnect.ConfirmText")) !=
		    QMessageBox::Yes)
			return;
		clear_config();
		set_channel_name("");
		relay().set_token("");
		refresh();
	});

	auto *timer = new QTimer(root);
	QObject::connect(timer, &QTimer::timeout, root, refresh);
	refresh(); // без цього на старті на мить блимали всі кнопки разом
	timer->start(500);

	// Перевірка оновлень раз на добу: на старті вона вже йде (post_load), а OBS
	// часто працює днями — нову версію інакше видно лише після перезапуску.
	auto *update_timer = new QTimer(root);
	QObject::connect(update_timer, &QTimer::timeout, root, [] { check_update_async(); });
	update_timer->start(UPDATE_CHECK_INTERVAL_MS);

	obs_frontend_add_dock_by_id("ebot-dock", obs_module_text("Dock.Title"), root);
}

void dock_frontend_ready()
{
	g_frontend_ready = true; // таймер дока сам відкриє пульт, якщо канал прив'язано
}

void dock_theme_changed()
{
	if (g_loaded)
		load_panel();
}

void dock_shutdown()
{
	// Порядок — як у самого OBS (BrowserDock::closeEvent, DestroyPanelCookieManager,
	// delete cef): закрити й знищити браузер, скинути й знищити сховище cookies,
	// знищити QCef. Живе сховище cookies на момент вивантаження obs-browser — це
	// CHECK у CEF і падіння OBS при закритті (crash 11.10.2026, libcef 80000003).
	++g_load_gen;
	g_frontend_ready = false;
	if (g_browser) {
		g_browser->closeBrowser();
		delete g_browser.data();
	}
	if (g_cookies) {
		g_cookies->FlushStore();
		delete g_cookies;
		g_cookies = nullptr;
	}
	delete g_cef;
	g_cef = nullptr;
}

} // namespace ebot
