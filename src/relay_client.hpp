#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ebot {
enum class LinkState { NoToken, Connecting, Connected, Offline, Rejected };

class WsConnection;

// Труба єБот ↔ OBS: та сама поведінка, що RelayClient в obs_relay.py.
class RelayClient {
public:
	void start();
	// Не блокує: просить зупинитись і обриває з'єднання (OBS_FRONTEND_EVENT_EXIT,
	// поки UI-потік ще крутиться — інакше join міг би чекати на запит до UI).
	void request_stop();
	// Блокує до кінця всіх потоків: обриває HTTP (бекапи), чекає їх і релей.
	void stop();
	void set_token(std::string token); // "" — від'єднати
	void push_scene(const std::string &scene);
	LinkState state() const { return state_.load(); }

private:
	void run();
	void serve(WsConnection &ws);
	void writer(WsConnection &ws);
	void handle(const std::string &raw);
	void enqueue(std::string frame);
	void sleep_interruptible(int ms);

	std::thread thread_;
	// ponytail: бекапи кілька на день — вектор лише росте до виходу OBS; чистити, якщо стане частим.
	std::mutex backups_mutex_;
	std::vector<std::thread> backups_;
	std::atomic<bool> stop_{false};
	std::atomic<LinkState> state_{LinkState::NoToken};
	std::atomic<long long> last_rx_ms_{0};
	std::atomic<bool> live_{false}; // writer працює, поки true
	std::mutex mutex_;               // token_, queue_, current_, last_scene_
	std::condition_variable cv_;
	std::string token_;
	std::string last_scene_;
	std::deque<std::string> queue_;
	WsConnection *current_ = nullptr;
};

RelayClient &relay();
} // namespace ebot
