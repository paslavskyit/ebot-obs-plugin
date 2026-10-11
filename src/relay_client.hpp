#pragma once
#include <atomic>
#include <climits>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace ebot {
enum class LinkState { NoToken, Connecting, Connected, Offline, Rejected };

class WsConnection;

// Труба єБот ↔ OBS: протокол obs_relay.py (JSON-RPC + ping/pong + пуш сцени).
// Свої правила: «токен не прийнято» (Rejected) теж пробуємо знову, але рідко;
// важкі запити — окремим потоком; тиша 35 с = мертве з'єднання.
class RelayClient {
public:
	void start();
	// Не блокує: просить зупинитись і обриває з'єднання (OBS_FRONTEND_EVENT_EXIT,
	// поки UI-потік ще крутиться — інакше join міг би чекати на запит до UI).
	void request_stop();
	// Блокує до кінця всіх потоків: обриває HTTP (бекап), чекає їх і релей.
	void stop();
	void set_token(std::string token); // "" — від'єднати
	void push_scene(const std::string &scene);
	LinkState state() const { return state_.load(); }

private:
	void run();
	bool serve(WsConnection &ws); // true — сервер не прийняв токен (4001)
	void writer(WsConnection &ws);
	void handle(const std::string &raw);
	void enqueue(std::string frame);
	// Чекає ms або нового токена (переприв'язка не чекає кінця паузи).
	void sleep_interruptible(int ms, int token_gen);
	void log_handshake_failure(unsigned long http_status);
	void post_heavy(std::function<void()> job);
	void heavy_loop();
	void start_backup(const std::string &id, const std::string &trigger, const std::string &kinds);

	std::thread thread_;
	std::atomic<bool> stop_{false};
	std::atomic<LinkState> state_{LinkState::NoToken};
	std::atomic<long long> last_rx_ms_{0};
	bool live_ = false;              // writer працює, поки true (під mutex_)
	std::mutex mutex_;               // token_, token_gen_, queue_, current_, last_scene_, live_
	std::condition_variable cv_;
	std::string token_;
	int token_gen_ = 0;              // +1 на кожен set_token
	std::string last_scene_;
	std::deque<std::string> queue_;
	WsConnection *current_ = nullptr;
	unsigned long last_logged_status_ = ULONG_MAX; // лише потік run()

	// ponytail: один потік на всі важкі запити — скріншот чекає за GetStats і
	// навпаки; окремий потік на кожен тип, якщо це колись стане помітно.
	std::thread heavy_thread_;
	std::mutex heavy_mutex_;
	std::condition_variable heavy_cv_;
	std::deque<std::function<void()>> heavy_jobs_;

	// Один бекап за раз: другий запит, поки качається перший, — помилка.
	std::mutex backup_mutex_;
	std::thread backup_thread_;
	std::atomic<bool> backup_running_{false};
};

RelayClient &relay();
} // namespace ebot
