// єБот для OBS: тонка труба між OBS і єБотом (https://ebot.live).
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include "config.hpp"
#include "dock.hpp"
#include "obs_bridge.hpp"
#include "pairing.hpp"
#include "relay_client.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static void on_frontend(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	// Нова колекція сцен і перейменування сцени теж міняють поточну назву;
	// повтор тієї самої сцени push_scene відсіює.
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		ebot::relay().push_scene(ebot::current_scene_name());
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		ebot::bridge_shutdown();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		// request_stop не блокує; на вихід obs-websocket уже не обслуговує нових
		// запитів (SCRIPTING_SHUTDOWN раніше за EXIT), а запит, що йде, доробиться,
		// поки UI ще обробляє події — join в obs_module_unload не зависне.
		ebot::relay().request_stop();
		ebot::bridge_shutdown();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	obs_frontend_add_event_callback(on_frontend, nullptr);
	return true;
}

// obs-websocket вантажиться раніше — його API доступний лише тут.
void obs_module_post_load(void)
{
	ebot::bridge_init();
	ebot::create_dock();
	ebot::Saved saved = ebot::load_config();
	ebot::set_channel_name(saved.channel);
	ebot::relay().start();
	ebot::relay().set_token(saved.token);
	ebot::check_update_async();
	obs_log(LOG_INFO, "eBot plugin loaded (version %s)", PLUGIN_VERSION);
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend, nullptr);
	ebot::pair_cancel();
	ebot::relay().stop();
	ebot::bridge_shutdown();
}
