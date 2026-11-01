// Фільтр «єБот: заокруглення камери»: кути джерела прозорі за радіусом, як
// отвір рамки оверлея над ним. Кнопка кабінету «Заокруглити камеру в OBS»
// ставить його командою set_webcam_mask з радіусом рамки, перерахованим у px
// джерела. Фільтр бачить джерело ДО обрізки елемента сцени, тож сервер шле й
// цю обрізку — інакше кути заокруглились би за межами видимої частини.
#include "webcam_mask.hpp"
#include "core.hpp"
#include <obs-module.h>
#include <obs.hpp>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <plugin-support.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ebot {

// Шейдер рядком, а не .effect-файлом: у zip та інсталятор іде лише data/locale
// (.github/workflows/windows.yml). Відстань до заокругленого прямокутника (SDF,
// px): < 0 — всередині. Півпікселя згладжування на краю; рамка — смуга шириною
// border_width уздовж краю. Вихід премультиплікований (блендинг ONE/INVSRCALPHA,
// як у color key OBS): текстура фільтра вже премультиплікована, множимо всі канали.
static const char *MASK_EFFECT = R"(
uniform float4x4 ViewProj;
uniform texture2d image;

uniform float2 mask_size;
uniform float4 mask_box;
uniform float mask_radius;
uniform float border_width;
uniform float4 border_color;

sampler_state textureSampler {
	Filter    = Linear;
	AddressU  = Clamp;
	AddressV  = Clamp;
};

struct VertData {
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
};

VertData VSDefault(VertData v_in)
{
	VertData vert_out;
	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	vert_out.uv  = v_in.uv;
	return vert_out;
}

float4 PSMask(VertData v_in) : TARGET
{
	float4 rgba = image.Sample(textureSampler, v_in.uv);
	float2 half_size = (mask_box.zw - mask_box.xy) * 0.5;
	float2 center = (mask_box.xy + mask_box.zw) * 0.5;
	float2 q = abs(v_in.uv * mask_size - center) - half_size + mask_radius;
	float d = min(max(q.x, q.y), 0.0) + length(max(q, float2(0.0, 0.0))) - mask_radius;
	float4 ring = float4(border_color.rgb * border_color.a, border_color.a);
	float edge = saturate(d + border_width + 0.5) * step(0.5, border_width);
	return lerp(rgba, ring, edge) * saturate(0.5 - d);
}

technique Draw
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMask(v_in);
	}
}
)";

static const char *const S_RADIUS = "radius";
static const char *const S_CROP[4] = {"crop_left", "crop_top", "crop_right", "crop_bottom"};
static const char *const CROP_TEXT[4] = {"WebcamMask.CropLeft", "WebcamMask.CropTop", "WebcamMask.CropRight",
					 "WebcamMask.CropBottom"};
static const char *const CROP_KEYS[4] = {"left", "top", "right", "bottom"}; // поле crop команди
static const char *const S_BORDER_WIDTH = "border_width";
static const char *const S_BORDER_COLOR = "border_color";
constexpr int DEFAULT_RADIUS = 24;
constexpr long long DEFAULT_BORDER_COLOR = 0xFFFFFFFF; // ABGR: білий, непрозорий
constexpr int BORDER_MAX_PX = 64;

struct MaskFilter {
	obs_source_t *context = nullptr;
	gs_effect_t *effect = nullptr;
	gs_eparam_t *size = nullptr;
	gs_eparam_t *box = nullptr;
	gs_eparam_t *radius = nullptr;
	gs_eparam_t *border_width = nullptr;
	gs_eparam_t *border_color = nullptr;
	float radius_px = 0.0f;
	float crop[4] = {}; // зліва, зверху, справа, знизу
	float border_px = 0.0f;
	vec4 color = {};
};

static const char *mask_name(void *)
{
	return obs_module_text("WebcamMask.Name");
}

static void mask_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<MaskFilter *>(data);
	f->radius_px = float(obs_data_get_int(settings, S_RADIUS));
	for (int i = 0; i < 4; ++i)
		f->crop[i] = float(obs_data_get_int(settings, S_CROP[i]));
	f->border_px = float(obs_data_get_int(settings, S_BORDER_WIDTH));
	vec4_from_rgba(&f->color, uint32_t(obs_data_get_int(settings, S_BORDER_COLOR)));
}

static void mask_destroy(void *data)
{
	auto *f = static_cast<MaskFilter *>(data);
	if (f->effect) {
		obs_enter_graphics();
		gs_effect_destroy(f->effect);
		obs_leave_graphics();
	}
	delete f;
}

static void *mask_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new MaskFilter();
	f->context = source;
	char *errors = nullptr;
	obs_enter_graphics();
	f->effect = gs_effect_create(MASK_EFFECT, "ebot_webcam_mask.effect", &errors);
	obs_leave_graphics();
	if (!f->effect) {
		obs_log(LOG_ERROR, "webcam mask shader failed: %s", errors ? errors : "(no details)");
		bfree(errors);
		delete f;
		return nullptr;
	}
	bfree(errors);
	f->size = gs_effect_get_param_by_name(f->effect, "mask_size");
	f->box = gs_effect_get_param_by_name(f->effect, "mask_box");
	f->radius = gs_effect_get_param_by_name(f->effect, "mask_radius");
	f->border_width = gs_effect_get_param_by_name(f->effect, "border_width");
	f->border_color = gs_effect_get_param_by_name(f->effect, "border_color");
	mask_update(f, settings);
	return f;
}

