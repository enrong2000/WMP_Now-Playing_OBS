#include "smtc_monitor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <exception>
#include <sstream>
#include <string_view>
#include <utility>

#include <Windows.h>
#include <ObjBase.h>
#include <comdef.h>

#import "wmp.dll" rename_namespace("WMPLib") named_guids

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Media.h>
#include <winrt/base.h>

namespace obs_wmp {
namespace {

using winrt::Windows::Media::Control::
	GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::
	GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::
	GlobalSystemMediaTransportControlsSessionPlaybackStatus;

std::string lowercase(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		       [](unsigned char ch) {
			       return static_cast<char>(std::tolower(ch));
		       });
	return value;
}

std::wstring lowercase(std::wstring value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		       [](wchar_t ch) {
			       return static_cast<wchar_t>(std::towlower(ch));
		       });
	return value;
}

std::string trim_copy(std::string value)
{
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return {};

	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

bool ends_with(std::string_view value, std::string_view suffix)
{
	return value.size() >= suffix.size() &&
	       value.compare(value.size() - suffix.size(), suffix.size(),
			     suffix) == 0;
}

std::string wide_to_utf8(const std::wstring &value)
{
	if (value.empty())
		return {};

	const int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
					       static_cast<int>(value.size()),
					       nullptr, 0, nullptr, nullptr);
	if (needed <= 0)
		return {};

	std::string converted(static_cast<size_t>(needed), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
			    static_cast<int>(value.size()), converted.data(),
			    needed, nullptr, nullptr);
	return converted;
}

std::string wide_to_utf8(const wchar_t *value)
{
	return value ? wide_to_utf8(std::wstring(value)) : std::string{};
}

std::string narrow_to_utf8(const char *value)
{
	if (!value || value[0] == '\0')
		return {};

	const int needed = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
	if (needed <= 0)
		return {};

	std::wstring converted(static_cast<size_t>(needed), L'\0');
	MultiByteToWideChar(CP_ACP, 0, value, -1, converted.data(), needed);
	if (!converted.empty() && converted.back() == L'\0')
		converted.pop_back();

	return wide_to_utf8(converted);
}

std::string tchar_to_utf8(const wchar_t *value)
{
	return wide_to_utf8(value);
}

std::string tchar_to_utf8(const char *value)
{
	return narrow_to_utf8(value);
}

std::wstring process_name_for_window(HWND window)
{
	DWORD process_id = 0;
	GetWindowThreadProcessId(window, &process_id);
	if (process_id == 0)
		return {};

	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
				     process_id);
	if (!process)
		return {};

	std::vector<wchar_t> path(32768);
	DWORD length = static_cast<DWORD>(path.size());
	std::wstring process_name;

	if (QueryFullProcessImageNameW(process, 0, path.data(), &length) &&
	    length > 0) {
		std::wstring full_path(path.data(), length);
		const auto split = full_path.find_last_of(L"\\/");
		process_name = split == std::wstring::npos
				       ? full_path
				       : full_path.substr(split + 1);
	}

	CloseHandle(process);
	return lowercase(process_name);
}

std::wstring window_title(HWND window)
{
	const int length = GetWindowTextLengthW(window);
	if (length <= 0)
		return {};

	std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
	const int copied = GetWindowTextW(window, buffer.data(), length + 1);
	if (copied <= 0)
		return {};

	return std::wstring(buffer.data(), static_cast<size_t>(copied));
}

struct WmpWindowList {
	std::vector<std::string> titles;
};

BOOL CALLBACK enum_wmp_windows(HWND window, LPARAM user_data)
{
	if (!IsWindowVisible(window))
		return TRUE;

	if (process_name_for_window(window) != L"wmplayer.exe")
		return TRUE;

	const auto title = trim_copy(wide_to_utf8(window_title(window)));
	if (!title.empty())
		reinterpret_cast<WmpWindowList *>(user_data)->titles.push_back(title);

	return TRUE;
}

std::vector<std::string> find_wmp_window_titles()
{
	WmpWindowList windows;
	EnumWindows(enum_wmp_windows, reinterpret_cast<LPARAM>(&windows));
	return windows.titles;
}

std::string media_title_from_wmp_window(std::string title)
{
	title = trim_copy(std::move(title));
	const std::string suffix = " - Windows Media Player";

	if (ends_with(title, suffix))
		title.resize(title.size() - suffix.size());

	title = trim_copy(std::move(title));
	if (title == "Windows Media Player")
		return {};

	return title;
}

