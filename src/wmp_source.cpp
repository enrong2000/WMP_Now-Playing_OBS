#include "wmp_source.hpp"

#include "smtc_monitor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <obs-module.h>
#include <sstream>
#include <string>
#include <string_view>

#include <ShlObj.h>

namespace obs_wmp {
namespace {

constexpr const char *kDefaultFormat =
	"{artist} - {title}\n"
	"{album}\n"
	"{position} / {duration} {progress_bar} {status}";

std::string default_json_path()
{
	wchar_t *appdata = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr,
					       &appdata)) &&
	    appdata) {
		std::filesystem::path dir =
			std::filesystem::path(appdata) / L"obs-wmp-smtc";
		CoTaskMemFree(appdata);
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		return (dir / "now-playing.json").string();
	}
	return {};
}

struct SourceContext {
	obs_source_t *source = nullptr;
	obs_source_t *text_source = nullptr;
	SmtcMonitor monitor;
	std::string app_filter = "wmplayer";
	std::string format = kDefaultFormat;
	bool hide_when_empty = false;
	int display_mode = 0;
	bool show_composer = true;
	uint32_t refresh_ms = 1000;
	int progress_width = 24;
	float update_elapsed = 0.0f;
	std::string last_text;
	std::string json_output_path;
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

	if (state.end_ms > state.start_ms)
		position = std::clamp(position, state.start_ms, state.end_ms);

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

std::string format_playlist_text(const MediaState &state)
{
	if (state.playlist.empty())
		return {};

	std::ostringstream out;
	for (const auto &item : state.playlist) {
		if (item.index == state.current_playlist_index)
			out << "▶ ";
		else
			out << "   ";

		out << (item.index + 1) << ". ";
		out << (item.title.empty() ? "Unknown" : item.title);

		if (!item.artist.empty())
			out << " - " << item.artist;

		out << "\n";
	}

	return out.str();
}

std::string render_text(const SourceContext &context, const MediaState &state)
{
	if (!state.available) {
		if (context.hide_when_empty)
			return {};
		return "Waiting for Windows Media Player";
	}

	const auto duration =
		state.end_ms > state.start_ms ? state.end_ms - state.start_ms : 0;
	const auto absolute_position = current_position_ms(state);
	const auto relative_position =
		duration > 0 ? std::clamp(absolute_position - state.start_ms,
					  int64_t{0}, duration)
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

	if (context.display_mode == 1 && state.available) {
		std::ostringstream ui;
		ui << "♪  " << fallback(state.title, "Unknown title") << "\n";
		ui << "👤 " << fallback(state.artist, "Unknown artist") << "\n";
		ui << "💿 " << fallback(state.album, "Unknown album") << "\n";
		if (context.show_composer && !state.composer.empty())
			ui << "✍  " << state.composer << "\n";
		ui << progress_bar(ratio, context.progress_width) << "  "
		   << format_time(relative_position) << " / "
		   << (duration > 0 ? format_time(duration) : "--:--");

		if (!state.playlist.empty()) {
			ui << "\n\n📋 Playlist ("
			   << state.playlist.size() << " tracks):\n";
			ui << format_playlist_text(state);
		}

		return ui.str();
	}

	std::string output = context.format;
	replace_all(output, "{title}", fallback(state.title, "Unknown title"));
	replace_all(output, "{artist}", fallback(state.artist, "Unknown artist"));
	replace_all(output, "{album}", state.album);
	replace_all(output, "{album_artist}", state.album_artist);
	replace_all(output, "{composer}", state.composer);
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
	replace_all(output, "{diagnostic}", state.error_message);

	std::ostringstream playlist_count;
	playlist_count << state.playlist.size();
	replace_all(output, "{playlist_count}", playlist_count.str());
	replace_all(output, "{playlist}", format_playlist_text(state));

	return output;
}

std::string escape_json(const std::string &input)
{
	std::string out;
	out.reserve(input.size() + 16);
	for (const char ch : input) {
		switch (ch) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			out += ch;
			break;
		}
	}
	return out;
}

