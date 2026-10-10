#pragma once
#include <filesystem>
#include <string>

namespace ebot {
struct HttpResponse {
	unsigned long status = 0; // 0 = мережевий збій
	std::string body;
};
struct DownloadResult {
	unsigned long status = 0; // 200 — файл записано повністю; 0 — мережа, обрив або запис
	size_t bytes = 0;
	std::string error_body; // початок тіла відповіді, коли сервер відповів не 200
	bool write_failed = false;
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
// «Authorization: Bearer …\r\n» (порожньо без токена) — токен не в рядку запиту,
// бо сервер пише рядок запиту в лог.
std::wstring auth_header(const std::string &bearer);
// Обриває всі запити, що зараз ідуть, і закриває HTTP назавжди (вихід OBS):
// блокуючі виклики WinHTTP повертаються з помилкою, нові одразу дають status 0.
void http_abort_all();
HttpResponse http_request(const std::string &base, const wchar_t *method, const std::string &path_and_query,
			  const std::string &json_body, unsigned long timeout_ms, const std::string &bearer = "");
// GET тілом прямо у файл, шматками: бекап на десятки МБ не лежить у пам'яті OBS.
DownloadResult http_download(const std::string &base, const std::string &path, const std::string &bearer,
			     const std::filesystem::path &file, unsigned long timeout_ms);
} // namespace ebot
