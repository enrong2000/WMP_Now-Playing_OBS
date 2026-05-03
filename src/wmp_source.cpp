#include "wmp_source.hpp"

#include "smtc_monitor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <obs-module.h>
#include <sstream>
#include <string>
#include <string_view>

namespace obs_wmp {
namespace {

constexpr const char *kDefaultFormat =
	"{artist} - {title}\n"
	"{album}\n"
	"{position} / {duration} {progress_bar} {status}";

struct SourceContext {
	obs_source_t *source = nullptr;
	obs_source_t *text_source = nullptr;
	SmtcMonitor monitor;
	std::string app_filter = "wmplayer";
	std::string format = kDefaultFormat;
	bool hide_when_empty = false;
	bool enable_wmp_window_fallback = true;
	uint32_t refresh_ms = 1000;
	int progress_width = 24;
	float update_elapsed = 0.0f;
	std::string last_text;
};

std::string fallback(std::string value, const char *text)
{
	if (value.empty())
		return text;
	return value;
}

void replace_all(std::string &value, std::string_view token,
		 std::string_view replacement)
{
	size_t offset = 0;
	while ((offset = value.find(token, offset)) != std::string::npos) {
		value.replace(offset, token.size(), replacement);
		offset += replacement.size();
	}
}

std::string format_time(int64_t ms)
{
	if (ms < 0)
		ms = 0;

	const auto total_seconds = ms / 1000;
	const auto hours = total_seconds / 3600;
	const auto minutes = (total_seconds / 60) % 60;
	const auto seconds = total_seconds % 60;

	std::ostringstream stream;
	if (hours > 0) {
		stream << hours << ':';
		if (minutes < 10)
			stream << '0';
		stream << minutes << ':';
	} else {
		stream << minutes << ':';
	}

	if (seconds < 10)
		stream << '0';
	stream << seconds;

	return stream.str();
}

std::string join_strings(const std::vector<std::string> &values)
{
	std::ostringstream stream;
	bool first = true;

	for (const auto &value : values) {
		if (value.empty())
			continue;
		if (!first)
			stream << ", ";
		first = false;
		stream << value;
	}

	return stream.str();
}

int64_t playable_start(const MediaState &state)
{
	if (state.start_ms > 0)
		return state.start_ms;
	return state.min_seek_ms;
}

int64_t playable_end(const MediaState &state)
{
	if (state.end_ms > state.start_ms)
		return state.end_ms;
	return state.max_seek_ms;
}

int64_t current_position_ms(const MediaState &state)
{
	int64_t position = state.position_ms;

	if (state.playback_status == PlaybackStatus::playing) {
		const auto elapsed = std::chrono::steady_clock::now() -
				     state.captured_at;
		position += std::chrono::duration_cast<std::chrono::milliseconds>(
				    elapsed)
				    .count();
	}

	const auto start = playable_start(state);
	const auto end = playable_end(state);
	if (end > start)
		position = std::clamp(position, start, end);

	return position;
}

std::string progress_bar(double ratio, int width)
{
	width = std::clamp(width, 4, 80);
	ratio = std::clamp(ratio, 0.0, 1.0);

	const auto filled =
		static_cast<int>(std::llround(ratio * static_cast<double>(width)));

	std::string bar = "[";
	bar.append(static_cast<size_t>(filled), '#');
	bar.append(static_cast<size_t>(width - filled), '-');
	bar.push_back(']');
	return bar;
}

std::string render_text(const SourceContext &context, const MediaState &state)
{
	if (!state.available) {
		if (context.hide_when_empty)
			return {};

		if (!state.active_sessions.empty()) {
			std::string text =
				"Waiting for matching SMTC media session\nSMTC sessions: " +
				join_strings(state.active_sessions);

			if (!state.legacy_wmp_windows.empty()) {
				text += "\nWMP windows: " +
					join_strings(state.legacy_wmp_windows);
			}

			return text;
		}

		return "Waiting for Windows Media Player";
	}

	const auto start = playable_start(state);
	const auto end = playable_end(state);
	const auto duration = end > start ? end - start : 0;
	const auto absolute_position = current_position_ms(state);
	const auto relative_position =
		duration > 0 ? std::clamp(absolute_position - start, int64_t{0},
					  duration)
			     : std::max<int64_t>(absolute_position, 0);
	const auto remaining = duration > 0
				       ? std::max<int64_t>(duration - relative_position,
							   0)
				       : 0;
	const auto ratio = duration > 0
				   ? static_cast<double>(relative_position) /
					     static_cast<double>(duration)
				   : 0.0;

	std::ostringstream percent;
	percent << static_cast<int>(std::llround(ratio * 100.0)) << '%';

	std::string output = context.format;
	replace_all(output, "{title}", fallback(state.title, "Unknown title"));
	replace_all(output, "{artist}", fallback(state.artist, "Unknown artist"));
	replace_all(output, "{album}", state.album);
	replace_all(output, "{album_artist}", state.album_artist);
	replace_all(output, "{subtitle}", state.subtitle);
	replace_all(output, "{genres}", join_strings(state.genres));
	replace_all(output, "{backend}", state.backend);
	replace_all(output, "{source_app_id}", state.source_app_id);
	replace_all(output, "{status}",
		    playback_status_text(state.playback_status));
	replace_all(output, "{position}",
		    state.timeline_available ? format_time(relative_position)
					     : "--:--");
	replace_all(output, "{duration}",
		    duration > 0 ? format_time(duration) : "--:--");
	replace_all(output, "{remaining}",
		    duration > 0 ? format_time(remaining) : "--:--");
	replace_all(output, "{progress_percent}", percent.str());
	replace_all(output, "{progress_bar}",
		    progress_bar(ratio, context.progress_width));
	replace_all(output, "{sessions}", join_strings(state.active_sessions));
	replace_all(output, "{diagnostic}", state.error_message);
	replace_all(output, "{wmp_windows}",
		    join_strings(state.legacy_wmp_windows));

	return output;
}

obs_source_t *create_text_source()
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", "");

