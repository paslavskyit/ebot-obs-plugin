// Док «єБот»: стан, «Підключити канал», код, «Відкрити кабінет», «Від'єднати».
// Стан читається таймером раз на 0,5 с — жодних сигналів між потоками.
#include "dock.hpp"
#include "config.hpp"
#include "pairing.hpp"
#include "relay_client.hpp"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <QDesktopServices>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

namespace ebot {

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
	default:
		return t("State.NoToken");
	}
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
	auto *cabinet_btn = new QPushButton(t("Button.Cabinet"));
	auto *disconnect_btn = new QPushButton(t("Button.Disconnect"));
	auto *update = new QLabel();
	update->setOpenExternalLinks(true);
	for (QWidget *w : {static_cast<QWidget *>(status), static_cast<QWidget *>(code), static_cast<QWidget *>(hint),
			   static_cast<QWidget *>(connect_btn), static_cast<QWidget *>(cancel_btn),
			   static_cast<QWidget *>(cabinet_btn), static_cast<QWidget *>(disconnect_btn),
			   static_cast<QWidget *>(update)})
		layout->addWidget(w);
	layout->addStretch();

	QObject::connect(connect_btn, &QPushButton::clicked, [] { pair_begin(); });
	QObject::connect(cancel_btn, &QPushButton::clicked, [] { pair_cancel(); });
	QObject::connect(cabinet_btn, &QPushButton::clicked,
			 [] { QDesktopServices::openUrl(QUrl("https://ebot.live/widgets?tab=obs")); });
	QObject::connect(disconnect_btn, &QPushButton::clicked, [] {
		clear_config();
		set_channel_name("");
		relay().set_token("");
	});

	auto *timer = new QTimer(root);
	QObject::connect(timer, &QTimer::timeout, root, [=] {
		PairStatus pair = pair_status();
		LinkState state = relay().state();
		bool linked = state != LinkState::NoToken;
		status->setText(pair.error.empty() ? state_text(state) : QString::fromStdString(pair.error));
		code->setText(QString::fromStdString(pair.user_code));
		code->setVisible(!pair.user_code.empty());
		hint->setVisible(!pair.user_code.empty());
		connect_btn->setVisible(!pair.active && (!linked || state == LinkState::Rejected));
		cancel_btn->setVisible(pair.active);
		cabinet_btn->setVisible(linked && !pair.active);
		disconnect_btn->setVisible(linked && !pair.active);
		std::string url = update_url();
		update->setVisible(!url.empty());
		update->setText(QString("<a href=\"%1\">%2</a>").arg(QString::fromStdString(url), t("Update.Available")));
	});
	timer->start(500);

	obs_frontend_add_dock_by_id("ebot-dock", obs_module_text("Dock.Title"), root);
}

} // namespace ebot
