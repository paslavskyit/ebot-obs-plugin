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
#include <vector>

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
	"GetVideoSettings",    "GetSceneItemList",    "GetGroupSceneItemList",
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

// Рівень мікрофона пушем (`mic_level`, 0.5): не частіше 10 разів на секунду —
// сирі InputVolumeMeters obs-websocket ідуть 20 Гц на кожен вхід, задорого для
// релею. Шкала 0..1 — лінійна в dB від MIC_LEVEL_FLOOR_DB до 0 dBFS, як смужка
// мікшера OBS.
constexpr double MIC_LEVEL_PUSH_INTERVAL_SEC = 0.1;
constexpr double MIC_LEVEL_FLOOR_DB = -60.0;

// Пуш `item_transform` (0.5): лише після тиші — перетягування елемента мишею
// шле зміну на кожен кадр.
constexpr double ITEM_TRANSFORM_DEBOUNCE_SEC = 0.5;

// Фільтр «єБот: заокруглення камери» (0.5). Id не міняти: за ним команда
// set_webcam_mask знаходить уже доданий фільтр (назву стример може змінити).
constexpr const char *WEBCAM_MASK_FILTER_ID = "ebot_webcam_mask";
constexpr int WEBCAM_MASK_MAX_PX = 4096; // радіус, обрізка й рамка — px джерела

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
// Інсталятор оновлює лише копію в ProgramData. Портативний OBS (і ручна
// розкладка деінде) отримує сторінку релізу: там zip та інструкція.
constexpr const char *RELEASES_PAGE_URL = "https://github.com/paslavskyit/ebot-obs-plugin/releases/latest";

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
// Файл лежить у теці: без регістру, «\» = «/» (OBS склеює шляхи з обома).
bool path_under(const std::string &path, const std::string &dir);
bool is_safe_token(const std::string &token);
std::string json_quote(const std::string &text);
double peak_to_db(const float *peaks, int channels);
// Значення з сервера (px джерела) → 0..WEBCAM_MASK_MAX_PX; NaN і мінус — 0.
int clamp_mask_px(double value);
// Пік у dBFS → 0..1 з кроком 0,01 (MIC_LEVEL_FLOOR_DB і тихіше — 0).
double mic_level_from_db(double peak_db);
// {"type":"mic_level","level":0.45} — без printf: десяткова кома локалі не потрапить у JSON.
std::string mic_level_frame(double level);

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

// Колбек volmeter-а (кожен аудіоблок, ~50 разів на секунду) → пуш `mic_level`:
// максимум піку за вікно MIC_LEVEL_PUSH_INTERVAL_SEC (короткий склад не
// губиться між пушами) і без повтору того самого значення (тиша не шле 0 щоразу).
class MicLevelThrottle {
public:
	// Рівень 0..1, який час пушити, або -1 — цього разу нічого не слати.
	double feed(double now, double peak_db);
	void reset(); // нове вмикання: вікно й останнє значення з нуля

private:
	std::mutex mutex_;
	double window_start_ = -1.0;
	double max_db_ = MIC_PEAK_FLOOR_DB;
	double last_sent_ = -1.0;
};

// Дебаунс пушу `item_transform`: ключ (сцена, джерело) відстоюється, поки
// touch не повторювались ITEM_TRANSFORM_DEBOUNCE_SEC.
class Debouncer {
public:
	using Key = std::pair<std::string, std::string>;
	void touch(const Key &key, double now);
	std::vector<Key> due(double now); // ключі, що відстоялися (забирає їх)
	void clear();

private:
	std::mutex mutex_;
	std::map<Key, double> deadlines_;
};

} // namespace ebot
