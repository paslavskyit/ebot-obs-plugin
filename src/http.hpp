#pragma once
#include <string>

namespace ebot {
struct HttpResponse {
	unsigned long status = 0; // 0 = мережевий збій
	std::string body;
};
struct ServerParts {
	bool secure = true;
	std::wstring host;
	unsigned short port = 443;
};
std::wstring widen(const std::string &text);
std::string narrow(const std::wstring &text);
// https://api.ebot.live; для розробки — EBOT_SERVER=http://localhost:8000 перед запуском OBS.
std::string server_base();
bool parse_base(const std::string &base, ServerParts &out);
// Обриває всі запити, що зараз ідуть, і закриває HTTP назавжди (вихід OBS):
// блокуючі виклики WinHTTP повертаються з помилкою, нові одразу дають status 0.
void http_abort_all();
HttpResponse http_request(const std::string &base, const wchar_t *method, const std::string &path_and_query,
			  const std::string &json_body, unsigned long timeout_ms);
} // namespace ebot
