#include "hr-ble-source.hpp"

#include "ble/ble_manager.hpp"

#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr int kDefaultWidth = 320;
constexpr int kDefaultHeight = 160;
constexpr int kDataTimeoutMs = 5000;
constexpr int kScanMs = 4000;
constexpr char kGitHubRepoUrl[] = "https://github.com/Discrutans/obs-ble-hr";
constexpr char kSupportUrl[] = "https://dalink.to/discrutans";

struct hr_source {
	obs_source_t *source = nullptr;

	std::unique_ptr<ble_hr::BleHeartRateManager> ble;

	std::mutex ui_mutex;
	std::vector<ble_hr::DeviceInfo> devices;
	std::string selected_id;
	std::string selected_name;
	std::string status_text;

	std::atomic<int> bpm{0};
	std::atomic<bool> bpm_valid{false};

	uint32_t width = kDefaultWidth;
	uint32_t height = kDefaultHeight;
	int zone_rest_max = 100;
	int zone_load_max = 140;
	uint32_t color_rest = 0xFF3DDC84;
	uint32_t color_load = 0xFFFFD54F;
	uint32_t color_peak = 0xFFFF5252;
	float heart_size = 56.0f;
	float heart_aspect = 1.0f;
	float heart_offset_x = 0.0f;
	float heart_offset_y = 0.0f;
	float pulse_amount = 0.12f;
	int heart_style = 0; /* 0=fill, 1=outline, 2=both */
	float outline_width = 0.10f;
	uint32_t outline_color = 0xFF000000;
	bool outline_follow_zone = false;

	int text_position = 0; /* 0=beside_right, 1=beside_left, 2=inside */
	float text_scale = 1.0f;
	float text_offset_x = 0.0f;
	float text_offset_y = 0.0f;
	bool text_follow_zone = true;
	uint32_t text_color = 0xFFFFFFFF;

	double pulse_phase = 0.0;

	obs_source_t *text_source = nullptr;
	std::string last_text;
	uint32_t last_text_color = 0;
};

static void vec4_from_color(vec4 *out, uint32_t color)
{
	vec4_set(out, (float)((color >> 16) & 0xFF) / 255.0f, (float)((color >> 8) & 0xFF) / 255.0f,
		 (float)(color & 0xFF) / 255.0f, (float)((color >> 24) & 0xFF) / 255.0f);
}

static uint32_t zone_color(const hr_source *ctx, int bpm)
{
	if (bpm <= ctx->zone_rest_max)
		return ctx->color_rest;
	if (bpm <= ctx->zone_load_max)
		return ctx->color_load;
	return ctx->color_peak;
}

static const char *text_source_id()
{
#ifdef _WIN32
	return "text_gdiplus";
#else
	return "text_ft2_source";
#endif
}

static void ensure_text_source(hr_source *ctx)
{
	if (ctx->text_source)
		return;

	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "outline", true);
	obs_data_set_int(settings, "outline_size", 2);
	obs_data_set_int(settings, "outline_color", 0xFF000000);
	obs_data_set_int(settings, "outline_opacity", 100);
	obs_data_set_string(settings, "text", "--");
	obs_data_set_int(settings, "color", 0xFFFFFFFF);
	obs_data_set_bool(settings, "extents", false);
#ifdef _WIN32
	obs_data_set_string(settings, "font", "Segoe UI");
	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", "Segoe UI");
	obs_data_set_int(font, "size", 72);
	obs_data_set_int(font, "flags", 1);
	obs_data_set_obj(settings, "font", font);
	obs_data_release(font);
#else
	obs_data_set_bool(settings, "drop_shadow", true);
	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", "Arial");
	obs_data_set_int(font, "size", 72);
	obs_data_set_obj(settings, "font", font);
	obs_data_release(font);
#endif

	ctx->text_source = obs_source_create_private(text_source_id(), "ble-hr-text", settings);
	obs_data_release(settings);
}

