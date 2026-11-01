#include "scene_watch.hpp"
#include "core.hpp"
#include "relay_client.hpp"
#include <obs.hpp>
#include <obs-frontend-api.h>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>

namespace ebot {

static double now_sec()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Що стример може змінити в елементі сам. Сигнал item_transform OBS шле й тоді,
// коли змінився лише розмір джерела (текст, що тикає щосекунди), — такі сервер
// не цікавлять, тож порівнюємо з останнім відомим станом.
struct ItemState {
	obs_transform_info info;
	obs_sceneitem_crop crop;
};

static bool same(const ItemState &a, const ItemState &b)
{
	const obs_transform_info &x = a.info, &y = b.info;
	return x.pos.x == y.pos.x && x.pos.y == y.pos.y && x.rot == y.rot && x.scale.x == y.scale.x &&
	       x.scale.y == y.scale.y && x.alignment == y.alignment && x.bounds_type == y.bounds_type &&
	       x.bounds_alignment == y.bounds_alignment && x.bounds.x == y.bounds.x && x.bounds.y == y.bounds.y &&
	       x.crop_to_bounds == y.crop_to_bounds && a.crop.left == b.crop.left && a.crop.top == b.crop.top &&
	       a.crop.right == b.crop.right && a.crop.bottom == b.crop.bottom;
}

static ItemState read_state(obs_sceneitem_t *item)
{
	ItemState state{};
	obs_sceneitem_get_info2(item, &state.info);
	obs_sceneitem_get_crop(item, &state.crop);
	return state;
}

static std::mutex g_mutex; // g_bound, g_items
static obs_weak_source_t *g_bound = nullptr;
static std::map<int64_t, ItemState> g_items; // id елемента → останній відомий стан
static Debouncer g_debounce;

// ponytail: лише елементи верхнього рівня сцени — камеру всередині групи посунули,
// а пушу нема (групу цілком — є). Підписка на кожну групу сцени, якщо знадобиться.
// Потік відео OBS, під м'ютексом сцени: нічого, що бере його знову (enum_items).
static void on_item_transform(void *, calldata_t *cd)
{
	auto *scene = static_cast<obs_scene_t *>(calldata_ptr(cd, "scene"));
	auto *item = static_cast<obs_sceneitem_t *>(calldata_ptr(cd, "item"));
	if (!scene || !item)
		return;
	ItemState state = read_state(item);
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto [known, added] = g_items.try_emplace(obs_sceneitem_get_id(item), state);
		if (!added) {
			if (same(known->second, state))
				return;
			known->second = state;
		}
	}
	const char *scene_name = obs_source_get_name(obs_scene_get_source(scene));
	const char *source_name = obs_source_get_name(obs_sceneitem_get_source(item));
	if (scene_name && source_name && *source_name)
		g_debounce.touch({scene_name, source_name}, now_sec());
}

static bool remember_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	(*static_cast<std::map<int64_t, ItemState> *>(param))[obs_sceneitem_get_id(item)] = read_state(item);
	return true;
}

// Кожен кадр відео: відстояні правки — пушем (обрив релею їх просто відкидає).
static void on_tick(void *, float)
{
	for (const auto &[scene, source] : g_debounce.due(now_sec()))
		relay().push("{\"type\":\"item_transform\",\"scene\":" + json_quote(scene) +
			     ",\"source\":" + json_quote(source) + "}");
}

void scene_watch_start()
{
	obs_add_tick_callback(on_tick, nullptr);
}

void scene_watch_unbind()
{
	obs_weak_source_t *bound = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		bound = g_bound;
		g_bound = nullptr;
		g_items.clear();
	}
	if (!bound)
		return;
	// Без g_mutex: відписка чекає на обробник сигналу, а той бере g_mutex.
	OBSSourceAutoRelease source = obs_weak_source_get_source(bound);
	if (source)
		signal_handler_disconnect(obs_source_get_signal_handler(source), "item_transform", on_item_transform,
					  nullptr);
	obs_weak_source_release(bound);
}

// UI-потік (події фронтенду). Стан елементів знімаємо до підписки і без g_mutex:
// enum_items бере м'ютекс сцени, а обробник сигналу під ним чекає на g_mutex.
void scene_watch_bind()
{
	OBSSourceAutoRelease current = obs_frontend_get_current_scene();
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (current && g_bound && obs_weak_source_references_source(g_bound, current))
			return;
	}
	scene_watch_unbind();
	obs_scene_t *scene = current ? obs_scene_from_source(current) : nullptr;
	if (!scene)
		return;
	std::map<int64_t, ItemState> items;
	obs_scene_enum_items(scene, remember_item, &items);
	signal_handler_connect(obs_source_get_signal_handler(current), "item_transform", on_item_transform, nullptr);
	std::lock_guard<std::mutex> lock(g_mutex);
	g_items = std::move(items);
	g_bound = obs_source_get_weak_source(current);
}

void scene_watch_stop()
{
	scene_watch_unbind();
	obs_remove_tick_callback(on_tick, nullptr);
	g_debounce.clear();
}

} // namespace ebot
