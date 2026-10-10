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
#include <random>
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
	heavy_thread_ = std::thread([this] { heavy_loop(); });
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
	{
		std::lock_guard<std::mutex> lock(heavy_mutex_);
	}
	heavy_cv_.notify_all();
}

void RelayClient::stop()
{
	request_stop();
	http_abort_all(); // бекап, що качається, обривається одразу, а не за 120 с
	if (thread_.joinable())
		thread_.join();
	if (heavy_thread_.joinable())
		heavy_thread_.join();
	std::thread backup;
	{
		std::lock_guard<std::mutex> lock(backup_mutex_);
		backup.swap(backup_thread_);
	}
	if (backup.joinable())
		backup.join();
}

void RelayClient::set_token(std::string token)
{
	std::lock_guard<std::mutex> lock(mutex_);
	token_ = std::move(token);
	++token_gen_;
	if (current_)
		current_->abort(); // перепідключитись з новим токеном
	state_ = token_.empty() ? LinkState::NoToken : LinkState::Connecting;
	cv_.notify_all();
}

void RelayClient::sleep_interruptible(int ms, int token_gen)
{
	std::unique_lock<std::mutex> lock(mutex_);
	cv_.wait_for(lock, std::chrono::milliseconds(ms),
		     [this, token_gen] { return stop_.load() || token_gen_ != token_gen; });
}

// Без мережі рукостискання падає щоразу: у лог — лише зміна причини, а не рядок
// кожні кілька секунд у журналі, який стример надсилає підтримці.
void RelayClient::log_handshake_failure(unsigned long http_status)
{
	if (http_status == last_logged_status_)
		return;
	last_logged_status_ = http_status;
	obs_log(LOG_WARNING, "eBot handshake failed (HTTP %lu)", http_status);
}

void RelayClient::run()
{
	std::mt19937 rng{std::random_device{}()};
	std::uniform_real_distribution<double> jitter(0.0, 1.0);
	int attempt = 0;          // невдалі підключення поспіль
	int rejected_attempt = 0; // «токен не прийнято» поспіль
	int last_gen = -1;
	while (!stop_) {
		std::string token;
		int gen = 0;
		WsConnection ws;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [this] { return stop_ || !token_.empty(); });
			if (stop_)
				break;
			token = token_;
			gen = token_gen_;
			current_ = &ws; // stop()/set_token() можуть обірвати навіть підключення
			queue_.clear();
			state_ = LinkState::Connecting; // під тим самим замком, що й токен
		}
		if (gen != last_gen) { // новий токен — паузи з нуля
			attempt = 0;
			rejected_attempt = 0;
			last_gen = gen;
		}
		long long started = now_ms();
		unsigned long http_status = 0;
		bool rejected = false;
		if (ws.open(server_base(), "/api/ws/obs-relay", token, http_status)) {
			rejected = serve(ws);
		} else if (!stop_) {
			log_handshake_failure(http_status);
			rejected = classify_handshake(http_status) == CloseKind::Rejected;
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			current_ = nullptr;
			if (gen == token_gen_) // токен не міняли, поки підключались
				state_ = rejected ? LinkState::Rejected : LinkState::Offline;
		}
		if (now_ms() - started >= STABLE_CONNECTION_MS)
			attempt = 0;
		// «Токен не прийнято» теж пробуємо знову, лише рідко: це міг бути збій
		// сервера, і без повтору канал лишався без OBS до ручної переприв'язки.
		int delay = rejected ? rejected_retry_delay_ms(rejected_attempt++, jitter(rng))
				     : reconnect_delay_ms(attempt++, jitter(rng));
		if (!rejected)
			rejected_attempt = 0;
		sleep_interruptible(delay, gen);
	}
}

bool RelayClient::serve(WsConnection &ws)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		last_scene_.clear(); // ресинк сцени на кожне (пере)підключення
		live_ = true;
		if (!token_.empty())
			state_ = LinkState::Connected;
	}
	last_rx_ms_ = now_ms();
	last_logged_status_ = ULONG_MAX; // наступний збій рукостискання — знову в лог
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
	bool rejected = classify_close(close_code) == CloseKind::Rejected;
	if (rejected)
		obs_log(LOG_WARNING, "eBot rejected the relay token (4001)");
	ws.abort();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		live_ = false;
	}
	cv_.notify_all();
	writer_thread.join();
	obs_log(LOG_INFO, "disconnected from eBot");
	return rejected;
}