static void update_text_source(hr_source *ctx, const char *text, uint32_t color)
{
	ensure_text_source(ctx);
	if (!ctx->text_source)
		return;

	if (ctx->last_text == text && ctx->last_text_color == color)
		return;

	ctx->last_text = text;
	ctx->last_text_color = color;

	obs_data_t *settings = obs_source_get_settings(ctx->text_source);
	obs_data_set_string(settings, "text", text);
	obs_data_set_int(settings, "color", color);
	obs_data_set_int(settings, "color1", color);
	obs_data_set_int(settings, "color2", color);
	obs_source_update(ctx->text_source, settings);
	obs_data_release(settings);
}

static void heart_point(float t, float sx, float sy, float *ox, float *oy)
{
	*ox = 16.0f * powf(sinf(t), 3.0f) * sx;
	*oy = -(13.0f * cosf(t) - 5.0f * cosf(2.0f * t) - 2.0f * cosf(3.0f * t) - cosf(4.0f * t)) * sy;
}

static void draw_heart_shape(float cx, float cy, float size, float aspect, uint32_t color, float scale)
{
	const float s = size * scale;
	const float sx = s / 32.0f * aspect;
	const float sy = s / 32.0f;
	constexpr int kSegments = 64;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");
	vec4 c;
	vec4_from_color(&c, color);
	gs_effect_set_vec4(color_param, &c);

	gs_render_start(true);
	for (int i = 0; i < kSegments; ++i) {
		const float t0 = (float)i / (float)kSegments * (float)(2.0 * M_PI);
		const float t1 = (float)(i + 1) / (float)kSegments * (float)(2.0 * M_PI);
		float x0, y0, x1, y1;
		heart_point(t0, sx, sy, &x0, &y0);
		heart_point(t1, sx, sy, &x1, &y1);
		gs_vertex2f(cx, cy);
		gs_vertex2f(cx + x0, cy + y0);
		gs_vertex2f(cx + x1, cy + y1);
	}
	gs_vertbuffer_t *tris = gs_render_save();
	if (!tris)
		return;

	while (gs_effect_loop(solid, "Solid")) {
		gs_load_vertexbuffer(tris);
		gs_draw(GS_TRIS, 0, 0);
	}

	gs_vertexbuffer_destroy(tris);
	gs_load_vertexbuffer(nullptr);
}

static void draw_heart(const hr_source *ctx, float cx, float cy, uint32_t fill_color, float pulse_scale)
{
	const float aspect = std::max(0.4f, ctx->heart_aspect);
	const float outline_pad = std::max(0.02f, ctx->outline_width);
	const uint32_t outline = ctx->outline_follow_zone ? fill_color : ctx->outline_color;

	if (ctx->heart_style == 1) {
		/* Outline only: outer ring, then punch transparent hole */
		draw_heart_shape(cx, cy, ctx->heart_size, aspect, outline, pulse_scale * (1.0f + outline_pad));
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ZERO, GS_BLEND_ZERO);
		draw_heart_shape(cx, cy, ctx->heart_size, aspect, 0x00000000,
				 pulse_scale * std::max(0.45f, 1.0f - outline_pad * 0.35f));
		gs_blend_state_pop();
		return;
	}

	if (ctx->heart_style == 2) {
		draw_heart_shape(cx, cy, ctx->heart_size, aspect, outline, pulse_scale * (1.0f + outline_pad));
	}

	draw_heart_shape(cx, cy, ctx->heart_size, aspect, fill_color, pulse_scale);
}

static void refresh_device_list_property(obs_property_t *list, hr_source *ctx)
{
	obs_property_list_clear(list);

	std::string selected;
	std::vector<ble_hr::DeviceInfo> devices;
	{
		std::lock_guard<std::mutex> lock(ctx->ui_mutex);
		devices = ctx->devices;
		selected = ctx->selected_id;
		if (!ctx->selected_name.empty() && !selected.empty()) {
			bool present = false;
			for (const auto &d : devices) {
				if (d.id == selected) {
					present = true;
					break;
				}
			}
			if (!present) {
				ble_hr::DeviceInfo remembered;
				remembered.id = selected;
				remembered.name = ctx->selected_name;
				remembered.rssi = 0;
				devices.insert(devices.begin(), remembered);
			}
		}
	}

	obs_property_list_add_string(list, obs_module_text("DeviceNone"), "");

	for (const auto &d : devices) {
		char label[256];
		if (d.rssi != 0)
			snprintf(label, sizeof(label), "%s  (%d dBm)", d.name.c_str(), d.rssi);
		else
			snprintf(label, sizeof(label), "%s", d.name.c_str());
		obs_property_list_add_string(list, label, d.id.c_str());
	}

	UNUSED_PARAMETER(selected);
}

