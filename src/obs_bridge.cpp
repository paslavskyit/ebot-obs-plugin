// Методи релею → запити obs-websocket (внутрішній API, без мережі й пароля).
// Форма відповідей — як в obs_relay.py, бо сервер розбирає один формат.
#include "obs_bridge.hpp"
#include "core.hpp"
#include "obs-websocket-api.h"
#include <obs.hpp>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <chrono>
#include <map>
#include <mutex>
#include <stdexcept>

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
	if (!r)
		throw std::runtime_error("obs-websocket недоступний");
	std::string out = r->response_data ? r->response_data : "{}";
	unsigned int code = r->status_code;
	std::string comment = r->comment ? r->comment : "";
	obs_websocket_request_response_free(r);
	if (code != 100) // RequestStatus::Success
		throw std::runtime_error(comment.empty() ? "OBS request failed: " + std::to_string(code) : comment);
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

std::string current_scene_name()
{
	try {
		OBSDataAutoRelease resp = call_obj("GetCurrentProgramScene");
		return obs_data_get_string(resp, "currentProgramSceneName");
	} catch (...) {
		return "";
	}
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
	OBSDataAutoRelease resp = call_obj("GetVersion");
	return "{\"version\":" + json_quote(obs_data_get_string(resp, "obsVersion")) +
	       ",\"relay_version\":" + std::to_string(RELAY_VERSION) + ",\"client\":" + json_quote(PLUGIN_CLIENT) +
	       ",\"plugin_version\":" + json_quote(PLUGIN_VERSION) + "}";
}

// --- Мікрофон: volmeter libobs замість InputVolumeMeters -------------------
// obs-websocket генерує InputVolumeMeters лише для підписаних клієнтів, тож
// лічильник ставимо самі, на вхід, про який спитав сервер.

struct Meter {
	obs_volmeter_t *meter = nullptr;
	obs_weak_source_t *weak = nullptr;
	std::string *name = nullptr; // параметр і колбека лічильника, і сигналу destroy
};

static MicPeaks g_peaks;
static std::mutex g_meters_mutex;
static std::map<std::string, Meter> g_meters;

static void meter_cb(void *param, const float *, const float *peak, const float *)
{
	g_peaks.add(*static_cast<std::string *>(param), now_sec(), peak_to_db(peak, MAX_AUDIO_CHANNELS));
}

static void on_source_destroy(void *data, calldata_t *);

static void free_meter(Meter &m, bool disconnect)
{
	obs_volmeter_remove_callback(m.meter, meter_cb, m.name);
	obs_volmeter_detach_source(m.meter);
	obs_volmeter_destroy(m.meter);
	if (disconnect) {
		OBSSourceAutoRelease source = obs_weak_source_get_source(m.weak);
		if (source)
			signal_handler_disconnect(obs_source_get_signal_handler(source), "destroy", on_source_destroy,
						  m.name);
	}
	obs_weak_source_release(m.weak);
	delete m.name;
}

static void on_source_destroy(void *data, calldata_t *)
{
	std::string name = *static_cast<std::string *>(data);
	std::lock_guard<std::mutex> lock(g_meters_mutex);
	auto it = g_meters.find(name);
	if (it == g_meters.end())
		return;
	free_meter(it->second, false); // сигнал уже йде — відписуватись нема від чого
	g_meters.erase(it);
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
	signal_handler_connect(obs_source_get_signal_handler(source), "destroy", on_source_destroy, m.name);
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

static std::string obs_call(obs_data_t *params)
{
	std::string request = obs_data_get_string(params, "request");
	if (!is_allowed_request(request))
		return "{\"error\":" + json_quote("request not allowed: " + request) + "}";
	OBSDataAutoRelease data = obs_data_get_obj(params, "data");
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
	std::lock_guard<std::mutex> lock(g_meters_mutex);
	for (auto &[name, m] : g_meters)
		free_meter(m, true);
	g_meters.clear();
}

} // namespace ebot
