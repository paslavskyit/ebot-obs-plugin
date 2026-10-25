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
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
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

// Стан, що потребує уваги, — ще й кольором теми OBS (класи text-warning і
// text-danger тем 30+), а не лише словами: «підключено» й «немає зв'язку»
// виглядали однаково. Тема без цих класів просто лишає колір тексту.
static const char *state_class(LinkState state, bool pair_error)
{
	if (pair_error || state == LinkState::Rejected)
		return "text-danger";
	if (state == LinkState::Offline || state == LinkState::Replaced)
		return "text-warning";
	return "";
}

// Розмір шрифту OBS (Налаштування → Вигляд) задає тема стилем, тож
// QApplication::font() його не знає — питаємо полірований віджет дока.
static int obs_font_px()
{
	if (!g_note)
		return 0;
	g_note->ensurePolished();
	QFont f = g_note->font();
	return f.pixelSize() > 0 ? f.pixelSize() : qRound(f.pointSizeF() * 96.0 / 72.0);
}

// Кольори й шрифт поточної теми OBS: пульт малюється ними й не вирізняється серед доків.
static DockTheme current_theme()
{
	QPalette p = QApplication::palette();
	auto hex = [&p](QPalette::ColorRole role) {
		QColor c = p.color(role);
		return hex_color(c.red(), c.green(), c.blue());
	};
	DockTheme theme{hex(QPalette::Window),          hex(QPalette::Button), hex(QPalette::WindowText),
			hex(QPalette::PlaceholderText), hex(QPalette::Mid),    hex(QPalette::Highlight)};
	theme.font_px = obs_font_px();
	return theme;
}