static bool scan_clicked(obs_properties_t *props, obs_property_t *, void *data)
{
	auto *ctx = static_cast<hr_source *>(data);
	if (!ctx || !ctx->ble)
		return false;

	{
		std::lock_guard<std::mutex> lock(ctx->ui_mutex);
		ctx->status_text = obs_module_text("StatusScanning");
	}

	auto found = ctx->ble->scan(std::chrono::milliseconds(kScanMs));

	{
		std::lock_guard<std::mutex> lock(ctx->ui_mutex);
		ctx->devices = std::move(found);
		char buf[128];
		snprintf(buf, sizeof(buf), obs_module_text("StatusFound"), (int)ctx->devices.size());
		ctx->status_text = buf;
	}

	obs_property_t *list = obs_properties_get(props, "device_id");
	if (list)
		refresh_device_list_property(list, ctx);

	return true;
}

static bool device_changed(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	return true;
}

static const char *hr_get_name(void *)
{
	return obs_module_text("HeartRateBle");
}

static void *hr_create(obs_data_t *settings, obs_source_t *source)
{
	auto *ctx = new hr_source();
	ctx->source = source;
	ctx->ble = std::make_unique<ble_hr::BleHeartRateManager>();

	ctx->ble->set_bpm_callback([ctx](int bpm) {
		ctx->bpm.store(bpm, std::memory_order_relaxed);
		ctx->bpm_valid.store(true, std::memory_order_relaxed);
	});

	ctx->ble->set_state_callback([ctx](ble_hr::ConnectionState state, const std::string &message) {
		std::lock_guard<std::mutex> lock(ctx->ui_mutex);
		switch (state) {
		case ble_hr::ConnectionState::Connected:
			ctx->status_text = obs_module_text("StatusConnected");
			break;
		case ble_hr::ConnectionState::Connecting:
			ctx->status_text = obs_module_text("StatusConnecting");
			break;
		case ble_hr::ConnectionState::Reconnecting:
			ctx->status_text = obs_module_text("StatusReconnecting");
			break;
		case ble_hr::ConnectionState::Disconnected:
			ctx->status_text = message.empty() ? obs_module_text("StatusDisconnected") : message;
			break;
		}
	});

	obs_source_update(source, settings);
	return ctx;
}

static void hr_destroy(void *data)
{
	auto *ctx = static_cast<hr_source *>(data);
	if (ctx->ble)
		ctx->ble->disconnect();
	if (ctx->text_source)
		obs_source_release(ctx->text_source);
	delete ctx;
}

