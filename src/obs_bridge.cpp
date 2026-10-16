// Методи релею → запити obs-websocket (внутрішній API, без мережі й пароля).
// Форма відповідей — як в obs_relay.py, бо сервер розбирає один формат.
#include "obs_bridge.hpp"
#include "core.hpp"
#include "http.hpp"
#include "obs-websocket-api.h"
#include "relay_client.hpp"
#include <obs.hpp>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <chrono>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace ebot {

static double now_sec()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Сирий виклик obs-websocket. Повертає responseData як JSON ("{}" без даних).
static std::string call(const char *type, obs_data_t *data = nullptr)
{
	obs_websocket_request_response *r = obs_websocket_call_request(type, data);
	if (!r) // obs-websocket ще не готовий: старт OBS, зміна колекції сцен, вихід
		throw std::runtime_error(OBS_NOT_READY_ERROR);
	std::string out = r->response_data ? r->response_data : "{}";
	unsigned int code = r->status_code;
	std::string comment = r->comment ? r->comment : "";
	obs_websocket_request_response_free(r);
	if (code != 100) // RequestStatus::Success; текст — формат obsws-python, сервер його розбирає
		throw std::runtime_error(obs_error_text(type, code, comment));
	return out;
}

static OBSDataAutoRelease call_obj(const char *type, obs_data_t *data = nullptr)
{
	return OBSDataAutoRelease(obs_data_create_from_json(call(type, data).c_str()));
}

static std::string success()
{
	return "{\"success\":true}";
}

// Прямо з OBS, не через obs-websocket: той відмовляє, поки «не готовий», — а
// саме тоді (зміна колекції сцен) OBS і повідомляє про нову сцену.
std::string current_scene_name()
{
	OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
	const char *name = scene ? obs_source_get_name(scene) : nullptr;
	return name ? name : "";
}

void bridge_init()
{
	obs_websocket_ensure_ph(); // один раз з UI-потоку, а не навипередки з потоку релею
}

static std::string get_scenes_list()
{
	OBSDataAutoRelease resp = call_obj("GetSceneList");
	OBSDataArrayAutoRelease scenes = obs_data_get_array(resp, "scenes");
	std::string json = "{\"scenes\":[";
	bool first = true;
	for (size_t i = 0, n = obs_data_array_count(scenes); i < n; ++i) {
		OBSDataAutoRelease scene = obs_data_array_item(scenes, i);
		const char *name = obs_data_get_string(scene, "sceneName");
		if (!name || !*name)
			continue;
		json += std::string(first ? "" : ",") + "{\"name\":" + json_quote(name) +
			",\"index\":" + std::to_string(i) + "}";
		first = false;
	}
	return json + "]}";
}

static std::string set_scene(obs_data_t *params)
{
	OBSDataAutoRelease req = obs_data_create();
	obs_data_set_string(req, "sceneName", obs_data_get_string(params, "scene_name"));
	call("SetCurrentProgramScene", req);
	return success();
}

static std::string get_source_volume(obs_data_t *params)
{
	OBSDataAutoRelease req = obs_data_create();
	obs_data_set_string(req, "inputName", obs_data_get_string(params, "source_name"));
	OBSDataAutoRelease resp = call_obj("GetInputVolume", req);
	return "{\"volume_db\":" + std::to_string(obs_data_get_double(resp, "inputVolumeDb")) + "}";
}

static std::string set_source_volume(obs_data_t *params)
{
	OBSDataAutoRelease req = obs_data_create();
	obs_data_set_string(req, "inputName", obs_data_get_string(params, "source_name"));
	obs_data_set_double(req, "inputVolumeDb", obs_data_get_double(params, "volume_db"));
	call("SetInputVolume", req);
	return success();
}

// obs_data не вміє масив рядків — JSON складаємо самі через json_quote (core).
static std::string list_sources()
{
	OBSDataAutoRelease resp = call_obj("GetInputList");
	OBSDataArrayAutoRelease inputs = obs_data_get_array(resp, "inputs");
	std::string json = "{\"sources\":[";
	bool first = true;
	for (size_t i = 0, n = obs_data_array_count(inputs); i < n; ++i) {
		OBSDataAutoRelease input = obs_data_array_item(inputs, i);
		const char *name = obs_data_get_string(input, "inputName");
		if (!name || !*name)
			continue;
		json += std::string(first ? "" : ",") + json_quote(name);
		first = false;
	}
	return json + "]}";
}

