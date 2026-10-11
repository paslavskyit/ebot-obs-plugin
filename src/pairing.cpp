// Прив'язка кнопкою (device flow, api/routers/obs_pair.py основного репо):
// start → браузер → poll раз на interval секунд, поки стример не натисне «Дозволити».
#include "pairing.hpp"
#include "config.hpp"
#include "core.hpp"
#include "http.hpp"
#include "relay_client.hpp"
#include <obs.hpp>
#include <obs-module.h>
#include <plugin-support.h>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QMetaObject>
#include <QString>
#include <QUrl>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace ebot {

static std::mutex g_mutex;
static PairStatus g_status;
static std::string g_channel;
static std::string g_update_url;
static std::atomic<int> g_generation{0}; // «Скасувати» = новий номер, старий потік виходить

// Браузер — з UI-потоку OBS через Qt: ShellExecuteW з робочого потоку без COM
// і без права на передній план міг не відкрити вкладку або сховати її за OBS.
static void open_in_browser(const std::string &url)
{
	QString target = QString::fromStdString(url);
	QMetaObject::invokeMethod(
		QCoreApplication::instance(), [target] { QDesktopServices::openUrl(QUrl(target)); },
		Qt::QueuedConnection);
}

static std::string computer_name()
{
	wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
	DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
	return GetComputerNameW(name, &size) ? narrow(std::wstring(name, size)) : "OBS";
}

static void set_status(int generation, PairStatus status)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (generation == g_generation)
		g_status = std::move(status);
}

static std::string body_with(const char *key, const std::string &value)
{
	return "{" + json_quote(key) + ":" + json_quote(value) + "}";
}

static void run_pairing(int generation)
{
	HttpResponse started = http_request(server_base(), L"POST", "/api/obs/pair/start",
					    body_with("device_name", computer_name()), 15000);
	OBSDataAutoRelease data = obs_data_create_from_json(started.body.c_str());
	if (started.status != 200 || !data) {
		set_status(generation, {"", obs_module_text(started.status == 429 ? "Pair.TooMany" : "Pair.NoServer"),
					false});
		return;
	}
	std::string device_code = obs_data_get_string(data, "device_code");
	std::string verify_url = obs_data_get_string(data, "verify_url");
	long long interval = (std::max)(1LL, (long long)obs_data_get_int(data, "interval"));
	long long expires = obs_data_get_int(data, "expires_in");
	set_status(generation, {obs_data_get_string(data, "user_code"), "", true});
	if (is_safe_verify_url(verify_url) && generation == g_generation)
		open_in_browser(verify_url);

	// Дедлайн за годинником, а не сумою пауз: без мережі кожен запит чекає ще
	// свої 15 с, і «10 хвилин» розтягувались на півтори години.
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(expires);
	std::string poll_body = body_with("device_code", device_code);
	while (std::chrono::steady_clock::now() < deadline && generation == g_generation) {
		Sleep((DWORD)(interval * 1000));
		if (generation != g_generation)
			return;
		HttpResponse resp = http_request(server_base(), L"POST", "/api/obs/pair/poll", poll_body, 15000);
		if (resp.status == 202 || resp.status == 0 || resp.status == 429)
			continue; // ще чекаємо / мережа блимнула / забагато запитів — не «код протух»
		if (resp.status != 200)
			break;
		if (generation != g_generation)
			return; // «Скасувати», поки летіла відповідь: токен не зберігаємо
		OBSDataAutoRelease done = obs_data_create_from_json(resp.body.c_str());
		std::string token = done ? obs_data_get_string(done, "token") : "";
		if (!is_safe_token(token))
			break;
		Saved saved{token, done ? obs_data_get_string(done, "channel") : ""};
		save_config(saved);
		set_channel_name(saved.channel);
		relay().set_token(token);
		set_status(generation, {"", "", false});
		obs_log(LOG_INFO, "paired with channel %s", saved.channel.c_str());
		return;
	}
	set_status(generation, {"", obs_module_text("Pair.Expired"), false});
}

void pair_begin()
{
	int generation = ++g_generation;
	set_status(generation, {"", "", true});
	std::thread(run_pairing, generation).detach();
}

void pair_cancel()
{
	int generation = ++g_generation;
	set_status(generation, {"", "", false});
}

PairStatus pair_status()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_status;
}

std::string channel_name()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_channel;
}

void set_channel_name(const std::string &channel)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_channel = channel;
}

std::string update_url()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_update_url;
}

// latest.json з останнього релізу публічного репо: {"version","url"}.
void check_update_async()
{
	std::thread([] {
		HttpResponse resp = http_request("https://github.com", L"GET",
						 "/paslavskyit/ebot-obs-plugin/releases/latest/download/latest.json", "",
						 15000);
		OBSDataAutoRelease data = obs_data_create_from_json(resp.body.c_str());
		if (resp.status != 200 || !data)
			return;
		std::string url = obs_data_get_string(data, "url");
		if (is_newer_version(obs_data_get_string(data, "version"), PLUGIN_VERSION) &&
		    url.rfind("https://github.com/paslavskyit/ebot-obs-plugin/", 0) == 0) {
			std::lock_guard<std::mutex> lock(g_mutex);
			g_update_url = url;
		}
	}).detach();
}

} // namespace ebot