static void hr_update(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<hr_source *>(data);

	ctx->width = (uint32_t)std::max(64, (int)obs_data_get_int(settings, "width"));
	ctx->height = (uint32_t)std::max(64, (int)obs_data_get_int(settings, "height"));
	ctx->zone_rest_max = (int)obs_data_get_int(settings, "zone_rest_max");
	ctx->zone_load_max = (int)obs_data_get_int(settings, "zone_load_max");
	if (ctx->zone_load_max < ctx->zone_rest_max)
		ctx->zone_load_max = ctx->zone_rest_max;

	ctx->color_rest = (uint32_t)obs_data_get_int(settings, "color_rest");
	ctx->color_load = (uint32_t)obs_data_get_int(settings, "color_load");
	ctx->color_peak = (uint32_t)obs_data_get_int(settings, "color_peak");
	ctx->heart_size = (float)obs_data_get_double(settings, "heart_size");
	ctx->heart_aspect = (float)obs_data_get_double(settings, "heart_aspect");
	ctx->heart_offset_x = (float)obs_data_get_double(settings, "heart_offset_x");
	ctx->heart_offset_y = (float)obs_data_get_double(settings, "heart_offset_y");
	ctx->pulse_amount = (float)obs_data_get_double(settings, "pulse_amount");
	ctx->heart_style = (int)obs_data_get_int(settings, "heart_style");
	ctx->outline_width = (float)obs_data_get_double(settings, "outline_width");
	ctx->outline_color = (uint32_t)obs_data_get_int(settings, "outline_color");
	ctx->outline_follow_zone = obs_data_get_bool(settings, "outline_follow_zone");

	ctx->text_position = (int)obs_data_get_int(settings, "text_position");
	ctx->text_scale = (float)obs_data_get_double(settings, "text_scale");
	ctx->text_offset_x = (float)obs_data_get_double(settings, "text_offset_x");
	ctx->text_offset_y = (float)obs_data_get_double(settings, "text_offset_y");
	ctx->text_follow_zone = obs_data_get_bool(settings, "text_follow_zone");
	ctx->text_color = (uint32_t)obs_data_get_int(settings, "text_color");

	const char *device_id = obs_data_get_string(settings, "device_id");
	const char *device_name = obs_data_get_string(settings, "device_name");

	std::string new_id = device_id ? device_id : "";
	std::string new_name = device_name ? device_name : "";

	{
		std::lock_guard<std::mutex> lock(ctx->ui_mutex);
		if (!new_id.empty() && new_name.empty()) {
			for (const auto &d : ctx->devices) {
				if (d.id == new_id) {
					new_name = d.name;
					break;
				}
			}
		}
		ctx->selected_id = new_id;
		if (!new_name.empty())
			ctx->selected_name = new_name;
	}

	if (!new_name.empty())
		obs_data_set_string(settings, "device_name", new_name.c_str());

	if (!new_id.empty())
		ctx->ble->connect(new_id);
	else
		ctx->ble->disconnect();
}

static uint32_t hr_width(void *data)
{
	return static_cast<hr_source *>(data)->width;
}

static uint32_t hr_height(void *data)
{
	return static_cast<hr_source *>(data)->height;
}

static void hr_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", kDefaultWidth);
	obs_data_set_default_int(settings, "height", kDefaultHeight);
	obs_data_set_default_int(settings, "zone_rest_max", 100);
	obs_data_set_default_int(settings, "zone_load_max", 140);
	obs_data_set_default_int(settings, "color_rest", 0xFF3DDC84);
	obs_data_set_default_int(settings, "color_load", 0xFFFFD54F);
	obs_data_set_default_int(settings, "color_peak", 0xFFFF5252);

	obs_data_set_default_double(settings, "heart_size", 72.0);
	obs_data_set_default_double(settings, "heart_aspect", 1.0);
	obs_data_set_default_double(settings, "heart_offset_x", 0.0);
	obs_data_set_default_double(settings, "heart_offset_y", 0.0);
	obs_data_set_default_double(settings, "pulse_amount", 0.12);
	obs_data_set_default_int(settings, "heart_style", 0);
	obs_data_set_default_double(settings, "outline_width", 0.10);
	obs_data_set_default_int(settings, "outline_color", 0xFF000000);
	obs_data_set_default_bool(settings, "outline_follow_zone", false);

	obs_data_set_default_int(settings, "text_position", 0);
	obs_data_set_default_double(settings, "text_scale", 1.0);
	obs_data_set_default_double(settings, "text_offset_x", 0.0);
	obs_data_set_default_double(settings, "text_offset_y", 0.0);
	obs_data_set_default_bool(settings, "text_follow_zone", true);
	obs_data_set_default_int(settings, "text_color", 0xFFFFFFFF);

	obs_data_set_default_string(settings, "device_id", "");
	obs_data_set_default_string(settings, "device_name", "");
}

static bool heart_style_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const int style = (int)obs_data_get_int(settings, "heart_style");
	const bool show_outline = style == 1 || style == 2;
	obs_property_set_visible(obs_properties_get(props, "outline_width"), show_outline);
	obs_property_set_visible(obs_properties_get(props, "outline_color"), show_outline);
	obs_property_set_visible(obs_properties_get(props, "outline_follow_zone"), show_outline);
	return true;
}

static bool text_follow_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool follow = obs_data_get_bool(settings, "text_follow_zone");
	obs_property_set_visible(obs_properties_get(props, "text_color"), !follow);
	return true;
}

