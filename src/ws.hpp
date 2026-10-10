#pragma once
#include <atomic>
#include <mutex>
#include <string>

namespace ebot {
// Один WebSocket на WinHTTP: читає один потік, пише будь-який (під м'ютексом),
// abort() з будь-якого потоку обриває і блокуюче читання, і підключення.
class WsConnection {
public:
	~WsConnection();
	bool open(const std::string &base, const std::string &path_and_query);
	bool send_text(const std::string &text);
	// false — з'єднання закрите або обірване; close_code — код від сервера (0, якщо не було).
	bool receive_text(std::string &out, unsigned short &close_code);
	void abort();

private:
	void *session_ = nullptr;
	void *connect_ = nullptr;
	std::atomic<void *> pending_{nullptr}; // запит апгрейду, поки open() чекає сервер
	std::atomic<void *> socket_{nullptr};
	std::atomic<bool> aborted_{false};
	std::mutex send_mutex_;
};
} // namespace ebot
