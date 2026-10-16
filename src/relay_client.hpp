#pragma once
#include <atomic>
#include <climits>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "core.hpp"

namespace ebot {
enum class LinkState { NoToken, Connecting, Connected, Offline, Rejected, Replaced };

// Для кнопки «Копіювати діагностику» дока: причина останнього збою — без токена.
struct RelayDiagnostics {
	LinkState state = LinkState::NoToken;
	std::string last_error;      // "" — збоїв ще не було
	long long error_age_sec = -1; // скільки секунд тому
};

class WsConnection;

// Труба єБот ↔ OBS: протокол obs_relay.py (JSON-RPC + ping/pong + пуші подій).
// Свої правила: «токен не прийнято» (Rejected) теж пробуємо знову, але рідко;
// «підключено з іншого ПК» (Replaced) — лише кнопкою; запити виконують два
// робочі потоки (звичайні й важкі), а потік читання лише розкладає кадри — тож
// запит, що застряг в OBS, не тримає ні ping, ні наступні кадри; тиша 35 с =
// мертве з'єднання.
class RelayClient {
public:
	void start();
	// Не блокує: просить зупинитись і обриває з'єднання (OBS_FRONTEND_EVENT_EXIT,
	// поки UI-потік ще крутиться — інакше join міг би чекати на запит до UI).
	void request_stop();
	// Блокує до кінця всіх потоків: обриває HTTP (бекап), чекає їх і релей.
	void stop();
	void set_token(std::string token); // "" — від'єднати
	// Підключитись знову з тим самим токеном (кнопка «Підключити тут» після 4002).
	void reconnect();
	void push_scene(const std::string &scene);
	// Пуш події серверу (готовий JSON-кадр); без з'єднання — відкидається.
	void push(std::string frame);
	LinkState state() const { return state_.load(); }
	RelayDiagnostics diagnostics();

private:
	void run();
	struct JobQueue {
		std::thread thread;
		std::mutex mutex;
		std::condition_variable cv;
		std::deque<std::pair<long long, std::function<void()>>> jobs; // (час прийому, робота)
	};

	CloseKind serve(WsConnection &ws);
	void writer(WsConnection &ws);
	void handle(const std::string &raw, long long received_ms);
	void enqueue(std::string frame);
	// Чекає ms або нового токена (переприв'язка не чекає кінця паузи).
	void sleep_interruptible(int ms, int token_gen);
	void log_handshake_failure(unsigned long http_status, unsigned long win_error);
	void set_error(std::string text);
	void post(JobQueue &queue, long long received_ms, std::function<void()> job);
	void job_loop(JobQueue &queue);
	void clear_jobs();
	void start_backup(const std::string &id, const std::string &trigger, const std::string &kinds);

	std::thread thread_;
	std::atomic<bool> stop_{false};
	std::atomic<LinkState> state_{LinkState::NoToken};
	std::atomic<long long> last_rx_ms_{0};
	bool live_ = false;              // writer працює, поки true (під mutex_)
	std::mutex mutex_;               // token_, token_gen_, queue_, current_, last_scene_, live_, last_error_*
	std::condition_variable cv_;
	std::string token_;
	int token_gen_ = 0;              // +1 на кожен set_token
	std::string last_scene_;
	std::deque<std::string> queue_;
	WsConnection *current_ = nullptr;
	unsigned long last_logged_status_ = ULONG_MAX; // лише потік run()
	std::string last_error_;
	long long last_error_ms_ = 0;

	// Зміни стану OBS і швидкі читання — по порядку в одному потоці.
	JobQueue light_;
	// ponytail: один потік на всі важкі запити — скріншот чекає за GetStats і
	// навпаки; окремий потік на кожен тип, якщо це колись стане помітно.
	JobQueue heavy_;

	// Один бекап за раз: другий запит, поки качається перший, — помилка.
	std::mutex backup_mutex_;
	std::thread backup_thread_;
	std::atomic<bool> backup_running_{false};
};

RelayClient &relay();
} // namespace ebot
