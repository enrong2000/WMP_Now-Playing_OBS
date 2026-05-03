#include "smtc_monitor.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <Windows.h>
#include <ObjBase.h>
#include <comdef.h>

#import "wmp.dll" rename_namespace("WMPLib") named_guids

namespace obs_wmp {
namespace {

/* ===================================================================
 *  String conversion utilities
 * =================================================================== */

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

std::string bstr_to_utf8(const _bstr_t &bs)
{
	const wchar_t *raw = static_cast<const wchar_t *>(bs);
	return raw ? wide_to_utf8(raw) : std::string{};
}

std::string tchar_to_utf8(const wchar_t *value)
{
	return wide_to_utf8(value);
}

std::string tchar_to_utf8(const char *value)
{
	if (!value || value[0] == '\0')
		return {};

	const int needed =
		MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
	if (needed <= 0)
		return {};

	std::wstring converted(static_cast<size_t>(needed), L'\0');
	MultiByteToWideChar(CP_ACP, 0, value, -1, converted.data(), needed);
	if (!converted.empty() && converted.back() == L'\0')
		converted.pop_back();

	return wide_to_utf8(converted);
}

/* ===================================================================
 *  Helper: unavailable state factory
 * =================================================================== */

MediaState unavailable_state(std::string message)
{
	MediaState state;
	state.available = false;
	state.error_message = std::move(message);
	state.captured_at = std::chrono::steady_clock::now();
	return state;
}

/* ===================================================================
 *  COM attach: find a running WMP instance via ROT / GetActiveObject
 * =================================================================== */

WMPLib::IWMPPlayer4Ptr attach_running_wmp()
{
	CLSID clsid;
	HRESULT hr = CLSIDFromProgID(L"WMPlayer.OCX", &clsid);
	if (FAILED(hr))
		return nullptr;

	IUnknown *punk = nullptr;
	hr = GetActiveObject(clsid, nullptr, &punk);
	if (FAILED(hr) || !punk)
		return nullptr;

	WMPLib::IWMPPlayer4Ptr player;
	hr = punk->QueryInterface(__uuidof(WMPLib::IWMPPlayer4),
				  reinterpret_cast<void **>(&player));
	punk->Release();

	if (FAILED(hr))
		return nullptr;

	return player;
}

/* ===================================================================
 *  Extract playlist from the running WMP instance
 * =================================================================== */

std::vector<PlaylistItem>
extract_playlist(WMPLib::IWMPPlayer4Ptr &player, int &current_index,
		 const _bstr_t &current_source_url)
{
	std::vector<PlaylistItem> items;
	current_index = -1;

	WMPLib::IWMPPlaylistPtr playlist;
	try {
		playlist = player->currentPlaylist;
	} catch (const _com_error &) {
		return items;
	}

	if (!playlist)
		return items;

	long count = 0;
	try {
		count = playlist->count;
	} catch (const _com_error &) {
		return items;
	}

	if (count <= 0)
		return items;

	items.reserve(static_cast<size_t>(count));

	for (long i = 0; i < count; ++i) {
		WMPLib::IWMPMediaPtr media;
		try {
			HRESULT hr = playlist->get_Item(i, &media);
			if (FAILED(hr))
				_com_issue_error(hr);
		} catch (const _com_error &) {
			continue;
		}
		if (!media)
			continue;

		PlaylistItem item;
		item.index = static_cast<int>(i);

		try {
			item.title = bstr_to_utf8(media->name);
		} catch (const _com_error &) {
		}

		try {
			item.artist =
				bstr_to_utf8(media->getItemInfo(_bstr_t(L"Author")));
		} catch (const _com_error &) {
		}

		try {
			item.album = bstr_to_utf8(
				media->getItemInfo(_bstr_t(L"WM/AlbumTitle")));
		} catch (const _com_error &) {
		}

		try {
			item.duration_sec = media->duration;
		} catch (const _com_error &) {
		}

		/* Determine current index by comparing source URLs */
		if (current_index < 0) {
			try {
				_bstr_t item_url = media->sourceURL;
				if (item_url.length() > 0 &&
				    current_source_url.length() > 0 &&
				    _wcsicmp(static_cast<const wchar_t *>(
						     item_url),
					     static_cast<const wchar_t *>(
						     current_source_url)) ==
					    0) {
					current_index = static_cast<int>(i);
				}
			} catch (const _com_error &) {
			}
		}

		items.push_back(std::move(item));
	}

	return items;
}

/* ===================================================================
 *  Extract full WMP state (current track + playlist)
 * =================================================================== */

MediaState capture_wmp_com_state()
{
	auto player = attach_running_wmp();
	if (!player)
		return unavailable_state(
			"Windows Media Player is not running or not accessible via ROT");

	/* Check play state — only extract details for Playing/Paused */
	WMPLib::WMPPlayState play_state;
	try {
		play_state = player->playState;
	} catch (const _com_error &err) {
		return unavailable_state(tchar_to_utf8(err.ErrorMessage()));
	}

	PlaybackStatus status = PlaybackStatus::unknown;
	switch (play_state) {
	case WMPLib::wmppsPlaying:
		status = PlaybackStatus::playing;
		break;
	case WMPLib::wmppsPaused:
		status = PlaybackStatus::paused;
		break;
	case WMPLib::wmppsStopped:
		status = PlaybackStatus::stopped;
		break;
	case WMPLib::wmppsTransitioning:
		status = PlaybackStatus::changing;
		break;
	default:
		status = PlaybackStatus::opened;
		break;
	}

	/* If WMP is not in a playable state, return minimal info */
	if (status != PlaybackStatus::playing &&
	    status != PlaybackStatus::paused) {
		MediaState state;
		state.available = true;
		state.legacy_wmp_running = true;
		state.backend = "WMP Legacy COM";
		state.source_app_id = "wmplayer.exe";
		state.playback_status = status;
		state.captured_at = std::chrono::steady_clock::now();
		return state;
	}

	/* Extract current media details */
	WMPLib::IWMPMediaPtr media;
	try {
		media = player->currentMedia;
	} catch (const _com_error &err) {
		return unavailable_state(tchar_to_utf8(err.ErrorMessage()));
	}

	if (!media)
		return unavailable_state("WMP COM: currentMedia is null");

	MediaState state;
	state.available = true;
	state.legacy_wmp_running = true;
	state.backend = "WMP Legacy COM";
	state.source_app_id = "wmplayer.exe";
	state.playback_status = status;

	try {
		state.title = bstr_to_utf8(media->name);
	} catch (const _com_error &) {
	}

	try {
		state.artist =
			bstr_to_utf8(media->getItemInfo(_bstr_t(L"Author")));
	} catch (const _com_error &) {
	}

	try {
		state.album = bstr_to_utf8(
			media->getItemInfo(_bstr_t(L"WM/AlbumTitle")));
	} catch (const _com_error &) {
	}

	try {
		state.album_artist = bstr_to_utf8(
			media->getItemInfo(_bstr_t(L"WM/AlbumArtist")));
	} catch (const _com_error &) {
	}

	try {
		state.composer = bstr_to_utf8(
			media->getItemInfo(_bstr_t(L"WM/Composer")));
	} catch (const _com_error &) {
	}

	try {
		double duration = media->duration;
		state.end_ms = static_cast<int64_t>(duration * 1000.0);
	} catch (const _com_error &) {
	}

	try {
		double position = player->controls->currentPosition;
		state.position_ms = static_cast<int64_t>(position * 1000.0);
	} catch (const _com_error &) {
	}

	state.start_ms = 0;
	state.timeline_available = state.end_ms > 0;
	state.captured_at = std::chrono::steady_clock::now();

	/* Extract playlist */
	_bstr_t current_source_url;
	try {
		current_source_url = media->sourceURL;
	} catch (const _com_error &) {
	}

	try {
		state.playlist = extract_playlist(
			player, state.current_playlist_index,
			current_source_url);
	} catch (const _com_error &) {
		/* Playlist extraction failed; state remains valid without it */
	}

	return state;
}

} // namespace

/* ===================================================================
 *  SmtcMonitor public interface
 * =================================================================== */

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

void SmtcMonitor::configure(std::string app_filter, uint32_t refresh_ms)
{
	{
		std::lock_guard lock(mutex_);
		app_filter_ = std::move(app_filter);
		refresh_ms_ = std::clamp<uint32_t>(refresh_ms, 250, 5000);
	}

	wake_.notify_all();
}

MediaState SmtcMonitor::snapshot() const
{
	std::lock_guard lock(mutex_);
	return state_;
}

/* ===================================================================
 *  Worker thread: dedicated STA apartment for COM calls
 * =================================================================== */

void SmtcMonitor::run()
{
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	for (;;) {
		uint32_t refresh_ms = 1000;

		{
			std::lock_guard lock(mutex_);
			if (stop_requested_)
				break;
			refresh_ms = refresh_ms_;
		}

		MediaState next_state;

		try {
			next_state = capture_wmp_com_state();
		} catch (const _com_error &err) {
			next_state =
				unavailable_state(tchar_to_utf8(err.ErrorMessage()));
		} catch (const std::exception &err) {
			next_state = unavailable_state(err.what());
		} catch (...) {
			next_state =
				unavailable_state("Unknown error in WMP COM polling");
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

	CoUninitialize();
}

/* ===================================================================
 *  Playback status text helper
 * =================================================================== */

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
