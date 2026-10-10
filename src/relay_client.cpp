#include "relay_client.hpp"
#include "core.hpp"
#include "http.hpp"
#include "obs_bridge.hpp"
#include "ws.hpp"
#include <obs.hpp>
#include <plugin-support.h>
#include <windows.h>
#include <shlobj.h>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace ebot {

static long long now_ms()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

RelayClient &relay()
{
	static RelayClient instance;
	return instance;
}

void RelayClient::start()
{
	stop_ = false;
	thread_ = std::thread([this] { run(); });
}

void RelayClient::request_stop()
{
	stop_ = true;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (current_)
			current_->abort();
	}
	cv_.notify_all();
}

void RelayClient::stop()
{
	request_stop();
	http_abort_all(); // бекап, що качається, обривається одразу, а не за 120 с
	if (thread_.joinable())
		thread_.join();
	std::vector<std::thread> backups;
	{
		std::lock_guard<std::mutex> lock(backups_mutex_);
		backups.swap(backups_);
	}
	for (auto &t : backups)
		if (t.joinable())
			t.join();
}

void RelayClient::set_token(std::string token)
{
	std::lock_guard<std::mutex> lock(mutex_);
	token_ = std::move(token);
	if (current_)
		current_->abort(); // перепідключитись з новим токеном
	state_ = token_.empty() ? LinkState::NoToken : LinkState::Connecting;
	cv_.notify_all();
}

void RelayClient::sleep_interruptible(int ms)
{
	std::unique_lock<std::mutex> lock(mutex_);
	cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return stop_.load(); });
}

void RelayClient::run()
{
	while (!stop_) {
		std::string token;
		WsConnection ws;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			// Rejected: чекаємо нового токена, а не довбемо сервер кожні 5 с.
			cv_.wait(lock, [this] { return stop_ || (!token_.empty() && state_ != LinkState::Rejected); });
			if (stop_)
				break;
			token = token_;
			current_ = &ws; // stop()/set_token() можуть обірвати навіть підключення
			queue_.clear();
		}
		state_ = LinkState::Connecting;
		unsigned long http_status = 0;
		if (ws.open(server_base(), "/api/ws/obs-relay?token=" + token, http_status)) {
			serve(ws);
		} else if (!stop_) {
			obs_log(LOG_WARNING, "eBot handshake failed (HTTP %lu)", http_status);
			if (classify_handshake(http_status) == CloseKind::Rejected)
				state_ = LinkState::Rejected; // перевипущений токен: чекаємо нової прив'язки
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			current_ = nullptr;
		}
		if (state_ != LinkState::Rejected && state_ != LinkState::NoToken)
			state_ = LinkState::Offline;
		sleep_interruptible(RECONNECT_DELAY_MS);
	}
}

void RelayClient::serve(WsConnection &ws)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		last_scene_.clear(); // ресинк сцени на кожне (пере)підключення
	}
	state_ = LinkState::Connected;
	last_rx_ms_ = now_ms();
	live_ = true;
	obs_log(LOG_INFO, "connected to eBot");
	std::thread writer_thread([this, &ws] { writer(ws); });
	push_scene(current_scene_name());

	std::string frame;
	unsigned short close_code = 0;
	while (!stop_ && ws.receive_text(frame, close_code)) {
		last_rx_ms_ = now_ms();
		if (frame != "pong")
			handle(frame);
	}
	if (classify_close(close_code) == CloseKind::Rejected) {
		obs_log(LOG_WARNING, "eBot rejected the relay token (4001)");
		state_ = LinkState::Rejected;
	}
	ws.abort();
	live_ = false;
	cv_.notify_all();
	writer_thread.join();
	obs_log(LOG_INFO, "disconnected from eBot");
}

// Єдиний, хто пише в сокет: кадри з черги, ping раз на 25 с, сторож тиші.
void RelayClient::writer(WsConnection &ws)
{
	long long next_ping = now_ms() + PING_INTERVAL_MS;
	while (live_) {
		std::string frame;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait_for(lock, std::chrono::milliseconds(1000),
				     [this] { return stop_ || !live_ || !queue_.empty(); });
			if (stop_ || !live_)
				return;
			if (!queue_.empty()) {
				frame = std::move(queue_.front());
				queue_.pop_front();
			}
		}
		long long now = now_ms();
		if (now - last_rx_ms_ > SILENCE_LIMIT_MS) {
			obs_log(LOG_WARNING, "no frames from eBot for %d s, reconnecting", SILENCE_LIMIT_MS / 1000);
			ws.abort();
			return;
		}
		if (frame.empty() && now >= next_ping) {
			frame = "ping";
			next_ping = now + PING_INTERVAL_MS;
		}
		if (!frame.empty() && !ws.send_text(frame)) {
			ws.abort();
			return;
		}
	}
}

void RelayClient::enqueue(std::string frame)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!live_)
		return;
	queue_.push_back(std::move(frame));
	cv_.notify_all();
}

