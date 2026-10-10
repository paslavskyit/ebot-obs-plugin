// Спайк: чи працює внутрішній API obs-websocket, коли WebSocket-сервер
// вимкнений у налаштуваннях OBS, і чи можна кликати його не з UI-потоку.
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <thread>
#include "obs-websocket-api.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static void on_frontend(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_SCENE_CHANGED)
		return;
	obs_source_t *scene = obs_frontend_get_current_scene();
	obs_log(LOG_INFO, "[spike] scene -> %s", scene ? obs_source_get_name(scene) : "(null)");
	obs_source_release(scene);
}

bool obs_module_load(void)
{
	obs_frontend_add_event_callback(on_frontend, nullptr);
	return true;
}

void obs_module_post_load(void)
{
	std::thread([] {
		obs_websocket_request_response *r = obs_websocket_call_request("GetSceneList", nullptr);
		if (!r) {
			obs_log(LOG_WARNING, "[spike] call_request returned null");
			return;
		}
		obs_log(LOG_INFO, "[spike] GetSceneList status=%u data=%.200s", r->status_code,
			r->response_data ? r->response_data : "");
		obs_websocket_request_response_free(r);
	}).detach();
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend, nullptr);
}