bool wmp_fallback_requested(const std::string &filter)
{
	const auto lowered = lowercase(filter);
	return lowered.empty() || lowered.find("wmp") != std::string::npos ||
	       lowered.find("wmplayer") != std::string::npos ||
	       lowered.find("windows media") != std::string::npos;
}

int64_t to_ms(winrt::Windows::Foundation::TimeSpan value)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(value)
		.count();
}

PlaybackStatus to_playback_status(
	GlobalSystemMediaTransportControlsSessionPlaybackStatus status)
{
	switch (status) {
	case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed:
		return PlaybackStatus::closed;
	case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Opened:
		return PlaybackStatus::opened;
	case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Changing:
		return PlaybackStatus::changing;
	case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped:
		return PlaybackStatus::stopped;
	case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing:
		return PlaybackStatus::playing;
	case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused:
		return PlaybackStatus::paused;
	default:
		return PlaybackStatus::unknown;
	}
}

std::string join_hstrings(
	const winrt::Windows::Foundation::Collections::IVectorView<
		winrt::hstring> &values)
{
	std::ostringstream stream;
	bool first = true;

	for (const auto &value : values) {
		if (!first)
			stream << ", ";
		first = false;
		stream << winrt::to_string(value);
	}

	return stream.str();
}

GlobalSystemMediaTransportControlsSession pick_session(
	const GlobalSystemMediaTransportControlsSessionManager &manager,
	const std::string &filter, std::vector<std::string> &session_ids)
{
	const auto sessions = manager.GetSessions();
	const auto lowered_filter = lowercase(filter);

	GlobalSystemMediaTransportControlsSession fallback{nullptr};

	for (const auto &session : sessions) {
		const auto id = winrt::to_string(session.SourceAppUserModelId());
		session_ids.push_back(id);

		if (!fallback)
			fallback = session;

		if (!lowered_filter.empty() &&
		    lowercase(id).find(lowered_filter) != std::string::npos)
			return session;
	}

	if (lowered_filter.empty()) {
		if (const auto current = manager.GetCurrentSession())
			return current;
	}

	return lowered_filter.empty() ? fallback : nullptr;
}

MediaState unavailable_state(std::string message,
			     std::vector<std::string> session_ids = {})
{
	MediaState state;
	state.available = false;
	state.error_message = std::move(message);
	state.active_sessions = std::move(session_ids);
	state.captured_at = std::chrono::steady_clock::now();
	return state;
}

MediaState extract_wmp_com_state(WMPLib::IWMPPlayer4Ptr &player,
				const char *backend_label,
				std::vector<std::string> session_ids)
{
	MediaState state;

	WMPLib::IWMPMediaPtr media = player->currentMedia;
	if (!media)
		return unavailable_state("WMP COM currentMedia unavailable",
					 std::move(session_ids));

	const _bstr_t title = media->name;
	const _bstr_t artist = media->getItemInfo(_bstr_t(L"Author"));
	const _bstr_t composer = media->getItemInfo(_bstr_t(L"WM/Composer"));
	const _bstr_t album = media->getItemInfo(_bstr_t(L"WM/AlbumTitle"));
	const _bstr_t album_artist = media->getItemInfo(_bstr_t(L"WM/AlbumArtist"));
	double duration = media->duration;
	double position = player->controls->currentPosition;

	WMPLib::WMPPlayState playState = player->playState;
	PlaybackStatus status = PlaybackStatus::unknown;
	switch (playState) {
	case WMPLib::wmppsPlaying:
		status = PlaybackStatus::playing;
		break;
	case WMPLib::wmppsPaused:
		status = PlaybackStatus::paused;
		break;
	case WMPLib::wmppsStopped:
		status = PlaybackStatus::stopped;
		break;
	default:
		status = PlaybackStatus::opened;
		break;
	}

	state.available = true;
	state.limited_fallback = false;
	state.legacy_wmp_running = true;
	state.is_legacy_wmp_com = true;
	state.backend = backend_label;
	state.source_app_id = "wmplayer.exe";
	state.title = wide_to_utf8(static_cast<const wchar_t *>(title));
	state.artist = wide_to_utf8(static_cast<const wchar_t *>(artist));
	state.album = wide_to_utf8(static_cast<const wchar_t *>(album));
	state.album_artist = wide_to_utf8(static_cast<const wchar_t *>(album_artist));
	state.composer = wide_to_utf8(static_cast<const wchar_t *>(composer));
	state.start_ms = 0;
	state.end_ms = static_cast<int64_t>(duration * 1000.0);
	state.position_ms = static_cast<int64_t>(position * 1000.0);
	state.timeline_available = state.end_ms > 0;
	state.playback_status = status;
	state.active_sessions = std::move(session_ids);
	state.captured_at = std::chrono::steady_clock::now();
	return state;
}

