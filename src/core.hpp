// Чиста частина плагіна: дзеркало констант і правил obs_relay.py.
// Без OBS і без Windows — щоб тестувалась у CI на будь-якій ОС.
#pragma once
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace ebot {

// Версія протоколу релею. Дзеркала: obs_relay.py (RELAY_VERSION) і
// obs_relay.ps1 ($RELAY_VERSION); паритет — tests/test_obs_relay_allowlist.py
// основного репо.
constexpr int RELAY_VERSION = 4;

// Сирі запити OBS, які сервер може надіслати через obs_call. Дзеркало
// obs_relay.py:OBS_CALL_ALLOWED_REQUESTS — той самий тест паритету.
inline const std::set<std::string> OBS_CALL_ALLOWED_REQUESTS = {
	"GetInputMute",        "SetInputMute",        "GetInputList",    "GetInputSettings",
	"GetInputAudioTracks", "GetProfileParameter", "GetSceneItemId",  "GetSceneItemEnabled",
	"SetSceneItemEnabled", "GetStreamStatus",     "GetRecordStatus", "GetStats",
	"StartStream",         "StopStream",          "StartRecord",     "StopRecord",
};

constexpr const char *SCREENSHOT_FORMAT = "jpg";
constexpr int SCREENSHOT_QUALITY = 75;
constexpr int SCREENSHOT_MIN_EDGE = 8;
constexpr int SCREENSHOT_MAX_EDGE = 1280;
constexpr int SCREENSHOT_DEFAULT_WIDTH = 1280;
constexpr int SCREENSHOT_DEFAULT_HEIGHT = 720;

constexpr int BACKUP_KEEP_DAYS = 30;
constexpr int BACKUP_DOWNLOAD_TIMEOUT_MS = 120000;

constexpr double MIC_PEAK_WINDOW_SEC = 30.0;
constexpr double MIC_PEAK_FLOOR_DB = -120.0;

constexpr int RECONNECT_DELAY_MS = 5000;
constexpr int PING_INTERVAL_MS = 25000;
constexpr int SILENCE_LIMIT_MS = 70000; // жодного кадру (навіть pong) — з'єднання мертве

bool is_allowed_request(const std::string &request);
std::pair<int, int> clamp_screenshot_size(long long width, long long height);
std::string backup_filename(const std::string &trigger, const std::tm &when);
bool is_newer_version(const std::string &latest, const std::string &current);
bool is_safe_token(const std::string &token);
std::string json_quote(const std::string &text);
double peak_to_db(const float *peaks, int channels);

enum class CloseKind { Retry, Rejected };
// Код закриття WS від сервера: 4001 = relay-токен не прийнято.
CloseKind classify_close(unsigned short code);
// HTTP-статус невдалого рукостискання WS: 401/403 = токен не прийнято (uvicorn
// відхиляє поганий токен ще до accept), решта — мережа чи деплой, пробуємо ще.
CloseKind classify_handshake(unsigned long http_status);

struct MicLevel {
	size_t samples = 0;
	double peak_db = MIC_PEAK_FLOOR_DB;
};

class MicPeaks {
public:
	void add(const std::string &input, double now, double peak_db);
	MicLevel read(const std::string &input, double now) const;

private:
	mutable std::mutex mutex_;
	std::map<std::string, std::deque<std::pair<double, double>>> buffers_;
};

} // namespace ebot
