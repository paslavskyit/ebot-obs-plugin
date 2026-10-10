// Запити до єБота через WinHTTP — вбудований у Windows, TLS через Schannel.
#include "http.hpp"
#include <windows.h>
#include <winhttp.h>
#include <cstdlib>
#include <mutex>
#include <set>

namespace ebot {

static std::mutex g_inflight_mutex;
static std::set<HINTERNET> g_inflight;
static bool g_http_closed = false;

// true — запит зареєстровано; false — HTTP уже закрито, запит треба кинути.
static bool track(HINTERNET request)
{
	std::lock_guard<std::mutex> lock(g_inflight_mutex);
	if (g_http_closed)
		return false;
	g_inflight.insert(request);
	return true;
}

// true — хендл ще наш і закрити його треба тут (інакше його вже закрив http_abort_all).
static bool untrack(HINTERNET request)
{
	std::lock_guard<std::mutex> lock(g_inflight_mutex);
	return g_inflight.erase(request) == 1;
}

void http_abort_all()
{
	std::lock_guard<std::mutex> lock(g_inflight_mutex);
	g_http_closed = true;
	for (HINTERNET request : g_inflight)
		WinHttpCloseHandle(request);
	g_inflight.clear();
}

std::wstring widen(const std::string &text)
{
	if (text.empty())
		return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n);
	return out;
}

std::string narrow(const std::wstring &text)
{
	if (text.empty())
		return "";
	int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
	std::string out((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n, nullptr, nullptr);
	return out;
}

std::string server_base()
{
	char *env = nullptr;
	size_t len = 0;
	std::string base = "https://api.ebot.live";
	if (_dupenv_s(&env, &len, "EBOT_SERVER") == 0 && env && *env)
		base = env;
	free(env);
	return base;
}

bool parse_base(const std::string &base, ServerParts &out)
{
	std::wstring wide = widen(base);
	wchar_t host[256] = {};
	URL_COMPONENTS uc{};
	uc.dwStructSize = sizeof(uc);
	uc.lpszHostName = host;
	uc.dwHostNameLength = 256;
	uc.dwUrlPathLength = (DWORD)-1;
	if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &uc))
		return false;
	out.secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
	out.host = host;
	out.port = uc.nPort;
	return true;
}

HttpResponse http_request(const std::string &base, const wchar_t *method, const std::string &path_and_query,
			  const std::string &json_body, unsigned long timeout_ms)
{
	HttpResponse result;
	ServerParts parts;
	if (!parse_base(base, parts))
		return result;
	HINTERNET session = WinHttpOpen(L"ebot-obs", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET connect = session ? WinHttpConnect(session, parts.host.c_str(), parts.port, 0) : nullptr;
	std::wstring path = widen(path_and_query);
	HINTERNET request = connect ? WinHttpOpenRequest(connect, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
							 WINHTTP_DEFAULT_ACCEPT_TYPES,
							 parts.secure ? WINHTTP_FLAG_SECURE : 0)
				    : nullptr;
	bool tracked = request && track(request);
	if (tracked) {
		int t = (int)timeout_ms;
		WinHttpSetTimeouts(request, t, t, t, t);
		const wchar_t *headers = json_body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
							   : L"Content-Type: application/json\r\n";
		DWORD headers_len = json_body.empty() ? 0 : (DWORD)-1L;
		BOOL ok = WinHttpSendRequest(request, headers, headers_len, (LPVOID)json_body.data(),
					     (DWORD)json_body.size(), (DWORD)json_body.size(), 0) &&
			  WinHttpReceiveResponse(request, nullptr);
		if (ok) {
			DWORD status = 0, size = sizeof(status);
			WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
			DWORD expected = 0, len_size = sizeof(expected);
			BOOL has_length = WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
							      WINHTTP_HEADER_NAME_BY_INDEX, &expected, &len_size,
							      WINHTTP_NO_HEADER_INDEX);
			char buf[16384];
			DWORD read = 0;
			BOOL read_ok = TRUE;
			for (;;) {
				read_ok = WinHttpReadData(request, buf, sizeof buf, &read);
				if (!read_ok || read == 0)
					break;
				result.body.append(buf, read);
			}
			// Обірване посеред тіла — збій, а не «200 з половиною архіву».
			bool complete = read_ok && (!has_length || result.body.size() == expected);
			result.status = complete ? status : 0;
		}
	}
	if (request && (!tracked || untrack(request)))
		WinHttpCloseHandle(request);
	if (connect)
		WinHttpCloseHandle(connect);
	if (session)
		WinHttpCloseHandle(session);
	return result;
}

} // namespace ebot