// Єдиний, хто пише в сокет: кадри з черги, ping, сторож тиші.
void RelayClient::writer(WsConnection &ws)
{
	long long next_ping = now_ms() + PING_INTERVAL_MS;
	for (;;) {
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

void RelayClient::post_heavy(std::function<void()> job)
{
	{
		std::lock_guard<std::mutex> lock(heavy_mutex_);
		heavy_jobs_.push_back(std::move(job));
	}
	heavy_cv_.notify_one();
}

void RelayClient::heavy_loop()
{
	for (;;) {
		std::function<void()> job;
		{
			std::unique_lock<std::mutex> lock(heavy_mutex_);
			heavy_cv_.wait(lock, [this] { return stop_.load() || !heavy_jobs_.empty(); });
			if (stop_)
				return; // OBS закривається: черга не доробляється
			job = std::move(heavy_jobs_.front());
			heavy_jobs_.pop_front();
		}
		job();
	}
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
	// wstring і increment(ec): кирилиця в шляху та збій читання теки після
	// успішного збереження не мають перетворити бекап на помилку.
	for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
		const fs::path &file = it->path();
		if (file.filename().wstring().rfind(L"ebot_backup_", 0) != 0 || file.extension() != L".zip")
			continue;
		std::error_code file_ec;
		auto written = fs::last_write_time(file, file_ec);
		if (!file_ec && written < cutoff && fs::remove(file, file_ec))
			++removed;
	}
	return removed;
}

static bool looks_like_zip(const fs::path &file)
{
	std::ifstream in(file, std::ios::binary);
	char magic[2] = {};
	return in.read(magic, 2) && magic[0] == 'P' && magic[1] == 'K';
}

// Той самий контракт, що _do_backup в obs_relay.py; помилки — винятком.
static std::string do_backup(const std::string &trigger, const std::string &kinds, const std::string &token)
{
	for (char ch : kinds)
		if (!(std::isalnum((unsigned char)ch) || ch == '_' || ch == ','))
			throw std::runtime_error("bad kinds");
	std::time_t t = std::time(nullptr);
	std::tm local{};
	localtime_s(&local, &t);
	fs::path dir = backup_dir();
	std::string filename = backup_filename(trigger.empty() ? "manual" : trigger, local);
	// Спершу .part, перейменування — лише після повного запису: обірваний чи
	// недописаний файл не лягає поруч із справжніми архівами як «успіх».
	fs::path part = dir / (filename + ".part");
	DownloadResult resp = http_download(server_base(), "/api/backup/relay-download?kinds=" + kinds, token, part,
					    BACKUP_DOWNLOAD_TIMEOUT_MS);
	std::error_code ec;
	if (resp.status != 200) {
		fs::remove(part, ec);
		if (resp.write_failed)
			throw std::runtime_error("Не вдалося записати архів у «Документи/єБот/backups»");
		if (resp.status == 0)
			throw std::runtime_error("Архів не завантажився: мережа обірвалась");
		throw std::runtime_error("Сервер відповів " + std::to_string(resp.status) + ": " +
					 resp.error_body.substr(0, 200));
	}
	if (!looks_like_zip(part)) {
		fs::remove(part, ec);
		throw std::runtime_error("Сервер віддав не ZIP");
	}
	fs::rename(part, dir / filename, ec);
	if (ec) {
		std::string reason = ec.message();
		fs::remove(part, ec);
		throw std::runtime_error("Не вдалося зберегти архів: " + reason);
	}
	int removed = prune_backups(dir);
	obs_log(LOG_INFO, "backup saved: %s (%zu bytes), pruned %d", filename.c_str(), resp.bytes, removed);
	return "{\"success\":true,\"filename\":" + json_quote(filename) +
	       ",\"size_bytes\":" + std::to_string(resp.bytes) + "}";
}

static std::string reply_ok(const std::string &id, const std::string &result_json)
{
	return "{\"id\":" + json_quote(id) + ",\"result\":" + result_json + "}";
}

static std::string reply_error(const std::string &id, const std::string &error)
{
	return "{\"id\":" + json_quote(id) + ",\"error\":" + json_quote(error) + "}";
}

void RelayClient::start_backup(const std::string &id, const std::string &trigger, const std::string &kinds)
{
	if (backup_running_.exchange(true)) {
		enqueue(reply_error(id, "Бекап уже завантажується"));
		return;
	}
	std::string token;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		token = token_;
	}
	std::lock_guard<std::mutex> lock(backup_mutex_);
	if (backup_thread_.joinable())
		backup_thread_.join(); // попередній уже закінчився (backup_running_ був false)
	// Довгий (до 120 с) — окремим потоком, щоб не тримати інші команди.
	backup_thread_ = std::thread([this, id, trigger, kinds, token] {
		try {
			enqueue(reply_ok(id, do_backup(trigger, kinds, token)));
		} catch (const std::exception &e) {
			enqueue(reply_error(id, e.what()));
		}
		backup_running_ = false;
	});
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
		start_backup(id, obs_data_get_string(params, "trigger"), obs_data_get_string(params, "kinds"));
		return;
	}
	if (stop_)
		return; // OBS закривається: не смикаємо UI-потік запитами
	if (is_heavy_request(method, obs_data_get_string(params, "request"))) {
		const char *params_json = obs_data_get_json(params);
		std::string json = params_json ? params_json : "{}";
		post_heavy([this, id, method, json] {
			OBSDataAutoRelease job_params = obs_data_create_from_json(json.c_str());
			try {
				enqueue(reply_ok(id, dispatch(method, job_params)));
			} catch (const std::exception &e) {
				enqueue(reply_error(id, e.what()));
			}
		});
		return;
	}
	try {
		enqueue(reply_ok(id, dispatch(method, params)));
		// OBS шле SCENE_CHANGED лише наприкінці переходу (0,3 с fade, 1–3 с
		// стингер) — нова сцена вже поточна, тож кажемо серверу одразу. Подія
		// наприкінці переходу відсіється як повтор.
		if (method == "set_scene")
			push_scene(current_scene_name());
	} catch (const std::exception &e) {
		enqueue(reply_error(id, e.what()));
	}
}

} // namespace ebot