	obs_source_t *source = obs_source_create_private(
		"text_gdiplus_v2", "obs-wmp-smtc internal text", settings);
	if (!source) {
		source = obs_source_create_private(
			"text_gdiplus", "obs-wmp-smtc internal text", settings);
	}

	if (!source) {
		blog(LOG_WARNING,
		     "[obs-wmp-smtc] OBS Windows text source is unavailable");
	}

	obs_data_release(settings);
	return source;
}

void update_text_source(SourceContext *context, const std::string &text)
{
	if (!context->text_source)
		return;

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", text.c_str());
	obs_source_update(context->text_source, settings);
	obs_data_release(settings);
}

const char *source_get_name(void *)
{
	return "WMP Legacy Now Playing (SMTC)";
}

void source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "app_filter", "wmplayer");
	obs_data_set_default_string(settings, "format", kDefaultFormat);
	obs_data_set_default_bool(settings, "hide_when_empty", false);
	obs_data_set_default_bool(settings, "enable_wmp_window_fallback", true);
	obs_data_set_default_int(settings, "refresh_ms", 1000);
	obs_data_set_default_int(settings, "progress_width", 24);
}

obs_properties_t *source_get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "app_filter", "App filter",
				OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "format", "Format",
				OBS_TEXT_MULTILINE);
	obs_properties_add_int_slider(props, "progress_width",
				      "Progress width", 4, 80, 1);
	obs_properties_add_int_slider(props, "refresh_ms",
				      "Refresh interval (ms)", 250, 5000,
				      250);
	obs_properties_add_bool(props, "enable_wmp_window_fallback",
				"Use WMP Legacy window title fallback");
	obs_properties_add_bool(props, "hide_when_empty",
				"Hide when no media");

	return props;
}

void source_update(void *data, obs_data_t *settings)
{
	auto *context = static_cast<SourceContext *>(data);

	context->app_filter = obs_data_get_string(settings, "app_filter");
	context->format = obs_data_get_string(settings, "format");
	context->hide_when_empty =
		obs_data_get_bool(settings, "hide_when_empty");
	context->enable_wmp_window_fallback =
		!obs_data_has_user_value(settings,
					 "enable_wmp_window_fallback") ||
		obs_data_get_bool(settings, "enable_wmp_window_fallback");
	context->refresh_ms =
		static_cast<uint32_t>(obs_data_get_int(settings, "refresh_ms"));
	context->progress_width =
		static_cast<int>(obs_data_get_int(settings, "progress_width"));

	if (context->format.empty())
		context->format = kDefaultFormat;

	context->monitor.configure(context->app_filter, context->refresh_ms,
				   context->enable_wmp_window_fallback);
	context->last_text.clear();
}

void *source_create(obs_data_t *settings, obs_source_t *source)
{
	auto *context = new SourceContext();
	context->source = source;
	context->text_source = create_text_source();

	source_update(context, settings);
	context->monitor.start();

	return context;
}

void source_destroy(void *data)
{
	auto *context = static_cast<SourceContext *>(data);
	context->monitor.stop();

	if (context->text_source)
		obs_source_release(context->text_source);

	delete context;
}

void source_video_tick(void *data, float seconds)
{
	auto *context = static_cast<SourceContext *>(data);
	context->update_elapsed += seconds;

	if (context->update_elapsed < 0.20f)
		return;

	context->update_elapsed = 0.0f;

	const auto state = context->monitor.snapshot();
	const auto text = render_text(*context, state);
	if (text == context->last_text)
		return;

	update_text_source(context, text);
	context->last_text = text;
}

void source_video_render(void *data, gs_effect_t *)
{
	auto *context = static_cast<SourceContext *>(data);
	if (!context->text_source || context->last_text.empty())
		return;

	obs_source_video_render(context->text_source);
}

uint32_t source_get_width(void *data)
{
	const auto *context = static_cast<SourceContext *>(data);
	return context->text_source ? obs_source_get_width(context->text_source)
				    : 0;
}

uint32_t source_get_height(void *data)
{
	const auto *context = static_cast<SourceContext *>(data);
	return context->text_source ? obs_source_get_height(context->text_source)
				    : 0;
}

} // namespace

obs_source_info wmp_source_info = [] {
	obs_source_info info = {};
	info.id = "obs_wmp_smtc_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = source_get_name;
	info.create = source_create;
	info.destroy = source_destroy;
	info.update = source_update;
	info.get_defaults = source_get_defaults;
	info.get_properties = source_get_properties;
	info.video_render = source_video_render;
	info.video_tick = source_video_tick;
	info.get_width = source_get_width;
	info.get_height = source_get_height;
	return info;
}();

} // namespace obs_wmp