static std::string get_screenshot(obs_data_t *params)
{
	std::string scene = current_scene_name();
	if (scene.empty())
		throw std::runtime_error("OBS did not report a current program scene");
	auto [w, h] = clamp_screenshot_size(obs_data_get_int(params, "width"), obs_data_get_int(params, "height"));
	OBSDataAutoRelease req = obs_data_create();
	obs_data_set_string(req, "sourceName", scene.c_str());
	obs_data_set_string(req, "imageFormat", SCREENSHOT_FORMAT);
	obs_data_set_int(req, "imageWidth", w);
	obs_data_set_int(req, "imageHeight", h);
	obs_data_set_int(req, "imageCompressionQuality", SCREENSHOT_QUALITY);
	OBSDataAutoRelease resp = call_obj("GetSourceScreenshot", req);
	const char *image = obs_data_get_string(resp, "imageData");
	if (!image || !*image)
		throw std::runtime_error("OBS returned an empty screenshot");
	return "{\"image\":" + json_quote(image) + ",\"scene\":" + json_quote(scene) +
	       ",\"width\":" + std::to_string(w) + ",\"height\":" + std::to_string(h) + "}";
}

static std::string get_version()
{
	// Напряму з libobs: GetVersion через obs-websocket падав на старті OBS.
	return "{\"version\":" + json_quote(obs_get_version_string()) +
	       ",\"relay_version\":" + std::to_string(RELAY_VERSION) + ",\"client\":" + json_quote(PLUGIN_CLIENT) +
	       ",\"plugin_version\":" + json_quote(PLUGIN_VERSION) + "}";
}

// --- Мікрофон: volmeter libobs замість InputVolumeMeters -------------------
// obs-websocket генерує InputVolumeMeters лише для підписаних клієнтів, тож
// лічильник ставимо самі, на вхід, про який спитав сервер.

struct Meter {
	obs_volmeter_t *meter = nullptr;
	obs_weak_source_t *weak = nullptr;
	std::string *name = nullptr; // параметр колбека лічильника й ключ сигналів джерела
};

static MicPeaks g_peaks;
static std::mutex g_meters_mutex;
static std::map<std::string, Meter> g_meters;

static void meter_cb(void *param, const float *, const float *peak, const float *)
{
	g_peaks.add(*static_cast<std::string *>(param), now_sec(), peak_to_db(peak, MAX_AUDIO_CHANNELS));
}

static void on_source_gone(void *data, calldata_t *);
static void on_source_mute(void *data, calldata_t *cd);

// Сигнали джерела, на які підписаний лічильник. «remove» (джерело видалили) і
// «rename» (ключ лічильника — назва) звільняють його, поки джерело ще живе:
// лише тоді obs_volmeter_detach_source справді знімає аудіоколбек з джерела.
// «destroy» лишається запасним шляхом (колекція сцен, вихід OBS).
static const char *const METER_SIGNALS[] = {"remove", "rename", "destroy"};

// Без g_meters_mutex: відписка чекає на м'ютекс сигналу, а обробник сигналу
// (on_source_gone) бере g_meters_mutex — тримати обидва навхрест = взаємне блокування.
static void free_meter(Meter &m)
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(m.weak);
	if (source) {
		signal_handler_t *sh = obs_source_get_signal_handler(source);
		for (const char *signal : METER_SIGNALS)
			signal_handler_disconnect(sh, signal, on_source_gone, m.name);
		signal_handler_disconnect(sh, "mute", on_source_mute, m.name);
	}
	obs_volmeter_remove_callback(m.meter, meter_cb, m.name);
	obs_volmeter_detach_source(m.meter);
	obs_volmeter_destroy(m.meter);
	obs_weak_source_release(m.weak);
	delete m.name;
}

// data — лише ключ для пошуку, не розіменовуємо: після bridge_shutdown() сигнал
// джерела, що знищується відкладено, може прийти з уже звільненим рядком.
static void on_source_gone(void *data, calldata_t *)
{
	Meter gone;
	{
		std::lock_guard<std::mutex> lock(g_meters_mutex);
		auto it = g_meters.begin();
		while (it != g_meters.end() && it->second.name != data)
			++it;
		if (it == g_meters.end())
			return;
		gone = it->second;
		g_meters.erase(it);
	}
	free_meter(gone);
}

// Мікрофон, за яким стежить сервер, вимкнули чи ввімкнули — одразу пушем, а не
// на наступному тіку сторожа. Назва — з самого джерела (живе під час свого
// сигналу), а не з data: див. on_source_gone.
static void on_source_mute(void *, calldata_t *cd)
{
	auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	const char *name = source ? obs_source_get_name(source) : nullptr;
	if (!name || !*name)
		return;
	relay().push("{\"type\":\"input_mute\",\"input\":" + json_quote(name) +
		     ",\"muted\":" + (calldata_bool(cd, "muted") ? "true" : "false") + "}");
}

