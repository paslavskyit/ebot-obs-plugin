#include "ws.hpp"
#include "http.hpp"
#include <windows.h>
#include <winhttp.h>

namespace ebot {

WsConnection::~WsConnection()
{
	abort();
	if (connect_)
		WinHttpCloseHandle(connect_);
	if (session_)
		WinHttpCloseHandle(session_);
}

bool WsConnection::open(const std::string &base, const std::string &path, const std::string &bearer,
			unsigned long &http_status)
{
	http_status = 0;
	ServerParts parts;
	if (aborted_ || !parse_base(base, parts))
		return false;
	session_ = WinHttpOpen(L"ebot-obs", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
			       WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session_)
		return false;
	// Читання без тайм-ауту (тишу ловить сторож у RelayClient), решта — 10 с.
	WinHttpSetTimeouts(session_, 10000, 10000, 10000, 0);
	connect_ = WinHttpConnect(session_, parts.host.c_str(), parts.port, 0);
	if (!connect_)
		return false;
	std::wstring wide_path = widen(path);
	std::wstring headers = auth_header(bearer);
	HINTERNET request = WinHttpOpenRequest(connect_, L"GET", wide_path.c_str(), nullptr, WINHTTP_NO_REFERER,
					       WINHTTP_DEFAULT_ACCEPT_TYPES, parts.secure ? WINHTTP_FLAG_SECURE : 0);
	if (!request)
		return false;
	pending_.store(request);
	if (aborted_) {
		abort();
		return false;
	}
	// Відповідь на апгрейд — не довше 10 с: тайм-аут читання 0 (без нього тишу
	// ловить сторож) діяв і тут, і сервер, що прийняв TLS, але не відповів 101,
	// тримав підключення вічно — сторож тиші стартує лише після апгрейду.
	DWORD response_timeout = 10000;
	WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &response_timeout, sizeof(response_timeout));
	BOOL ok = WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
		  WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
				     headers.empty() ? 0 : (DWORD)-1L, nullptr, 0, 0, 0) &&
		  WinHttpReceiveResponse(request, nullptr);
	if (!ok)
		win_error_ = GetLastError();
	DWORD status = 0, size = sizeof(status);
	if (ok)
		WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
	http_status = status;
	HINTERNET socket = (ok && status == 101) ? WinHttpWebSocketCompleteUpgrade(request, 0) : nullptr;
	// Якщо abort() уже закрив запит — він же його і звільнив.
	if (pending_.exchange(nullptr) == request)
		WinHttpCloseHandle(request);
	socket_.store(socket);
	if (socket && aborted_) {
		abort();
		return false;
	}
	return socket != nullptr;
}

bool WsConnection::send_text(const std::string &text)
{
	std::lock_guard<std::mutex> lock(send_mutex_);
	HINTERNET socket = socket_.load();
	return socket && WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text.data(),
					      (DWORD)text.size()) == NO_ERROR;
}

bool WsConnection::receive_text(std::string &out, unsigned short &close_code)
{
	out.clear();
	close_code = 0;
	char buf[16384];
	for (;;) {
		HINTERNET socket = socket_.load();
		if (!socket)
			return false;
		DWORD read = 0;
		WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
		if (WinHttpWebSocketReceive(socket, buf, sizeof buf, &read, &type) != NO_ERROR)
			return false;
		if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
			USHORT status = 0;
			DWORD reason_len = 0;
			char reason[128];
			if (WinHttpWebSocketQueryCloseStatus(socket, &status, reason, sizeof reason, &reason_len) ==
			    NO_ERROR)
				close_code = status;
			return false;
		}
		out.append(buf, read);
		if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
		    type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
			return true;
	}
}

// Закриття хендла з іншого потоку — документований спосіб обірвати блокуючий
// виклик WinHTTP; exchange гарантує одне закриття кожного хендла.
void WsConnection::abort()
{
	aborted_ = true;
	if (HINTERNET request = pending_.exchange(nullptr))
		WinHttpCloseHandle(request);
	if (HINTERNET socket = socket_.exchange(nullptr))
		WinHttpCloseHandle(socket);
}

} // namespace ebot
