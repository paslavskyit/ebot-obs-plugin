// Запити до єБота через WinHTTP — вбудований у Windows, TLS через Schannel.
#include "http.hpp"
#include <windows.h>
#include <winhttp.h>
#include <cstdlib>
#include <fstream>
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

std::wstring auth_header(const std::string &bearer)
{
	return bearer.empty() ? std::wstring() : L"Authorization: Bearer " + widen(bearer) + L"\r\n";
}

namespace {
// Сесія → з'єднання → запит; закриває все сам. Запит реєструється в g_inflight,
// щоб http_abort_all() міг обірвати його з іншого потоку.
class Exchange {
public:
	DWORD status = 0;

	~Exchange()
	{
		if (request_ && (!tracked_ || untrack(request_)))
			WinHttpCloseHandle(request_);
		if (connect_)
			WinHttpCloseHandle(connect_);
		if (session_)
			WinHttpCloseHandle(session_);
	}

	bool send(const std::string &base, const wchar_t *method, const std::string &path, const std::string &body,
		  unsigned long timeout_ms, const std::string &bearer)
	{
		ServerParts parts;
		if (!parse_base(base, parts))
			return false;
		session_ = WinHttpOpen(L"ebot-obs", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
				       WINHTTP_NO_PROXY_BYPASS, 0);
		connect_ = session_ ? WinHttpConnect(session_, parts.host.c_str(), parts.port, 0) : nullptr;
		std::wstring wide_path = widen(path);
		request_ = connect_ ? WinHttpOpenRequest(connect_, method, wide_path.c_str(), nullptr, WINHTTP_NO_REFERER,
							 WINHTTP_DEFAULT_ACCEPT_TYPES, parts.secure ? WINHTTP_FLAG_SECURE : 0)
				    : nullptr;
		tracked_ = request_ && track(request_);
		if (!tracked_)
			return false;
		int t = (int)timeout_ms;
		WinHttpSetTimeouts(request_, t, t, t, t);
		std::wstring headers = auth_header(bearer);
		if (!body.empty())
			headers += L"Content-Type: application/json\r\n";
		if (!WinHttpSendRequest(request_, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
					headers.empty() ? 0 : (DWORD)-1L, (LPVOID)body.data(), (DWORD)body.size(),
					(DWORD)body.size(), 0) ||
		    !WinHttpReceiveResponse(request_, nullptr))
			return false;
		DWORD size = sizeof(status);
		WinHttpQueryHeaders(request_, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
		DWORD len_size = sizeof(expected_);
		has_length_ = WinHttpQueryHeaders(request_, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
						  WINHTTP_HEADER_NAME_BY_INDEX, &expected_, &len_size,
						  WINHTTP_NO_HEADER_INDEX) != FALSE;
		return true;
	}

	// Тіло шматками в sink (false з sink — зупинитись). true — тіло прочитане
	// повністю: обірване посеред — збій, а не «200 з половиною архіву».
	template<typename Sink> bool read_body(Sink &&sink, size_t &total)
	{
		char buf[16384];
		total = 0;
		for (;;) {
			DWORD read = 0;
			if (!WinHttpReadData(request_, buf, sizeof buf, &read))
				return false;
			if (read == 0)
				break;
			if (!sink(buf, read))
				return false;
			total += read;
		}
		return !has_length_ || total == expected_;
	}

private:
	HINTERNET session_ = nullptr;
	HINTERNET connect_ = nullptr;
	HINTERNET request_ = nullptr;
	bool tracked_ = false;
	bool has_length_ = false;
	DWORD expected_ = 0;
};
} // namespace

HttpResponse http_request(const std::string &base, const wchar_t *method, const std::string &path_and_query,
			  const std::string &json_body, unsigned long timeout_ms, const std::string &bearer)
{
	HttpResponse result;
	Exchange ex;
	if (!ex.send(base, method, path_and_query, json_body, timeout_ms, bearer))
		return result;
	size_t total = 0;
	bool complete = ex.read_body(
		[&](const char *data, DWORD n) {
			result.body.append(data, n);
			return true;
		},
		total);
	result.status = complete ? ex.status : 0;
	return result;
}

DownloadResult http_download(const std::string &base, const std::string &path, const std::string &bearer,
			     const std::filesystem::path &file, unsigned long timeout_ms)
{
	DownloadResult result;
	Exchange ex;
	if (!ex.send(base, L"GET", path, "", timeout_ms, bearer))
		return result;
	size_t total = 0;
	if (ex.status != 200) {
		// Помилка сервера: початок тіла — для тексту помилки, файл не чіпаємо.
		ex.read_body(
			[&](const char *data, DWORD n) {
				result.error_body.append(data, n);
				return result.error_body.size() < 512;
			},
			total);
		result.status = ex.status;
		return result;
	}
	std::ofstream out(file, std::ios::binary | std::ios::trunc);
	if (!out) {
		result.write_failed = true;
		return result;
	}
	bool complete = ex.read_body(
		[&](const char *data, DWORD n) {
			out.write(data, (std::streamsize)n);
			return out.good();
		},
		result.bytes);
	out.close();
	result.write_failed = !out.good();
	result.status = (complete && !result.write_failed) ? 200 : 0;
	return result;
}

} // namespace ebot