MediaState capture_wmp_com_fallback(std::vector<std::string> session_ids)
{
	/* COM calls to WMP require an STA apartment.  The worker thread is
	   already in a WinRT MTA, so we spin up a temporary STA thread for
	   the COM work and join on it. */
	MediaState result;
	std::thread sta_thread([&result, ids = std::move(session_ids)]() mutable {
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

		try {
			/* ----- Strategy 1: GetActiveObject (running WMP) ----- */
			IUnknown *punk = nullptr;
			CLSID clsid;
			HRESULT hr = CLSIDFromProgID(L"WMPlayer.OCX", &clsid);
			if (SUCCEEDED(hr))
				hr = GetActiveObject(clsid, nullptr, &punk);

			if (SUCCEEDED(hr) && punk) {
				WMPLib::IWMPPlayer4Ptr player;
				hr = punk->QueryInterface(__uuidof(WMPLib::IWMPPlayer4),
							 reinterpret_cast<void **>(&player));
				punk->Release();

				if (SUCCEEDED(hr) && player) {
					result = extract_wmp_com_state(
						player, "WMP Legacy COM (ROT)",
						std::move(ids));
					CoUninitialize();
					return;
				}
			}

			/* ----- Strategy 2: CreateInstance (embedded WMP) ----- */
			WMPLib::IWMPPlayer4Ptr player;
			hr = player.CreateInstance(__uuidof(WMPLib::WindowsMediaPlayer));
			if (SUCCEEDED(hr) && player) {
				result = extract_wmp_com_state(
					player, "WMP Legacy COM (embed)",
					std::move(ids));
				CoUninitialize();
				return;
			}

			result = unavailable_state(
				"Failed to connect to Windows Media Player via COM",
				std::move(ids));
		} catch (const _com_error &err) {
			result = unavailable_state(tchar_to_utf8(err.ErrorMessage()),
						   std::move(ids));
		} catch (...) {
			result = unavailable_state(
				"Unknown error in WMP COM fallback",
				std::move(ids));
		}

		CoUninitialize();
	});

	sta_thread.join();
	return result;
}

MediaState capture_wmp_window_fallback(std::vector<std::string> session_ids)
{
	auto titles = find_wmp_window_titles();
	if (titles.empty())
		return unavailable_state("No matching SMTC media session",
					 std::move(session_ids));

	std::string media_title;
	for (const auto &title : titles) {
		media_title = media_title_from_wmp_window(title);
		if (!media_title.empty())
			break;
	}

	MediaState state;
	state.available = true;
	state.limited_fallback = true;
	state.legacy_wmp_running = true;
	state.backend = "WMP window title";
	state.source_app_id = "wmplayer.exe";
	state.title = media_title.empty() ? "Windows Media Player Legacy"
					  : media_title;
	state.playback_status = PlaybackStatus::opened;
	state.error_message =
		"WMP Legacy did not publish an SMTC session; using window title fallback";
	state.active_sessions = std::move(session_ids);
	state.legacy_wmp_windows = std::move(titles);
	state.captured_at = std::chrono::steady_clock::now();
	return state;
}