// Підказка під смугою; rich — текст з посиланням (уже екранований).
static void show_note(const QString &text, bool rich = false)
{
	if (!g_note)
		return;
	g_note->setText(rich ? text : text.toHtmlEscaped());
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
					// Дія тут же, а не «натисніть ↻»: іконка без підпису, і назви
					// «Оновити пульт» на екрані не видно (аудит 25.10.2026).
					show_note(t("Dock.Failed").toHtmlEscaped() +
							  QString(" <a href=\"retry\">%1</a>").arg(t("Dock.Retry").toHtmlEscaped()),
						  true);
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
	// Пульт (сторінка) — врівень із краями доку, як браузерні доки самого OBS:
	// у пульта свої поля, і поля дока навколо давали подвійну рамку. Поля має
	// лише смуга стану й кнопок над ним.
	auto *root = new QWidget();
	auto *layout = new QVBoxLayout(root);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	auto *strip = new QWidget();
	auto *strip_layout = new QVBoxLayout(strip);
	strip_layout->setContentsMargins(8, 6, 8, 6);
	strip_layout->setSpacing(6);
	auto *status = new QLabel();
	status->setWordWrap(true);
	// Код читають з екрана і звіряють із сайтом: моноширинний, з проміжками
	// між знаками, щоб 0/O і 1/I не плутались. Системний моноширинний Windows —
	// тонкий Courier New із засічками; Cascadia Mono / Consolas читаються краще.
	auto *code = new QLabel();
	QFont big = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	big.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"), big.family()});
	big.setPointSizeF(code->font().pointSizeF() > 0 ? code->font().pointSizeF() * 2 : 20);
	big.setBold(true);
	big.setLetterSpacing(QFont::AbsoluteSpacing, 3);
	code->setFont(big);
	code->setAlignment(Qt::AlignCenter);
	code->setTextInteractionFlags(Qt::TextSelectableByMouse);
	// Посилання — на випадок, коли браузер не відкрився сам (немає браузера за
	// замовчуванням, вкладка сховалась за OBS): без нього код нікуди було ввести.
	auto *hint = new QLabel();
	hint->setWordWrap(true);
	hint->setTextFormat(Qt::RichText);
	hint->setOpenExternalLinks(true);
	auto *connect_btn = new QPushButton(t("Button.Connect"));
	auto *cancel_btn = new QPushButton(t("Button.Cancel"));
	auto *update = new QLabel();
	update->setOpenExternalLinks(true);
	auto *note = new QLabel();
	note->setWordWrap(true);
	note->setTextFormat(Qt::RichText);
	note->setVisible(false);
	QObject::connect(note, &QLabel::linkActivated, [](const QString &link) {
		if (link == "retry")
			load_panel();
	});

	// Прив'язаний канал: одна компактна смуга — пульту нижче потрібне місце.
	// Оновити й «⋯» — квадратні кнопки з іконками самої теми OBS (класи
	// icon-refresh / icon-dots-vert: тема малює їх сама, світла й темна). Гліфи
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
	// Рідкісне — під «⋯» (аудит 25.10.2026): діагностика потрібна раз на місяць, а
	// її ⓘ означало «інформація», не «копіювати»; ✕ «Від'єднати» поруч з ↻
	// читався як «закрити док».
	auto *more_btn = icon_button("icon-dots-vert", QStyle::SP_ToolBarHorizontalExtensionButton, "Button.More");
	auto *more_menu = new QMenu(more_btn);
	QAction *diag_action = more_menu->addAction(t("Button.CopyDiagnostics"));
	more_menu->addSeparator();
	// «…» — далі ще питання; так пише й OBS у своїх меню.
	QAction *disconnect_action = more_menu->addAction(t("Button.Disconnect") + QStringLiteral("…"));
	row->addWidget(cabinet_btn, 1);
	row->addWidget(refresh_btn);
	row->addWidget(more_btn);
	// 4002: канал перейшов на інший ПК — повертаємо лише кнопкою.
	auto *here_btn = new QPushButton(t("Button.ConnectHere"));

	for (QWidget *w : {static_cast<QWidget *>(status), static_cast<QWidget *>(code), static_cast<QWidget *>(hint),
			   static_cast<QWidget *>(connect_btn), static_cast<QWidget *>(cancel_btn),
			   static_cast<QWidget *>(here_btn), linked_row,
			   static_cast<QWidget *>(update), static_cast<QWidget *>(note)})
		strip_layout->addWidget(w);
	layout->addWidget(strip);
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
		QString cls = state_class(state, !pair.error.empty());
		if (status->property("class").toString() != cls) {
			status->setProperty("class", cls);
			status->style()->unpolish(status);
			status->style()->polish(status);
		}
		QString hint_text = t("Pair.Hint").toHtmlEscaped();
		if (!pair.verify_url.empty())
			hint_text += QString(" <a href=\"%1\">%2</a>")
					     .arg(QString::fromStdString(pair.verify_url).toHtmlEscaped(),
						  t("Pair.OpenAgain").toHtmlEscaped());
		// Той самий текст раз на 0,5 с не переписуємо: setText скидав би наведення на посилання.
		if (hint->text() != hint_text)
			hint->setText(hint_text);
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
	QObject::connect(more_btn, &QPushButton::clicked, [=] {
		// Правим краєм до кнопки: док часто стоїть біля краю вікна OBS.
		QPoint at(more_btn->width() - more_menu->sizeHint().width(), more_btn->height());
		more_menu->exec(more_btn->mapToGlobal(at));
	});
	QObject::connect(diag_action, &QAction::triggered, [] {
		QGuiApplication::clipboard()->setText(QString::fromStdString(diagnostics_text()));
		show_note(t("Dock.DiagnosticsCopied"));
		QTimer::singleShot(3000, [] {
			if (g_note && g_note->text() == t("Dock.DiagnosticsCopied"))
				show_note("");
		});
	});
	QObject::connect(cabinet_btn, &QPushButton::clicked,
			 [] { QDesktopServices::openUrl(QUrl("https://ebot.live/widgets?tab=obs")); });
	QObject::connect(disconnect_action, &QAction::triggered, [=] {
		// Повернути — лише новою прив'язкою, тож питаємо. Кнопки — з назвами дії,
		// Enter і Esc — «Скасувати»: QMessageBox::question ставив «Yes» за
		// замовчуванням і англійські Yes/No (OBS сам задає тексти через OBSMessageBox).
		QMessageBox box(QMessageBox::Question, t("Disconnect.ConfirmTitle"), t("Disconnect.ConfirmText"),
				QMessageBox::NoButton, root);
		QPushButton *confirm = box.addButton(t("Disconnect.Confirm"), QMessageBox::DestructiveRole);
		QPushButton *cancel = box.addButton(t("Button.Cancel"), QMessageBox::RejectRole);
		box.setDefaultButton(cancel);
		box.setEscapeButton(cancel);
		box.exec();
		if (box.clickedButton() != confirm)
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