static void mask_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_RADIUS, DEFAULT_RADIUS);
	obs_data_set_default_int(settings, S_BORDER_COLOR, DEFAULT_BORDER_COLOR);
}

static obs_properties_t *mask_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p =
		obs_properties_add_int(props, S_RADIUS, obs_module_text("WebcamMask.Radius"), 0, WEBCAM_MASK_MAX_PX, 1);
	obs_property_int_set_suffix(p, " px");
	for (int i = 0; i < 4; ++i) {
		p = obs_properties_add_int(props, S_CROP[i], obs_module_text(CROP_TEXT[i]), 0, WEBCAM_MASK_MAX_PX, 1);
		obs_property_int_set_suffix(p, " px");
		obs_property_set_long_description(p, obs_module_text("WebcamMask.CropHint"));
	}
	p = obs_properties_add_int_slider(props, S_BORDER_WIDTH, obs_module_text("WebcamMask.BorderWidth"), 0,
					  BORDER_MAX_PX, 1);
	obs_property_int_set_suffix(p, " px");
	obs_properties_add_color_alpha(props, S_BORDER_COLOR, obs_module_text("WebcamMask.BorderColor"));
	return props;
}

static void mask_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<MaskFilter *>(data);
	obs_source_t *target = obs_filter_get_target(f->context);
	float w = float(target ? obs_source_get_base_width(target) : 0);
	float h = float(target ? obs_source_get_base_height(target) : 0);
	float x0 = f->crop[0], y0 = f->crop[1], x1 = w - f->crop[2], y1 = h - f->crop[3];
	// Обрізка «з'їла» все джерело або фільтру нічого робити — віддаємо як є.
	bool idle = f->radius_px < 0.5f && f->border_px < 0.5f;
	if (idle || x1 - x0 < 1.0f || y1 - y0 < 1.0f) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	if (!obs_source_process_filter_begin(f->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		return;
	vec2 size;
	vec2_set(&size, w, h);
	vec4 box;
	vec4_set(&box, x0, y0, x1, y1);
	gs_effect_set_vec2(f->size, &size);
	gs_effect_set_vec4(f->box, &box);
	// Радіус більший за половину меншої сторони ламає SDF — тоді це просто овал.
	gs_effect_set_float(f->radius, std::min(f->radius_px, std::min(x1 - x0, y1 - y0) / 2.0f));
	gs_effect_set_float(f->border_width, f->border_px);
	gs_effect_set_vec4(f->border_color, &f->color);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_end(f->context, f->effect, 0, 0);
	gs_blend_state_pop();
}

void register_webcam_mask()
{
	obs_source_info info = {};
	info.id = WEBCAM_MASK_FILTER_ID;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = mask_name;
	info.create = mask_create;
	info.destroy = mask_destroy;
	info.update = mask_update;
	info.get_defaults = mask_defaults;
	info.get_properties = mask_properties;
	info.video_render = mask_render;
	obs_register_source(&info);
}

// Під м'ютексом фільтрів джерела: лише бере посилання, нічого не викликає.
static void find_mask(obs_source_t *, obs_source_t *filter, void *param)
{
	auto *found = static_cast<obs_source_t **>(param);
	const char *id = obs_source_get_id(filter);
	if (!*found && id && std::strcmp(id, WEBCAM_MASK_FILTER_ID) == 0)
		*found = obs_source_get_ref(filter);
}

std::string set_webcam_mask(obs_data_t *params)
{
	std::string name = obs_data_get_string(params, "source");
	OBSSourceAutoRelease source = obs_get_source_by_name(name.c_str());
	if (!source)
		throw std::runtime_error("No source was found by the name of `" + name + "`");
	// За id, а не назвою: стример міг перейменувати фільтр у вікні фільтрів OBS.
	obs_source_t *found = nullptr;
	obs_source_enum_filters(source, find_mask, &found);
	OBSSourceAutoRelease filter = found;
	if (!obs_data_get_bool(params, "enabled")) {
		if (filter)
			obs_source_filter_remove(source, filter);
		return "{\"success\":true}";
	}
	// Рамку фільтра (ширину й колір) сервер не задає — їх стример міг виставити сам,
	// obs_source_update міняє лише передані ключі.
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, S_RADIUS, clamp_mask_px(obs_data_get_double(params, "radius")));
	OBSDataAutoRelease crop = obs_data_get_obj(params, "crop");
	for (int i = 0; i < 4; ++i)
		obs_data_set_int(settings, S_CROP[i], crop ? clamp_mask_px(obs_data_get_double(crop, CROP_KEYS[i])) : 0);
	bool created = !filter;
	if (created) {
		filter = obs_source_create(WEBCAM_MASK_FILTER_ID, obs_module_text("WebcamMask.Name"), settings, nullptr);
		if (!filter)
			throw std::runtime_error("OBS did not create the webcam mask filter");
		obs_source_filter_add(source, filter); // фільтр бере своє посилання
	} else {
		obs_source_update(filter, settings);
	}
	obs_source_set_enabled(filter, true); // стример міг вимкнути його оком у вікні фільтрів
	return std::string("{\"success\":true,\"created\":") + (created ? "true" : "false") + "}";
}

} // namespace ebot
