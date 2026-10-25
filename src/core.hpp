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

// Перепідключення — «повний розкид»: пауза випадкова від 0 до вікна, вікно
// вдвічі ширше з кожною спробою до стелі. Деплой рве всі з'єднання разом, і
// перша спроба за 0,5–1 с означала, що весь парк плагінів стукав у новий
// інстанс в ту саму секунду (рев'ю 16.10.2026).
constexpr int RECONNECT_FIRST_MS = 5000;
constexpr int RECONNECT_MAX_MS = 30000;
constexpr int STABLE_CONNECTION_MS = 60000; // протрималось стільки — лічильник спроб з нуля
// «Токен не прийнято» теж пробуємо знову, але рідко: збій сервера не має
// вимикати канал до ручної переприв'язки (рев'ю 11.10.2026).
constexpr int REJECTED_RETRY_FIRST_MS = 30000;
constexpr int REJECTED_RETRY_MAX_MS = 300000;
constexpr int PING_INTERVAL_MS = 15000;
constexpr int SILENCE_LIMIT_MS = 35000; // жодного кадру (навіть pong) — з'єднання мертве

// Скільки сервер чекає відповіді на запит (OBS_RELAY_REQUEST_TIMEOUT_SEC
// основного репо). Запит, що пролежав у черзі довше, сервер уже оголосив
// невдалим — виконати його зараз означало б «примарну» дію: сцена чи старт
// стріму через пів хвилини після того, як стример побачив «не вдалося».
constexpr int REQUEST_STALE_MS = 5000;

// Пульт у доці: повтор одноразового коду, коли сервер не дав його (503, мережа).
// Раніше повтор ішов на кожному тіку таймера дока — двічі на секунду без кінця.
constexpr int DOCK_RETRY_FIRST_MS = 5000;
constexpr int DOCK_RETRY_MAX_MS = 60000;

// Перевірка нової версії (latest.json на GitHub) — раз на добу, не лише на старті.
constexpr int UPDATE_CHECK_INTERVAL_MS = 24 * 60 * 60 * 1000;

// Коди закриття WS від сервера (obs_relay_server основного репо).
constexpr unsigned short RELAY_CLOSE_REJECTED = 4001; // токен не прийнято
constexpr unsigned short RELAY_CLOSE_REPLACED = 4002; // канал підключено з іншого ПК

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
// Пауза перед повтором коду пульта; 429 («забагато спроб») — без автоповтору:
// кожна спроба лише продовжує блок, і то для всіх за тією самою адресою.
int dock_retry_delay_ms(int attempt);
bool dock_auto_retry(unsigned long http_status);
// Хост адреси в нижньому регістрі, без порту; "" — не http(s) або з «user@».
std::string url_host(const std::string &url);
std::string obs_error_text(const std::string &request, unsigned int code, const std::string &comment);
std::pair<int, int> clamp_screenshot_size(long long width, long long height);
std::string backup_filename(const std::string &trigger, const std::tm &when);
bool is_newer_version(const std::string &latest, const std::string &current);
bool is_safe_token(const std::string &token);
std::string json_quote(const std::string &text);
double peak_to_db(const float *peaks, int channels);

enum class CloseKind { Retry, Rejected, Replaced };
// Код закриття WS від сервера: 4001 = relay-токен не прийнято; 4002 = той самий
// канал підключився з іншого ПК (або поруч лишився старий релей) — тут не
// перепідключаємось самі, інакше два ПК відбирали б канал один в одного;
// 1013 «спробуй пізніше» (сервер не зміг перевірити токен) — звичайне перепідключення.
CloseKind classify_close(unsigned short code);
// HTTP-статус невдалого рукостискання WS: 401/403 = токен не прийнято (uvicorn
// відхиляє поганий токен ще до accept), решта — мережа чи деплой, пробуємо ще.
CloseKind classify_handshake(unsigned long http_status);

// Пульт у доці: кольори поточної теми OBS для сторінки /dock. Ключі — ті самі,
// що OBS_DOCK_THEME_KEYS сервера; сервер (і сторінка) приймають лише #rrggbb.
// font_px — розмір шрифту OBS (Налаштування → Вигляд) у px: пульт масштабується
// разом з ним; поза межами OBS_DOCK_FONT_PX_* сервера не передається.
struct DockTheme {
	std::string bg, panel, fg, muted, border, accent;
	int font_px = 0;
};
constexpr int DOCK_FONT_PX_MIN = 9;
constexpr int DOCK_FONT_PX_MAX = 32;
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