// Перший get_mic_level для входу вмикає лічильник; до нього seen_samples == 0,
// і сервер не читає це як тишу (так само поводиться старий релей).
static void ensure_meter(const std::string &name)
{
	std::lock_guard<std::mutex> lock(g_meters_mutex);
	if (name.empty() || g_meters.count(name))
		return;
	OBSSourceAutoRelease source = obs_get_source_by_name(name.c_str());
	if (!source)
		return;
	Meter m;
	m.meter = obs_volmeter_create(OBS_FADER_LOG);
	m.weak = obs_source_get_weak_source(source);
	m.name = new std::string(name);
	obs_volmeter_add_callback(m.meter, meter_cb, m.name);
	obs_volmeter_attach_source(m.meter, source);
	signal_handler_t *sh = obs_source_get_signal_handler(source);
	for (const char *signal : METER_SIGNALS)
		signal_handler_connect(sh, signal, on_source_gone, m.name);
	signal_handler_connect(sh, "mute", on_source_mute, m.name);
	g_meters[name] = m;
}

static std::string get_mic_level(obs_data_t *params)
{
	std::string input = obs_data_get_string(params, "input_name");
	ensure_meter(input);
	// Форма відповіді obs_relay.py: null, коли значення невідоме.
	std::string muted = "null";
	try {
		OBSDataAutoRelease req = obs_data_create();
		obs_data_set_string(req, "inputName", input.c_str());
		OBSDataAutoRelease resp = call_obj("GetInputMute", req);
		muted = obs_data_get_bool(resp, "inputMuted") ? "true" : "false";
	} catch (...) {
	}
	MicLevel level = g_peaks.read(input, now_sec());
	std::string peak = level.samples ? std::to_string(level.peak_db) : "null";
	return "{\"peak_db\":" + peak + ",\"muted\":" + muted + ",\"seen_samples\":" + std::to_string(level.samples) +
	       "}";
}

// GetInputSettings віддає ВСІ налаштування входу, а серверу потрібна лише адреса
// плеєра музики (services/obs_vod_audio.py). URL алертів сторонніх сервісів несуть
// їхні секретні токени — тож адреса йде лише тоді, коли веде на сервер єБота.
static std::string input_settings_url_only(const std::string &json)
{
	OBSDataAutoRelease resp = obs_data_create_from_json(json.c_str());
	OBSDataAutoRelease settings = resp ? obs_data_get_obj(resp, "inputSettings") : nullptr;
	std::string url = settings ? obs_data_get_string(settings, "url") : "";
	std::string own = url_host(server_base());
	bool ours = !own.empty() && url_host(url) == own;
	std::string kind = resp ? obs_data_get_string(resp, "inputKind") : "";
	return "{\"inputKind\":" + json_quote(kind) + ",\"inputSettings\":" +
	       (ours ? "{\"url\":" + json_quote(url) + "}" : std::string("{}")) + "}";
}

static std::string obs_call(obs_data_t *params)
{
	std::string request = obs_data_get_string(params, "request");
	if (!is_allowed_request(request))
		return "{\"error\":" + json_quote("request not allowed: " + request) + "}";
	OBSDataAutoRelease data = obs_data_get_obj(params, "data");
	if (request == "GetProfileParameter") {
		std::string category = data ? obs_data_get_string(data, "parameterCategory") : "";
		std::string name = data ? obs_data_get_string(data, "parameterName") : "";
		if (!is_allowed_profile_param(category, name))
			return "{\"error\":" + json_quote("profile parameter not allowed: " + category + "/" + name) + "}";
	}
	if (request == "GetInputSettings")
		return input_settings_url_only(call(request.c_str(), data));
	return call(request.c_str(), data);
}

std::string dispatch(const std::string &method, obs_data_t *params)
{
	if (method == "get_scenes_list")
		return get_scenes_list();
	if (method == "get_current_scene") {
		std::string scene = current_scene_name();
		return "{\"scene\":" + (scene.empty() ? std::string("null") : json_quote(scene)) + "}";
	}
	if (method == "set_scene")
		return set_scene(params);
	if (method == "get_source_volume")
		return get_source_volume(params);
	if (method == "set_source_volume")
		return set_source_volume(params);
	if (method == "list_sources")
		return list_sources();
	if (method == "get_version")
		return get_version();
	if (method == "get_screenshot")
		return get_screenshot(params);
	if (method == "obs_call")
		return obs_call(params);
	if (method == "get_mic_level")
		return get_mic_level(params);
	throw std::runtime_error("Unknown method: " + method);
}

void bridge_shutdown()
{
	std::vector<Meter> meters;
	{
		std::lock_guard<std::mutex> lock(g_meters_mutex);
		for (auto &[name, m] : g_meters)
			meters.push_back(m);
		g_meters.clear();
	}
	for (Meter &m : meters)
		free_meter(m);
}

std::string outputs_frame()
{
	return std::string("{\"type\":\"outputs\",\"stream\":") + (obs_frontend_streaming_active() ? "true" : "false") +
	       ",\"record\":" + (obs_frontend_recording_active() ? "true" : "false") + "}";
}

} // namespace ebot