static obs_properties_t *hr_properties(void *data)
{
	auto *ctx = static_cast<hr_source *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_button(props, "scan", obs_module_text("ScanDevices"), scan_clicked);

	obs_property_t *list =
		obs_properties_add_list(props, "device_id", obs_module_text("Device"), OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(list, device_changed);
	if (ctx)
		refresh_device_list_property(list, ctx);

	obs_properties_add_text(props, "device_name", obs_module_text("DeviceName"), OBS_TEXT_DEFAULT);
	obs_property_set_visible(obs_properties_get(props, "device_name"), false);

	obs_properties_add_int(props, "width", obs_module_text("Width"), 64, 1920, 1);
	obs_properties_add_int(props, "height", obs_module_text("Height"), 64, 1080, 1);

	/* Heart */
	obs_property_t *style = obs_properties_add_list(props, "heart_style", obs_module_text("HeartStyle"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(style, obs_module_text("HeartStyleFill"), 0);
	obs_property_list_add_int(style, obs_module_text("HeartStyleOutline"), 1);
	obs_property_list_add_int(style, obs_module_text("HeartStyleBoth"), 2);
	obs_property_set_modified_callback(style, heart_style_modified);

	obs_properties_add_float_slider(props, "heart_size", obs_module_text("HeartSize"), 24.0, 240.0, 1.0);
	obs_properties_add_float_slider(props, "heart_aspect", obs_module_text("HeartAspect"), 0.5, 1.6, 0.01);
	obs_properties_add_float_slider(props, "heart_offset_x", obs_module_text("HeartOffsetX"), -400.0, 400.0, 1.0);
	obs_properties_add_float_slider(props, "heart_offset_y", obs_module_text("HeartOffsetY"), -400.0, 400.0, 1.0);
	obs_properties_add_float_slider(props, "pulse_amount", obs_module_text("PulseAmount"), 0.0, 0.35, 0.01);
	obs_properties_add_float_slider(props, "outline_width", obs_module_text("OutlineWidth"), 0.02, 0.30, 0.01);
	obs_properties_add_color(props, "outline_color", obs_module_text("OutlineColor"));
	obs_properties_add_bool(props, "outline_follow_zone", obs_module_text("OutlineFollowZone"));

	/* Text */
	obs_property_t *text_pos = obs_properties_add_list(props, "text_position", obs_module_text("TextPosition"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(text_pos, obs_module_text("TextBesideRight"), 0);
	obs_property_list_add_int(text_pos, obs_module_text("TextBesideLeft"), 1);
	obs_property_list_add_int(text_pos, obs_module_text("TextInside"), 2);

	obs_properties_add_float_slider(props, "text_scale", obs_module_text("TextScale"), 0.3, 3.0, 0.05);
	obs_properties_add_float_slider(props, "text_offset_x", obs_module_text("TextOffsetX"), -400.0, 400.0, 1.0);
	obs_properties_add_float_slider(props, "text_offset_y", obs_module_text("TextOffsetY"), -400.0, 400.0, 1.0);

	obs_property_t *follow = obs_properties_add_bool(props, "text_follow_zone", obs_module_text("TextFollowZone"));
	obs_property_set_modified_callback(follow, text_follow_modified);
	obs_properties_add_color(props, "text_color", obs_module_text("TextColor"));

	/* Zones */
	obs_properties_add_int(props, "zone_rest_max", obs_module_text("ZoneRestMax"), 40, 220, 1);
	obs_properties_add_int(props, "zone_load_max", obs_module_text("ZoneLoadMax"), 40, 220, 1);
	obs_properties_add_color(props, "color_rest", obs_module_text("ColorRest"));
	obs_properties_add_color(props, "color_load", obs_module_text("ColorLoad"));
	obs_properties_add_color(props, "color_peak", obs_module_text("ColorPeak"));

	/* One row of links (OBS stacks native buttons vertically) */
	char links_html[768];
	snprintf(links_html, sizeof(links_html),
		 "<p style=\"margin:6px 0; white-space:nowrap\">"
		 "<a href=\"%s\">%s</a>"
		 "&nbsp;&nbsp;&nbsp;|&nbsp;&nbsp;&nbsp;"
		 "<a href=\"%s\">%s</a>"
		 "</p>",
		 kSupportUrl, obs_module_text("OpenSupport"), kGitHubRepoUrl, obs_module_text("OpenGitHub"));
	obs_property_t *links = obs_properties_add_text(props, "author_links", links_html, OBS_TEXT_INFO);
	obs_property_text_set_info_type(links, OBS_TEXT_INFO_NORMAL);

	return props;
}

static void hr_video_tick(void *data, float seconds)
{
	auto *ctx = static_cast<hr_source *>(data);

	const bool alive = ctx->ble && ctx->ble->has_recent_bpm(std::chrono::milliseconds(kDataTimeoutMs));
	if (!alive) {
		ctx->bpm_valid.store(false, std::memory_order_relaxed);
		ctx->pulse_phase = 0.0;
		return;
	}

	const int bpm = ctx->bpm.load(std::memory_order_relaxed);
	if (bpm <= 0) {
		ctx->pulse_phase = 0.0;
		return;
	}

	const double beat_hz = (double)bpm / 60.0;
	ctx->pulse_phase += (double)seconds * beat_hz * 2.0 * M_PI;
	if (ctx->pulse_phase > 2.0 * M_PI * 1000.0)
		ctx->pulse_phase = fmod(ctx->pulse_phase, 2.0 * M_PI);

	UNUSED_PARAMETER(seconds);
}

static void hr_video_render(void *data, gs_effect_t *)
{
	auto *ctx = static_cast<hr_source *>(data);

	const bool valid = ctx->bpm_valid.load(std::memory_order_relaxed);
	const int bpm = ctx->bpm.load(std::memory_order_relaxed);
	const uint32_t zone = valid ? zone_color(ctx, bpm) : 0xFF9E9E9E;
	const uint32_t bpm_color = ctx->text_follow_zone ? zone : ctx->text_color;

	char text[32];
	if (valid)
		snprintf(text, sizeof(text), "%d", bpm);
	else
		snprintf(text, sizeof(text), "--");

	update_text_source(ctx, text, bpm_color);

	const float cx = (float)ctx->width * 0.5f + ctx->heart_offset_x;
	const float cy = (float)ctx->height * 0.5f + ctx->heart_offset_y;
	const float pulse = valid ? (ctx->pulse_amount * (0.5f + 0.5f * (float)sin(ctx->pulse_phase))) : 0.0f;
	const float scale = 1.0f + pulse;

	draw_heart(ctx, cx, cy, zone, scale);

	if (!ctx->text_source)
		return;

	const uint32_t tw = obs_source_get_width(ctx->text_source);
	const uint32_t th = obs_source_get_height(ctx->text_source);
	if (tw == 0 || th == 0)
		return;

	const float scaled_w = (float)tw * ctx->text_scale;
	const float scaled_h = (float)th * ctx->text_scale;
	float tx = 0.0f;
	float ty = 0.0f;

	if (ctx->text_position == 2) {
		/* Inside heart */
		tx = cx - scaled_w * 0.5f + ctx->text_offset_x;
		ty = cy - scaled_h * 0.45f + ctx->text_offset_y;
	} else if (ctx->text_position == 1) {
		/* Beside left */
		const float gap = ctx->heart_size * scale * 0.55f + 12.0f;
		tx = cx - gap - scaled_w + ctx->text_offset_x;
		ty = cy - scaled_h * 0.5f + ctx->text_offset_y;
	} else {
		/* Beside right */
		const float gap = ctx->heart_size * scale * 0.55f + 12.0f;
		tx = cx + gap + ctx->text_offset_x;
		ty = cy - scaled_h * 0.5f + ctx->text_offset_y;
	}

	gs_matrix_push();
	gs_matrix_translate3f(tx, ty, 0.0f);
	gs_matrix_scale3f(ctx->text_scale, ctx->text_scale, 1.0f);
	obs_source_video_render(ctx->text_source);
	gs_matrix_pop();
}

} /* namespace */

struct obs_source_info hr_ble_source_info = {
	.id = "ble_heart_rate_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
	.get_name = hr_get_name,
	.create = hr_create,
	.destroy = hr_destroy,
	.get_width = hr_width,
	.get_height = hr_height,
	.get_defaults = hr_defaults,
	.get_properties = hr_properties,
	.update = hr_update,
	.video_tick = hr_video_tick,
	.video_render = hr_video_render,
};
