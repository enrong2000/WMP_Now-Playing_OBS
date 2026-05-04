#include "wmp_source.hpp"

#include "wmp_monitor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
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

constexpr int kDisplayModeTemplateText = 0;
constexpr int kDisplayModeUiCardText = 1;
constexpr int kDisplayModeEmbeddedOverlay = 2;
constexpr int kOverlayModeMinimized = 0;
constexpr int kOverlayModeMaximized = 1;

std::string default_json_path()
{
	wchar_t *appdata = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr,
					       &appdata)) &&
	    appdata) {
		std::filesystem::path dir =
			std::filesystem::path(appdata) / L"obs-wmp-legacy";
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
	obs_source_t *browser_source = nullptr;
	obs_source_t *active_child = nullptr;
	WmpMonitor monitor;
	std::string app_filter = "wmplayer";
	std::string format = kDefaultFormat;
	bool hide_when_empty = false;
	int display_mode = kDisplayModeEmbeddedOverlay;
	int overlay_work_mode = kOverlayModeMinimized;
	bool show_composer = true;
	bool show_full_playlist = true;
	uint32_t refresh_ms = 1000;
	int progress_width = 24;
	int overlay_width = 1120;
	int overlay_height = 460;
	int upcoming_tracks = 3;
	int compact_playlist_reveal_ms = 3000;
	float update_elapsed = 0.0f;
	std::string last_text;
	std::string json_output_path;
	std::string browser_url;
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

std::string url_encode(std::string_view value, bool path_component)
{
	std::ostringstream out;
	out << std::uppercase << std::hex;

	for (const unsigned char ch : value) {
		const bool unreserved = std::isalnum(ch) || ch == '-' ||
					ch == '_' || ch == '.' || ch == '~';
		const bool path_safe = path_component &&
				       (ch == '/' || ch == ':');

		if (unreserved || path_safe) {
			out << static_cast<char>(ch);
		} else {
			out << '%' << std::setw(2) << std::setfill('0')
			    << static_cast<int>(ch);
		}
	}

	return out.str();
}

std::string file_url_from_path(std::string path)
{
	if (path.empty())
		return {};

	for (auto &ch : path) {
		if (ch == '\\')
			ch = '/';
	}

	while (!path.empty() && path.front() == '/')
		path.erase(path.begin());

	return "file:///" + url_encode(path, true);
}

std::string overlay_mode_query_value(int mode)
{
	return mode == kOverlayModeMaximized ? "maximized" : "minimized";
}

std::string build_overlay_url(const std::string &local_html,
			      const SourceContext &context)
{
	std::string url = file_url_from_path(local_html);
	if (url.empty())
		return {};

	url += "?json=" + url_encode(context.json_output_path, false);
	url += "&mode=" + std::string(overlay_mode_query_value(
				  context.overlay_work_mode));
	url += "&upcoming=" + std::to_string(context.upcoming_tracks);
	url += "&reveal_ms=" +
	       std::to_string(context.compact_playlist_reveal_ms);
	return url;
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

/**
 * Select the appropriate playlist to display based on user preference.
 *
 * When show_full_playlist is true and the full playlist is available
 * (and larger than the current queue), use the full playlist.
 * Otherwise, use the current playback queue.
 */
const std::vector<PlaylistItem> &
effective_playlist(const SourceContext &context, const MediaState &state)
{
	if (context.show_full_playlist && !state.full_playlist.empty() &&
	    state.full_playlist.size() > state.playlist.size())
		return state.full_playlist;
	return state.playlist;
}

int effective_playlist_index(const SourceContext &context,
			     const MediaState &state)
{
	if (context.show_full_playlist && !state.full_playlist.empty() &&
	    state.full_playlist.size() > state.playlist.size())
		return state.full_playlist_index;
	return state.current_playlist_index;
}

std::string format_playlist_text(const SourceContext &context,
				 const MediaState &state)
{
	const auto &pl = effective_playlist(context, state);
	const int idx = effective_playlist_index(context, state);

	if (pl.empty())
		return {};

	std::ostringstream out;
	for (const auto &item : pl) {
		out << (item.index == idx ? "> " : "  ");
		out << (item.index + 1) << ". ";
		out << (item.title.empty() ? "Unknown" : item.title);

		if (!item.artist.empty())
			out << " - " << item.artist;

		out << "\n";
	}

	return out.str();
}

std::string render_ui_card_text(const SourceContext &context,
				const MediaState &state, int64_t position,
				int64_t duration, double ratio,
				const std::string &percent)
{
	const auto &pl = effective_playlist(context, state);
	const int idx = effective_playlist_index(context, state);

	std::ostringstream ui;
	ui << "NOW PLAYING  " << playback_status_text(state.playback_status)
	   << "\n";
	ui << fallback(state.title, "Unknown title") << "\n";
	ui << fallback(state.artist, "Unknown artist");

	if (!state.album.empty())
		ui << "  |  " << state.album;
	ui << "\n";

	if (context.show_composer && !state.composer.empty())
		ui << "Composer: " << state.composer << "\n";

	ui << progress_bar(ratio, context.progress_width) << "  " << percent
	   << "\n";
	ui << format_time(position) << " / "
	   << (duration > 0 ? format_time(duration) : "--:--");

	if (!pl.empty()) {
		ui << "\n\nQUEUE (" << pl.size() << " tracks)\n";
		ui << format_playlist_text(context, state);
	}

	return ui.str();
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

	if (context.display_mode == kDisplayModeUiCardText && state.available)
		return render_ui_card_text(context, state, relative_position,
					   duration, ratio, percent.str());

	const auto &pl = effective_playlist(context, state);

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
	playlist_count << pl.size();
	replace_all(output, "{playlist_count}", playlist_count.str());
	replace_all(output, "{playlist}",
		    format_playlist_text(context, state));

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

	const auto &pl = effective_playlist(context, state);
	const int pl_index = effective_playlist_index(context, state);

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
	json << "  \"source_app_id\": \"" << escape_json(state.source_app_id)
	     << "\",\n";
	json << "  \"legacy_wmp_running\": "
	     << (state.legacy_wmp_running ? "true" : "false") << ",\n";
	json << "  \"diagnostic\": \"" << escape_json(state.error_message)
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
	json << "  \"current_playlist_index\": " << pl_index
	     << ",\n";
	json << "  \"playlist\": [\n";
	for (size_t i = 0; i < pl.size(); ++i) {
		const auto &item = pl[i];
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
		if (i + 1 < pl.size())
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
	obs_data_t *font = obs_data_create();

	obs_data_set_string(settings, "text", "");
	obs_data_set_string(font, "face", "Segoe UI Semibold");
	obs_data_set_int(font, "size", 30);
	obs_data_set_int(font, "flags", OBS_FONT_BOLD);
	obs_data_set_obj(settings, "font", font);
	obs_data_set_int(settings, "color", 0xF6FAFF);
	obs_data_set_bool(settings, "gradient", true);
	obs_data_set_int(settings, "gradient_color", 0x7CE0C3);
	obs_data_set_int(settings, "gradient_opacity", 100);
	obs_data_set_double(settings, "gradient_dir", 90.0);
	obs_data_set_int(settings, "bk_color", 0x101418);
	obs_data_set_int(settings, "bk_opacity", 72);
	obs_data_set_bool(settings, "outline", true);
	obs_data_set_int(settings, "outline_size", 2);
	obs_data_set_int(settings, "outline_color", 0x0B0F12);
	obs_data_set_int(settings, "outline_opacity", 80);
	obs_data_release(font);

	obs_source_t *source = obs_source_create_private(
		"text_gdiplus_v2", "obs-wmp-legacy internal text", settings);
	if (!source) {
		source = obs_source_create_private(
			"text_gdiplus", "obs-wmp-legacy internal text", settings);
	}

	if (!source) {
		blog(LOG_WARNING,
		     "[obs-wmp-legacy] OBS Windows text source is unavailable");
	}

	obs_data_release(settings);
	return source;
}

/**
 * Locate the bundled overlay HTML file
 * (data/obs-plugins/<plugin>/overlay/index.html).
 */
std::string find_overlay_html_path()
{
	char *path = obs_module_file("overlay/index.html");
	if (!path)
		return {};
	std::string result(path);
	bfree(path);
	return result;
}

/**
 * Create a private OBS Browser Source that loads the bundled
 * Now-Playing overlay HTML.  This lets the user add a single
 * OBS source that displays the rich glassmorphism overlay
 * directly - no second Browser Source required.
 *
 * The overlay URL carries the JSON path and UI mode as query
 * parameters so it does not depend on a writable config.json
 * next to the installed HTML.
 */
obs_source_t *create_browser_source(int width, int height,
				    const std::string &overlay_url)
{
	if (overlay_url.empty()) {
		blog(LOG_WARNING,
		     "[obs-wmp-legacy] overlay/index.html not found in plugin "
		     "data directory; embedded overlay unavailable");
		return nullptr;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", false);
	obs_data_set_string(settings, "url", overlay_url.c_str());
	obs_data_set_int(settings, "width", width);
	obs_data_set_int(settings, "height", height);
	obs_data_set_int(settings, "fps", 30);
	obs_data_set_bool(settings, "shutdown", false);
	obs_data_set_bool(settings, "restart_when_active", false);
	obs_data_set_bool(settings, "reroute_audio", false);
	obs_data_set_int(settings, "webpage_control_level", 1);

	obs_source_t *source = obs_source_create_private(
		"browser_source", "obs-wmp-legacy embedded overlay",
		settings);

	if (!source) {
		blog(LOG_WARNING,
		     "[obs-wmp-legacy] OBS Browser Source is unavailable; "
		     "ensure the obs-browser plugin is installed");
	}

	obs_data_release(settings);
	return source;
}

/**
 * Update the embedded browser source's URL and dimensions in place.
 */
void update_browser_source_settings(obs_source_t *browser, int width, int height,
				    const std::string &overlay_url)
{
	if (!browser)
		return;
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", false);
	if (!overlay_url.empty())
		obs_data_set_string(settings, "url", overlay_url.c_str());
	obs_data_set_int(settings, "width", width);
	obs_data_set_int(settings, "height", height);
	obs_source_update(browser, settings);
	obs_data_release(settings);
}

/**
 * Tell the embedded browser to refresh-no-cache (used after
 * the JSON output path / overlay config is rewritten so the
 * page picks up the new sidecar config without manual reload).
 */
void browser_refresh_no_cache(obs_source_t *browser)
{
	if (!browser)
		return;
	proc_handler_t *ph = obs_source_get_proc_handler(browser);
	if (!ph)
		return;
	calldata_t cd = {};
	calldata_init(&cd);
	proc_handler_call(ph, "refreshnocache", &cd);
	calldata_free(&cd);
}

void set_active_child(SourceContext *context, obs_source_t *child)
{
	if (!context || context->active_child == child)
		return;

	if (context->source && context->active_child)
		obs_source_remove_active_child(context->source,
					       context->active_child);
	context->active_child = nullptr;

	if (!context->source || !child)
		return;

	if (obs_source_add_active_child(context->source, child)) {
		context->active_child = child;
	} else {
		blog(LOG_WARNING,
		     "[obs-wmp-legacy] failed to activate child source");
	}
}

void refresh_browser_source(SourceContext *context, bool force)
{
	if (!context || !context->browser_source)
		return;

	const std::string overlay_url =
		build_overlay_url(find_overlay_html_path(), *context);
	const bool url_changed = overlay_url != context->browser_url;
	if (!force && !url_changed)
		return;

	update_browser_source_settings(context->browser_source,
				       context->overlay_width,
				       context->overlay_height, overlay_url);
	context->browser_url = overlay_url;
	browser_refresh_no_cache(context->browser_source);
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
	return "Windows Media Player (Legacy) Now Playing";
}

/* Forward declaration - defined below; called by source_update. */
void ensure_active_source(SourceContext *context);
void write_overlay_config(const SourceContext &context);
void log_overlay_url(const SourceContext &context);

void source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "app_filter", "wmplayer");
	obs_data_set_default_string(settings, "format", kDefaultFormat);
	obs_data_set_default_bool(settings, "hide_when_empty", false);
	obs_data_set_default_int(settings, "refresh_ms", 500);
	obs_data_set_default_int(settings, "progress_width", 24);
	/* Default to the new single-source Embedded Overlay mode */
	obs_data_set_default_int(settings, "display_mode",
				 kDisplayModeEmbeddedOverlay);
	obs_data_set_default_int(settings, "overlay_work_mode",
				 kOverlayModeMinimized);
	obs_data_set_default_int(settings, "overlay_width", 1120);
	obs_data_set_default_int(settings, "overlay_height", 460);
	obs_data_set_default_int(settings, "upcoming_tracks", 3);
	obs_data_set_default_int(settings, "compact_playlist_reveal_ms", 3000);
	obs_data_set_default_bool(settings, "show_composer", true);
	obs_data_set_default_bool(settings, "show_full_playlist", true);
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
	obs_property_list_add_int(display_mode_list, "Embedded Overlay",
				  kDisplayModeEmbeddedOverlay);
	obs_property_list_add_int(display_mode_list, "Template Text",
				  kDisplayModeTemplateText);
	obs_property_list_add_int(display_mode_list, "UI Card (Text)",
				  kDisplayModeUiCardText);

	obs_property_t *overlay_mode_list = obs_properties_add_list(
		props, "overlay_work_mode", "Overlay work mode",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(overlay_mode_list, "Minimized Window",
				  kOverlayModeMinimized);
	obs_property_list_add_int(overlay_mode_list, "Maximized Window",
				  kOverlayModeMaximized);

	obs_properties_add_int(props, "overlay_width",
			       "Embedded overlay width (px)", 200, 4096, 10);
	obs_properties_add_int(props, "overlay_height",
			       "Embedded overlay height (px)", 100, 4096, 10);
	obs_properties_add_int_slider(props, "upcoming_tracks",
				      "Upcoming tracks", 0, 10, 1);
	obs_properties_add_int_slider(props, "compact_playlist_reveal_ms",
				      "Compact playlist reveal (ms)", 0,
				      10000, 250);

	obs_properties_add_text(props, "format", "Format",
				OBS_TEXT_MULTILINE);
	obs_properties_add_int_slider(props, "progress_width",
				      "Progress width", 4, 80, 1);
	obs_properties_add_bool(props, "show_composer",
				"Show composer in UI Card mode");
	obs_properties_add_bool(props, "show_full_playlist",
				"Show full playlist (all tracks)");
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

	const std::string old_json_path = context->json_output_path;
	const int old_display_mode = context->display_mode;
	const int old_overlay_work_mode = context->overlay_work_mode;
	const int old_upcoming_tracks = context->upcoming_tracks;
	const int old_compact_reveal_ms =
		context->compact_playlist_reveal_ms;

	context->app_filter = obs_data_get_string(settings, "app_filter");
	context->format = obs_data_get_string(settings, "format");
	context->hide_when_empty =
		obs_data_get_bool(settings, "hide_when_empty");
	context->refresh_ms =
		static_cast<uint32_t>(obs_data_get_int(settings, "refresh_ms"));
	context->progress_width =
		static_cast<int>(obs_data_get_int(settings, "progress_width"));
	const int new_display_mode =
		static_cast<int>(obs_data_get_int(settings, "display_mode"));
	const int new_overlay_work_mode =
		static_cast<int>(obs_data_get_int(settings,
						  "overlay_work_mode"));
	const int new_w = std::max<int>(
		1, static_cast<int>(obs_data_get_int(settings, "overlay_width")));
	const int new_h = std::max<int>(
		1,
		static_cast<int>(obs_data_get_int(settings, "overlay_height")));
	context->upcoming_tracks = std::clamp<int>(
		static_cast<int>(obs_data_get_int(settings, "upcoming_tracks")),
		0, 10);
	context->compact_playlist_reveal_ms = std::clamp<int>(
		static_cast<int>(obs_data_get_int(
			settings, "compact_playlist_reveal_ms")),
		0, 10000);
	context->show_composer = obs_data_get_bool(settings, "show_composer");
	context->show_full_playlist =
		obs_data_get_bool(settings, "show_full_playlist");
	context->json_output_path =
		obs_data_get_string(settings, "json_output_path");

	if (context->format.empty())
		context->format = kDefaultFormat;

	if (context->json_output_path.empty())
		context->json_output_path = default_json_path();

	const bool size_changed = (new_w != context->overlay_width) ||
				  (new_h != context->overlay_height);
	context->overlay_width = new_w;
	context->overlay_height = new_h;

	context->display_mode = new_display_mode;
	context->overlay_work_mode =
		new_overlay_work_mode == kOverlayModeMaximized
			? kOverlayModeMaximized
			: kOverlayModeMinimized;

	const bool mode_changed = new_display_mode != old_display_mode;
	const bool overlay_settings_changed =
		context->json_output_path != old_json_path ||
		context->overlay_work_mode != old_overlay_work_mode ||
		context->upcoming_tracks != old_upcoming_tracks ||
		context->compact_playlist_reveal_ms != old_compact_reveal_ms;

	write_overlay_config(*context);
	ensure_active_source(context);

	/* Live propagation of size changes to embedded browser. */
	if (context->display_mode == kDisplayModeEmbeddedOverlay &&
	    context->browser_source &&
	    (size_changed || mode_changed || overlay_settings_changed))
		refresh_browser_source(context, true);

	context->monitor.configure(context->app_filter, context->refresh_ms);
	context->last_text.clear();
}

void write_overlay_config(const SourceContext &context)
{
	if (context.json_output_path.empty())
		return;

	std::ostringstream json;
	json << "{\n";
	json << "  \"jsonPath\": \"" << escape_json(context.json_output_path)
	     << "\",\n";
	json << "  \"mode\": \""
	     << overlay_mode_query_value(context.overlay_work_mode)
	     << "\",\n";
	json << "  \"upcomingTracks\": " << context.upcoming_tracks << ",\n";
	json << "  \"compactRevealMs\": "
	     << context.compact_playlist_reveal_ms << "\n";
	json << "}\n";
	const std::string json_content = json.str();

	/* 1. Write to the OBS plugin config directory (always writable) */
	char *config_dir = obs_module_config_path("");
	if (config_dir) {
		std::filesystem::path dir(config_dir);
		bfree(config_dir);
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		std::ofstream cfg(dir / "overlay-config.json",
				  std::ios::trunc | std::ios::binary);
		if (cfg)
			cfg << json_content;
	}

	/* 2. Write to the overlay data directory (may fail if read-only) */
	char *data_path = obs_module_file("overlay/config.json");
	if (data_path) {
		std::ofstream cfg(data_path,
				  std::ios::trunc | std::ios::binary);
		if (cfg)
			cfg << json_content;
		bfree(data_path);
	}
}

/**
 * Log the complete overlay URL with ?json= query parameter
 * for easy configuration of the OBS browser source.
 */
void log_overlay_url(const SourceContext &context)
{
	const std::string overlay_url =
		build_overlay_url(find_overlay_html_path(), context);
	if (overlay_url.empty())
		return;

	blog(LOG_INFO,
	     "[obs-wmp-legacy] ========================================");
	blog(LOG_INFO,
	     "[obs-wmp-legacy] For the 'Now Playing' browser source, use:");
	blog(LOG_INFO, "[obs-wmp-legacy]   %s", overlay_url.c_str());
	blog(LOG_INFO,
	     "[obs-wmp-legacy] JSON output path: %s",
	     context.json_output_path.c_str());
	blog(LOG_INFO,
	     "[obs-wmp-legacy] ========================================");
}

/**
 * Make sure the right child source for the current display mode
 * exists.  For Embedded Overlay mode (default), this creates a
 * private browser_source pointing at the bundled overlay HTML.
 * For text modes it creates the GDI+ text source.
 */
void ensure_active_source(SourceContext *context)
{
	if (context->display_mode == kDisplayModeEmbeddedOverlay) {
		/* Embedded Overlay (browser source) */
		if (!context->browser_source) {
			context->browser_url = build_overlay_url(
				find_overlay_html_path(), *context);
			context->browser_source = create_browser_source(
				context->overlay_width,
				context->overlay_height,
				context->browser_url);
		}
		set_active_child(context, context->browser_source);
	} else {
		/* Template Text / UI Card (text source) */
		if (!context->text_source)
			context->text_source = create_text_source();
		set_active_child(context, context->text_source);
	}
}

void *source_create(obs_data_t *settings, obs_source_t *source)
{
	auto *context = new SourceContext();
	context->source = source;

	source_update(context, settings);
	log_overlay_url(*context);
	context->monitor.start();

	return context;
}

void source_destroy(void *data)
{
	auto *context = static_cast<SourceContext *>(data);
	context->monitor.stop();
	set_active_child(context, nullptr);

	if (context->text_source)
		obs_source_release(context->text_source);
	if (context->browser_source)
		obs_source_release(context->browser_source);

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

	/* Always write JSON - the embedded overlay (and any external
	   consumer) relies on it. */
	write_json_file(*context, state);

	/* For text-based modes, render and push to the private text
	   source.  In Embedded Overlay mode the browser source picks
	   up changes by polling the JSON file directly, so there is
	   no separate text-render step. */
	if (context->display_mode == kDisplayModeEmbeddedOverlay)
		return;

	const auto text = render_text(*context, state);
	if (text == context->last_text)
		return;

	update_text_source(context, text);
	context->last_text = text;
}

void source_video_render(void *data, gs_effect_t *)
{
	auto *context = static_cast<SourceContext *>(data);

	if (context->display_mode == kDisplayModeEmbeddedOverlay) {
		if (context->browser_source)
			obs_source_video_render(context->browser_source);
		return;
	}

	if (!context->text_source || context->last_text.empty())
		return;

	obs_source_video_render(context->text_source);
}

uint32_t source_get_width(void *data)
{
	const auto *context = static_cast<SourceContext *>(data);

	if (context->display_mode == kDisplayModeEmbeddedOverlay) {
		if (context->browser_source) {
			const uint32_t width =
				obs_source_get_width(context->browser_source);
			if (width > 0)
				return width;
		}
		return static_cast<uint32_t>(std::max(1, context->overlay_width));
	}

	return context->text_source ? obs_source_get_width(context->text_source)
				    : 0;
}

uint32_t source_get_height(void *data)
{
	const auto *context = static_cast<SourceContext *>(data);

	if (context->display_mode == kDisplayModeEmbeddedOverlay) {
		if (context->browser_source) {
			const uint32_t height =
				obs_source_get_height(context->browser_source);
			if (height > 0)
				return height;
		}
		return static_cast<uint32_t>(std::max(1, context->overlay_height));
	}

	return context->text_source ? obs_source_get_height(context->text_source)
				    : 0;
}

void source_enum_active_sources(void *data,
				obs_source_enum_proc_t enum_callback,
				void *param)
{
	const auto *context = static_cast<SourceContext *>(data);
	if (!context || !context->source || !enum_callback ||
	    !context->active_child)
		return;

	enum_callback(context->source, context->active_child, param);
}

} // namespace

obs_source_info wmp_source_info = [] {
	obs_source_info info = {};
	info.id = "obs_wmp_legacy_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
			    OBS_SOURCE_COMPOSITE;
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
	info.enum_active_sources = source_enum_active_sources;
	return info;
}();

} // namespace obs_wmp
