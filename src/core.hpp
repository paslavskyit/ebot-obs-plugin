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

// Версія протоколу релею. Дзеркало: obs_relay.py (RELAY_VERSION, macOS / Linux);
// паритет — tests/test_obs_relay_allowlist.py основного репо.
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

// GetProfileParameter читає будь-який ключ профілю OBS — і Twitch-токени
// вбудованого входу OBS теж. Сервер питає лише ці пари (services/obs_vod_audio.py).
inline const std::set<std::pair<std::string, std::string>> OBS_PROFILE_PARAMS_ALLOWED = {
	{"Output", "Mode"},          {"SimpleOutput", "UseAdvanced"}, {"SimpleOutput", "VodTrackEnabled"},
	{"AdvOut", "TrackIndex"},    {"AdvOut", "VodTrackIndex"},     {"AdvOut", "VodTrackEnabled"},
};

// Перепідключення: перша спроба майже одразу, далі вдвічі довше до стелі; розкид
// — щоб після деплою всі плагіни не стукали в ту саму секунду.
constexpr int RECONNECT_FIRST_MS = 500;
constexpr int RECONNECT_MAX_MS = 30000;
constexpr int STABLE_CONNECTION_MS = 60000; // протрималось стільки — лічильник спроб з нуля
// «Токен не прийнято» теж пробуємо знову, але рідко: збій сервера не має
// вимикати канал до ручної переприв'язки (рев'ю 11.10.2026).
constexpr int REJECTED_RETRY_FIRST_MS = 30000;
constexpr int REJECTED_RETRY_MAX_MS = 300000;
constexpr int PING_INTERVAL_MS = 15000;
constexpr int SILENCE_LIMIT_MS = 35000; // жодного кадру (навіть pong) — з'єднання мертве

// Тексти помилок OBS у форматі obsws-python: сервер (_map_relay_error) розбирає саме їх.
constexpr const char *OBS_NOT_READY_ERROR = "OBS is not ready (returned code 207)";

bool is_allowed_request(const std::string &request);
bool is_allowed_profile_param(const std::string &category, const std::string &name);
// Запити, які можуть тривати довго (скріншот: GPU + JPEG; GetStats: вільне місце
// на диску запису, на сонному мережевому — секунди): окрема черга, щоб не
// тримати решту команд.
bool is_heavy_request(const std::string &method, const std::string &obs_request);
// Куди можна відкрити браузер для прив'язки: https або локальний сервер розробника.
bool is_safe_verify_url(const std::string &url);
// jitter — від 0 до 1 (випадкове), attempt — 0 для першої спроби.
int reconnect_delay_ms(int attempt, double jitter);
int rejected_retry_delay_ms(int attempt, double jitter);
std::string obs_error_text(const std::string &request, unsigned int code, const std::string &comment);
std::pair<int, int> clamp_screenshot_size(long long width, long long height);
std::string backup_filename(const std::string &trigger, const std::tm &when);
bool is_newer_version(const std::string &latest, const std::string &current);
bool is_safe_token(const std::string &token);
std::string json_quote(const std::string &text);
double peak_to_db(const float *peaks, int channels);

enum class CloseKind { Retry, Rejected };
// Код закриття WS від сервера: 4001 = relay-токен не прийнято; 1013 «спробуй
// пізніше» (сервер не зміг перевірити токен) — звичайне перепідключення.
CloseKind classify_close(unsigned short code);
// HTTP-статус невдалого рукостискання WS: 401/403 = токен не прийнято (uvicorn
// відхиляє поганий токен ще до accept), решта — мережа чи деплой, пробуємо ще.
CloseKind classify_handshake(unsigned long http_status);

// Пульт у доці: кольори поточної теми OBS для сторінки /dock. Ключі — ті самі,
// що OBS_DOCK_THEME_KEYS сервера; сервер (і сторінка) приймають лише #rrggbb.
struct DockTheme {
	std::string bg, panel, fg, muted, border, accent;
};
std::string hex_color(int r, int g, int b);
// /api/obs/dock/enter?code=…&bg=%23rrggbb… — невалідні кольори просто не йдуть.
std::string dock_enter_path(const std::string &code, const DockTheme &theme);

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