// UI-потік OBS: лише кладе кадр у чергу, нічого не чекає.
void RelayClient::push_scene(const std::string &scene)
{
	if (scene.empty())
		return;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!live_ || scene == last_scene_)
			return;
		last_scene_ = scene;
	}
	enqueue("{\"type\":\"scene_changed\",\"scene\":" + json_quote(scene) + "}");
}

static fs::path backup_dir()
{
	PWSTR documents = nullptr;
	fs::path dir;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)))
		dir = fs::path(documents) / L"єБот" / L"backups"; // «єБот»
	CoTaskMemFree(documents);
	if (dir.empty())
		throw std::runtime_error("Не знайдено теку «Документи»");
	fs::create_directories(dir);
	return dir;
}

static int prune_backups(const fs::path &dir)
{
	int removed = 0;
	auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * BACKUP_KEEP_DAYS);
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator(dir, ec)) {
		std::string name = entry.path().filename().string();
		if (name.rfind("ebot_backup_", 0) != 0 || entry.path().extension() != ".zip")
			continue;
		if (entry.last_write_time(ec) < cutoff && fs::remove(entry.path(), ec))
			++removed;
	}
	return removed;
}

// Той самий контракт, що _do_backup в obs_relay.py; помилки — винятком.
static std::string do_backup(const std::string &trigger, const std::string &kinds, const std::string &token)
{
	for (char ch : kinds)
		if (!(std::isalnum((unsigned char)ch) || ch == '_' || ch == ','))
			throw std::runtime_error("bad kinds");
	HttpResponse resp = http_request(server_base(), L"GET",
					 "/api/backup/relay-download?token=" + token + "&kinds=" + kinds, "",
					 BACKUP_DOWNLOAD_TIMEOUT_MS);
	if (resp.status != 200)
		throw std::runtime_error("Сервер відповів " + std::to_string(resp.status) + ": " +
					 resp.body.substr(0, 200));
	if (resp.body.rfind("PK", 0) != 0)
		throw std::runtime_error("Це не ZIP: " + resp.body.substr(0, 200));
	std::time_t t = std::time(nullptr);
	std::tm local{};
	localtime_s(&local, &t);
	fs::path dir = backup_dir();
	std::string filename = backup_filename(trigger.empty() ? "manual" : trigger, local);
	// Спершу .part, перейменування — лише після повного запису: обірваний чи
	// недописаний файл не лягає поруч із справжніми архівами як «успіх».
	fs::path part = dir / (filename + ".part");
	{
		std::ofstream out(part, std::ios::binary);
		out.write(resp.body.data(), (std::streamsize)resp.body.size());
		out.close();
		if (!out.good()) {
			std::error_code ec;
			fs::remove(part, ec);
			throw std::runtime_error("Не вдалося записати архів у «Документи/єБот/backups»");
		}
	}
	std::error_code ec;
	if (fs::file_size(part, ec) != resp.body.size() || ec) {
		fs::remove(part, ec);
		throw std::runtime_error("Архів записався не повністю");
	}
	fs::rename(part, dir / filename, ec);
	if (ec)
		throw std::runtime_error("Не вдалося зберегти архів: " + ec.message());
	int removed = prune_backups(dir);
	obs_log(LOG_INFO, "backup saved: %s (%zu bytes), pruned %d", filename.c_str(), resp.body.size(), removed);
	return "{\"success\":true,\"filename\":" + json_quote(filename) +
	       ",\"size_bytes\":" + std::to_string(resp.body.size()) + "}";
}

static std::string reply_ok(const std::string &id, const std::string &result_json)
{
	return "{\"id\":" + json_quote(id) + ",\"result\":" + result_json + "}";
}

static std::string reply_error(const std::string &id, const std::string &error)
{
	return "{\"id\":" + json_quote(id) + ",\"error\":" + json_quote(error) + "}";
}

void RelayClient::handle(const std::string &raw)
{
	OBSDataAutoRelease msg = obs_data_create_from_json(raw.c_str());
	if (!msg)
		return;
	std::string id = obs_data_get_string(msg, "id");
	std::string method = obs_data_get_string(msg, "method");
	if (id.empty() || method.empty())
		return;
	obs_data_t *raw_params = obs_data_get_obj(msg, "params");
	OBSDataAutoRelease params(raw_params ? raw_params : obs_data_create());

	if (method == "do_backup") {
		// Довгий (до 120 с) — окремим потоком, щоб не тримати інші команди.
		std::string trigger = obs_data_get_string(params, "trigger");
		std::string kinds = obs_data_get_string(params, "kinds");
		std::string token;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			token = token_;
		}
		std::lock_guard<std::mutex> lock(backups_mutex_);
		backups_.emplace_back([this, id, trigger, kinds, token] {
			try {
				enqueue(reply_ok(id, do_backup(trigger, kinds, token)));
			} catch (const std::exception &e) {
				enqueue(reply_error(id, e.what()));
			}
		});
		return;
	}
	if (stop_)
		return; // OBS закривається: не смикаємо UI-потік запитами
	try {
		enqueue(reply_ok(id, dispatch(method, params)));
	} catch (const std::exception &e) {
		enqueue(reply_error(id, e.what()));
	}
}

} // namespace ebot