MediaState capture_state(
	const GlobalSystemMediaTransportControlsSessionManager &manager,
	const std::string &filter, bool enable_wmp_window_fallback)
{
	std::vector<std::string> session_ids;
	const auto session = pick_session(manager, filter, session_ids);

	if (!session) {
		if (enable_wmp_window_fallback && wmp_fallback_requested(filter)) {
			auto com_state = capture_wmp_com_fallback(session_ids);
			if (com_state.available)
				return com_state;
			return capture_wmp_window_fallback(std::move(session_ids));
		}

		return unavailable_state("No matching SMTC media session",
					 std::move(session_ids));
	}

	MediaState state;
	state.available = true;
	state.backend = "SMTC";
	state.source_app_id = winrt::to_string(session.SourceAppUserModelId());
	state.active_sessions = std::move(session_ids);
	state.captured_at = std::chrono::steady_clock::now();

	const auto props = session.TryGetMediaPropertiesAsync().get();
	state.title = winrt::to_string(props.Title());
	state.artist = winrt::to_string(props.Artist());
	state.album = winrt::to_string(props.AlbumTitle());
	state.album_artist = winrt::to_string(props.AlbumArtist());
	state.subtitle = winrt::to_string(props.Subtitle());

	const auto joined_genres = join_hstrings(props.Genres());
	if (!joined_genres.empty())
		state.genres.push_back(joined_genres);

	const auto timeline = session.GetTimelineProperties();
	state.start_ms = to_ms(timeline.StartTime());
	state.end_ms = to_ms(timeline.EndTime());
	state.min_seek_ms = to_ms(timeline.MinSeekTime());
	state.max_seek_ms = to_ms(timeline.MaxSeekTime());
	state.position_ms = to_ms(timeline.Position());
	state.timeline_available =
		state.end_ms > state.start_ms || state.position_ms > 0 ||
		state.max_seek_ms > state.min_seek_ms;

	const auto playback_info = session.GetPlaybackInfo();
	state.playback_status =
		to_playback_status(playback_info.PlaybackStatus());

	if (const auto controls = playback_info.Controls()) {
		state.can_play = controls.IsPlayEnabled();
		state.can_pause = controls.IsPauseEnabled();
		state.can_next = controls.IsNextEnabled();
		state.can_previous = controls.IsPreviousEnabled();
		state.can_seek = controls.IsPlaybackPositionEnabled();
	}

	return state;
}

} // namespace

SmtcMonitor::~SmtcMonitor()
{
	stop();
}

void SmtcMonitor::start()
{
	std::lock_guard lock(mutex_);
	if (started_)
		return;

	stop_requested_ = false;
	started_ = true;
	worker_ = std::thread(&SmtcMonitor::run, this);
}

void SmtcMonitor::stop()
{
	{
		std::lock_guard lock(mutex_);
		if (!started_)
			return;
		stop_requested_ = true;
	}

	wake_.notify_all();

	if (worker_.joinable())
		worker_.join();

	std::lock_guard lock(mutex_);
	started_ = false;
}

void SmtcMonitor::configure(std::string app_filter, uint32_t refresh_ms,
			    bool enable_wmp_window_fallback)
{
	{
		std::lock_guard lock(mutex_);
		app_filter_ = std::move(app_filter);
		refresh_ms_ = std::clamp<uint32_t>(refresh_ms, 250, 5000);
		enable_wmp_window_fallback_ = enable_wmp_window_fallback;
	}

	wake_.notify_all();
}

MediaState SmtcMonitor::snapshot() const
{
	std::lock_guard lock(mutex_);
	return state_;
}

void SmtcMonitor::run()
{
	winrt::init_apartment(winrt::apartment_type::multi_threaded);

	GlobalSystemMediaTransportControlsSessionManager manager{nullptr};

	for (;;) {
		std::string filter;
		uint32_t refresh_ms = 1000;
		bool enable_wmp_window_fallback = true;

		{
			std::lock_guard lock(mutex_);
			if (stop_requested_)
				break;
			filter = app_filter_;
			refresh_ms = refresh_ms_;
			enable_wmp_window_fallback = enable_wmp_window_fallback_;
		}

		MediaState next_state;

		try {
			if (!manager) {
				manager =
					GlobalSystemMediaTransportControlsSessionManager::
						RequestAsync()
							.get();
			}

			next_state = capture_state(manager, filter,
						   enable_wmp_window_fallback);
		} catch (const winrt::hresult_error &err) {
			manager = nullptr;
			next_state = unavailable_state(winrt::to_string(err.message()));
		} catch (const std::exception &err) {
			manager = nullptr;
			next_state = unavailable_state(err.what());
		} catch (...) {
			manager = nullptr;
			next_state = unavailable_state("Unknown SMTC error");
		}

		{
			std::lock_guard lock(mutex_);
			state_ = std::move(next_state);
		}

		std::unique_lock lock(mutex_);
		wake_.wait_for(lock, std::chrono::milliseconds(refresh_ms),
			       [this] { return stop_requested_; });
		if (stop_requested_)
			break;
	}

	winrt::uninit_apartment();
}

const char *playback_status_text(PlaybackStatus status)
{
	switch (status) {
	case PlaybackStatus::closed:
		return "Closed";
	case PlaybackStatus::opened:
		return "Opened";
	case PlaybackStatus::changing:
		return "Changing";
	case PlaybackStatus::stopped:
		return "Stopped";
	case PlaybackStatus::playing:
		return "Playing";
	case PlaybackStatus::paused:
		return "Paused";
	case PlaybackStatus::unknown:
	default:
		return "Unknown";
	}
}

} // namespace obs_wmp
