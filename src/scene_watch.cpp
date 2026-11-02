#include "scene_watch.hpp"
#include "core.hpp"
#include "relay_client.hpp"
#include <obs.hpp>
#include <obs-frontend-api.h>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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

// id елемента унікальний лише в межах своєї сцени чи групи — ключ з обох.
using ItemKey = std::pair<const void *, int64_t>;

static std::mutex g_mutex; // g_watched, g_scene, g_items
// Програмна сцена (перша) і групи в ній: правку всередині групи OBS шле
// сигналом самої групи, а не батьківської сцени — камеру часто групують з рамкою.
static std::vector<obs_weak_source_t *> g_watched;
static std::string g_scene; // назва програмної сцени — у пуші й для правок у групах
static std::map<ItemKey, ItemState> g_items; // останній відомий стан елемента
static Debouncer g_debounce;

// ponytail: групи — ті, що були в сцені на момент підв'язки; щойно створена
// група почне пушити після наступної зміни сцени (саме створення — пушить).
// Елементи вкладеної сцени-джерела — лише коли та сцена сама стане програмною.
// Потік відео OBS, під м'ютексом сцени: нічого, що бере його знову (enum_items).
static void on_item_transform(void *, calldata_t *cd)
{
	auto *scene = static_cast<obs_scene_t *>(calldata_ptr(cd, "scene"));
	auto *item = static_cast<obs_sceneitem_t *>(calldata_ptr(cd, "item"));
	if (!scene || !item)
		return;
	ItemState state = read_state(item);
	std::string scene_name;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto [known, added] = g_items.try_emplace(ItemKey{scene, obs_sceneitem_get_id(item)}, state);
		if (!added) {
			if (same(known->second, state))
				return;
			known->second = state;
		}
		scene_name = g_scene;
	}
	const char *source_name = obs_source_get_name(obs_sceneitem_get_source(item));
	if (!scene_name.empty() && source_name && *source_name)
		g_debounce.touch({scene_name, source_name}, now_sec());
}

struct Snapshot {
	std::map<ItemKey, ItemState> items;
	std::vector<OBSSource> groups;
};

static bool remember_item(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	auto *snap = static_cast<Snapshot *>(param);
	snap->items[ItemKey{scene, obs_sceneitem_get_id(item)}] = read_state(item);
	if (obs_sceneitem_is_group(item)) // груп у групах OBS не буває
		snap->groups.emplace_back(obs_sceneitem_get_source(item));
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
	std::vector<obs_weak_source_t *> watched;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		watched.swap(g_watched);
		g_items.clear();
		g_scene.clear();
	}
	// Без g_mutex: відписка чекає на обробник сигналу, а той бере g_mutex.
	for (obs_weak_source_t *weak : watched) {
		OBSSourceAutoRelease source = obs_weak_source_get_source(weak);
		if (source)
			signal_handler_disconnect(obs_source_get_signal_handler(source), "item_transform",
						  on_item_transform, nullptr);
		obs_weak_source_release(weak);
	}
}

// UI-потік (події фронтенду). Стан елементів знімаємо до підписки і без g_mutex:
// enum_items бере м'ютекс сцени, а обробник сигналу під ним чекає на g_mutex.
void scene_watch_bind()
{
	OBSSourceAutoRelease current = obs_frontend_get_current_scene();
	const char *name = current ? obs_source_get_name(current) : nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (current && !g_watched.empty() && obs_weak_source_references_source(g_watched[0], current)) {
			g_scene = name ? name : ""; // та сама сцена, але її могли перейменувати
			return;
		}
	}
	scene_watch_unbind();
	obs_scene_t *scene = current ? obs_scene_from_source(current) : nullptr;
	if (!scene)
		return;
	Snapshot snap;
	obs_scene_enum_items(scene, remember_item, &snap);
	std::vector<OBSSource> groups = std::move(snap.groups);
	std::vector<obs_source_t *> sources{current};
	for (OBSSource &group : groups) {
		obs_scene_enum_items(obs_group_or_scene_from_source(group), remember_item, &snap);
		sources.push_back(group);
	}
	std::vector<obs_weak_source_t *> watched;
	for (obs_source_t *source : sources) {
		signal_handler_connect(obs_source_get_signal_handler(source), "item_transform", on_item_transform,
				       nullptr);
		watched.push_back(obs_source_get_weak_source(source));
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	g_items = std::move(snap.items);
	g_watched = std::move(watched);
	g_scene = name ? name : "";
}

void scene_watch_stop()
{
	scene_watch_unbind();
	obs_remove_tick_callback(on_tick, nullptr);
	g_debounce.clear();
}

} // namespace ebot