void write_json_file(const SourceContext &context, const MediaState &state)
{
	if (context.json_output_path.empty())
		return;

	const auto duration =
		state.end_ms > state.start_ms ? state.end_ms - state.start_ms : 0;
	const auto position = current_position_ms(state);
	const auto relative_position =
		duration > 0 ? std::clamp(position - state.start_ms, int64_t{0},
					  duration)
			     : std::max<int64_t>(position, 0);
	const auto ratio = duration > 0
				   ? static_cast<double>(relative_position) /
					     static_cast<double>(duration)
				   : 0.0;

	std::ostringstream json;
	json << "{\n";
	json << "  \"available\": " << (state.available ? "true" : "false")
	     << ",\n";
	json << "  \"title\": \"" << escape_json(state.title) << "\",\n";
	json << "  \"artist\": \"" << escape_json(state.artist) << "\",\n";
	json << "  \"album\": \"" << escape_json(state.album) << "\",\n";
	json << "  \"album_artist\": \"" << escape_json(state.album_artist)
	     << "\",\n";
	json << "  \"composer\": \"" << escape_json(state.composer)
	     << "\",\n";
	json << "  \"status\": \""
	     << playback_status_text(state.playback_status) << "\",\n";
	json << "  \"backend\": \"" << escape_json(state.backend)
	     << "\",\n";
	json << "  \"position_ms\": " << relative_position << ",\n";
	json << "  \"duration_ms\": " << duration << ",\n";
	json << "  \"progress\": " << ratio << ",\n";
	json << "  \"position_text\": \""
	     << (state.timeline_available ? format_time(relative_position)
					  : "--:--")
	     << "\",\n";
	json << "  \"duration_text\": \""
	     << (duration > 0 ? format_time(duration) : "--:--")
	     << "\",\n";

	/* Playlist array */
	json << "  \"current_playlist_index\": " << state.current_playlist_index
	     << ",\n";
	json << "  \"playlist\": [\n";
	for (size_t i = 0; i < state.playlist.size(); ++i) {
		const auto &item = state.playlist[i];
		json << "    {\n";
		json << "      \"index\": " << item.index << ",\n";
		json << "      \"title\": \"" << escape_json(item.title)
		     << "\",\n";
		json << "      \"artist\": \"" << escape_json(item.artist)
		     << "\",\n";
		json << "      \"album\": \"" << escape_json(item.album)
		     << "\",\n";
		json << "      \"duration_sec\": " << item.duration_sec
		     << "\n";
		json << "    }";
		if (i + 1 < state.playlist.size())
			json << ",";
		json << "\n";
	}
	json << "  ]\n";
	json << "}\n";

	/* Atomic write: write to a temp file, then rename */
	std::filesystem::path target(context.json_output_path);
	std::filesystem::path temp = target;
	temp += ".tmp";

	std::ofstream file(temp, std::ios::trunc | std::ios::binary);
	if (file) {
		file << json.str();
		file.close();
		std::error_code ec;
		std::filesystem::rename(temp, target, ec);
	}
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
	return "WMP Legacy Now Playing (COM)";
}

void source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "app_filter", "wmplayer");
	obs_data_set_default_string(settings, "format", kDefaultFormat);
	obs_data_set_default_bool(settings, "hide_when_empty", false);
	obs_data_set_default_int(settings, "refresh_ms", 1000);
	obs_data_set_default_int(settings, "progress_width", 24);
	obs_data_set_default_int(settings, "display_mode", 1);
	obs_data_set_default_bool(settings, "show_composer", true);
	obs_data_set_default_string(settings, "json_output_path",
				    default_json_path().c_str());
}

obs_properties_t *source_get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "app_filter", "App filter",
				OBS_TEXT_DEFAULT);

	obs_property_t *display_mode_list = obs_properties_add_list(
		props, "display_mode", "Display mode",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(display_mode_list, "Template Text", 0);
	obs_property_list_add_int(display_mode_list, "UI Card", 1);

	obs_properties_add_text(props, "format", "Format",
				OBS_TEXT_MULTILINE);
	obs_properties_add_int_slider(props, "progress_width",
				      "Progress width", 4, 80, 1);
	obs_properties_add_bool(props, "show_composer",
				"Show composer in UI Card mode");
	obs_properties_add_int_slider(props, "refresh_ms",
				      "Refresh interval (ms)", 250, 5000,
				      250);
	obs_properties_add_bool(props, "hide_when_empty",
				"Hide when no media");
	obs_properties_add_path(props, "json_output_path",
				"JSON output path (for overlay)",
				OBS_PATH_FILE_SAVE, "JSON (*.json)", nullptr);

	return props;
}

void source_update(void *data, obs_data_t *settings)
{
	auto *context = static_cast<SourceContext *>(data);

	context->app_filter = obs_data_get_string(settings, "app_filter");
	context->format = obs_data_get_string(settings, "format");
	context->hide_when_empty =
		obs_data_get_bool(settings, "hide_when_empty");
	context->refresh_ms =
		static_cast<uint32_t>(obs_data_get_int(settings, "refresh_ms"));
	context->progress_width =
		static_cast<int>(obs_data_get_int(settings, "progress_width"));
	context->display_mode = static_cast<int>(obs_data_get_int(settings, "display_mode"));
	context->show_composer = obs_data_get_bool(settings, "show_composer");
	context->json_output_path =
		obs_data_get_string(settings, "json_output_path");

	if (context->format.empty())
		context->format = kDefaultFormat;

	if (context->json_output_path.empty())
		context->json_output_path = default_json_path();

	context->monitor.configure(context->app_filter, context->refresh_ms);
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

	write_json_file(*context, state);

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
