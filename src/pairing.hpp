#pragma once
#include <string>

namespace ebot {
struct PairStatus {
	std::string user_code; // "ABCD-2345", поки чекаємо підтвердження
	std::string error;     // текст для дока
	bool active = false;
};
void pair_begin();
void pair_cancel();
PairStatus pair_status();
std::string channel_name();
void set_channel_name(const std::string &channel);
std::string update_url();
void check_update_async();
// Одноразовий код входу в пульт (POST /api/obs/dock/handoff relay-токеном): у
// посилання браузера доку йде код на хвилину, а не сам токен. "" — без токена
// чи збій. Блокує — кликати з робочого потоку.
std::string dock_handoff_code();
} // namespace ebot
