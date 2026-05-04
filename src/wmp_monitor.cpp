#include "wmp_monitor.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <Windows.h>
#include <ObjBase.h>
#include <OleCtl.h>
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
 *  IWMPRemoteMediaServices host site
 *
 *  Windows Media Player (Legacy) does NOT register itself in the
 *  Running Object Table, so GetActiveObject always fails.
 *  The correct approach is to CoCreateInstance an in-process WMP
 *  OCX and set a client site implementing IWMPRemoteMediaServices
 *  with service type "Remote". This causes the OCX to attach to
 *  the already-running WMP process and share its playback state.
 * =================================================================== */

class RemoteHostSite final : public IOleClientSite,
			     public IServiceProvider,
			     public WMPLib::IWMPRemoteMediaServices {
public:
	/* IUnknown */
	ULONG __stdcall AddRef() override
	{
		return InterlockedIncrement(&refs_);
	}

	ULONG __stdcall Release() override
	{
		const ULONG refs = InterlockedDecrement(&refs_);
		if (refs == 0)
			delete this;
		return refs;
	}

	HRESULT __stdcall QueryInterface(REFIID riid, void **out) override
	{
		if (!out)
			return E_POINTER;
		*out = nullptr;
		if (riid == IID_IUnknown || riid == IID_IOleClientSite)
			*out = static_cast<IOleClientSite *>(this);
		else if (riid == IID_IServiceProvider)
			*out = static_cast<IServiceProvider *>(this);
		else if (riid == __uuidof(WMPLib::IWMPRemoteMediaServices))
			*out = static_cast<WMPLib::IWMPRemoteMediaServices *>(
				this);
		else
			return E_NOINTERFACE;
		AddRef();
		return S_OK;
	}

	/* IOleClientSite */
	HRESULT __stdcall SaveObject() override { return E_NOTIMPL; }
	HRESULT __stdcall GetMoniker(DWORD, DWORD, IMoniker **) override
	{
		return E_NOTIMPL;
	}
	HRESULT __stdcall GetContainer(IOleContainer **container) override
	{
		if (container)
			*container = nullptr;
		return E_NOINTERFACE;
	}
	HRESULT __stdcall ShowObject() override { return S_OK; }
	HRESULT __stdcall OnShowWindow(BOOL) override { return S_OK; }
	HRESULT __stdcall RequestNewObjectLayout() override
	{
		return E_NOTIMPL;
	}

	/* IServiceProvider */
	HRESULT __stdcall QueryService(REFGUID service, REFIID riid,
				       void **out) override
	{
		if (!out)
			return E_POINTER;
		*out = nullptr;
		if (service == __uuidof(WMPLib::IWMPRemoteMediaServices) ||
		    riid == __uuidof(WMPLib::IWMPRemoteMediaServices))
			return QueryInterface(riid, out);
		return E_NOINTERFACE;
	}

	/* IWMPRemoteMediaServices */
	HRESULT __stdcall raw_GetServiceType(BSTR *type) override
	{
		if (!type)
			return E_POINTER;
		*type = SysAllocString(L"Remote");
		return *type ? S_OK : E_OUTOFMEMORY;
	}

	HRESULT __stdcall raw_GetApplicationName(BSTR *name) override
	{
		if (!name)
			return E_POINTER;
		*name = SysAllocString(L"OBS WMP Legacy Now-Playing");
		return *name ? S_OK : E_OUTOFMEMORY;
	}

	HRESULT __stdcall raw_GetScriptableObject(BSTR *name,
						  IDispatch **dispatch) override
	{
		if (name)
			*name = nullptr;
		if (dispatch)
			*dispatch = nullptr;
		return E_NOTIMPL;
	}

	HRESULT __stdcall raw_GetCustomUIMode(BSTR *file) override
	{
		if (file)
			*file = nullptr;
		return E_NOTIMPL;
	}

private:
	volatile LONG refs_ = 1;
};

/* ===================================================================
 *  Persistent remote connection context
 *
 *  We keep the OLE object and player pointer alive across polls
 *  to avoid re-creating the COM connection on every tick.
 *  The connection is re-established if the player becomes invalid
 *  or if WMP is restarted.
 * =================================================================== */

struct WmpConnection {
	IOleObject *ole_object = nullptr;
	WMPLib::IWMPPlayer4Ptr player;

	bool is_valid() const
	{
		if (!player || !ole_object)
			return false;
		/* Probe the connection with a lightweight call */
		try {
			WMPLib::WMPPlayState ps;
			HRESULT hr = player->get_playState(&ps);
			return SUCCEEDED(hr);
		} catch (...) {
			return false;
		}
	}

	void close()
	{
		player = nullptr;
		if (ole_object) {
			ole_object->Close(OLECLOSE_NOSAVE);
			ole_object->Release();
			ole_object = nullptr;
		}
	}
};

static WmpConnection open_remote_connection()
{
	WmpConnection conn;

	auto *site = new RemoteHostSite();
	IOleObject *ole = nullptr;
	HRESULT hr = CoCreateInstance(__uuidof(WMPLib::WindowsMediaPlayer),
				     nullptr, CLSCTX_INPROC_SERVER,
				     IID_PPV_ARGS(&ole));
	if (FAILED(hr) || !ole) {
		site->Release();
		return conn;
	}

	hr = ole->SetClientSite(site);
	if (FAILED(hr)) {
		ole->Release();
		site->Release();
		return conn;
	}

	ole->SetHostNames(L"OBS WMP Legacy Now-Playing", nullptr);
	OleSetContainedObject(ole, TRUE);
	OleRun(ole);

	WMPLib::IWMPPlayer4Ptr player;
	hr = ole->QueryInterface(__uuidof(WMPLib::IWMPPlayer4),
				 reinterpret_cast<void **>(&player));
	if (FAILED(hr) || !player) {
		ole->Close(OLECLOSE_NOSAVE);
		ole->Release();
		site->Release();
		return conn;
	}

	/* Verify it actually connected to the running instance */
	VARIANT_BOOL remote = VARIANT_FALSE;
	player->get_isRemote(&remote);
	if (remote != VARIANT_TRUE) {
		/* Not connected to a running WMP — clean up */
		player = nullptr;
		ole->Close(OLECLOSE_NOSAVE);
		ole->Release();
		site->Release();
		return conn;
	}

	conn.ole_object = ole;
	conn.player = player;
	/* site ref is held by the OLE object */
	return conn;
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

MediaState capture_wmp_com_state(WmpConnection &conn)
{
	/* Ensure we have a valid connection */
	if (!conn.is_valid()) {
		conn.close();
		conn = open_remote_connection();
		if (!conn.player)
			return unavailable_state(
				"Windows Media Player (Legacy) is not running or not accessible");
	}

	auto &player = conn.player;

	/* Check play state */
	WMPLib::WMPPlayState play_state;
	try {
		play_state = player->playState;
	} catch (const _com_error &err) {
		/* Connection went stale — force reconnect next poll */
		conn.close();
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
		conn.close();
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
 *  WmpMonitor public interface
 * =================================================================== */

WmpMonitor::~WmpMonitor()
{
	stop();
	if (stop_event_)
		CloseHandle(static_cast<HANDLE>(stop_event_));
	if (wake_event_)
		CloseHandle(static_cast<HANDLE>(wake_event_));
}

void WmpMonitor::start()
{
	std::lock_guard lock(mutex_);
	if (started_)
		return;

	if (!stop_event_)
		stop_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	else
		ResetEvent(static_cast<HANDLE>(stop_event_));

	if (!wake_event_)
		wake_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	else
		ResetEvent(static_cast<HANDLE>(wake_event_));

	stop_requested_ = false;
	started_ = true;
	worker_ = std::thread(&WmpMonitor::run, this);
}

void WmpMonitor::stop()
{
	{
		std::lock_guard lock(mutex_);
		if (!started_)
			return;
		stop_requested_ = true;
	}

	if (stop_event_)
		SetEvent(static_cast<HANDLE>(stop_event_));
	if (wake_event_)
		SetEvent(static_cast<HANDLE>(wake_event_));

	if (worker_.joinable())
		worker_.join();

	std::lock_guard lock(mutex_);
	started_ = false;
}

void WmpMonitor::configure(std::string app_filter, uint32_t refresh_ms)
{
	{
		std::lock_guard lock(mutex_);
		app_filter_ = std::move(app_filter);
		refresh_ms_ = std::clamp<uint32_t>(refresh_ms, 250, 5000);
	}

	if (wake_event_)
		SetEvent(static_cast<HANDLE>(wake_event_));
}

MediaState WmpMonitor::snapshot() const
{
	std::lock_guard lock(mutex_);
	return state_;
}

/* ===================================================================
 *  Worker thread: dedicated STA apartment for COM calls
 * =================================================================== */

void WmpMonitor::run()
{
	/* OleInitialize is needed instead of CoInitializeEx because we
	   use OLE embedding interfaces (IOleClientSite, OleRun, etc.) */
	OleInitialize(nullptr);

	WmpConnection conn;

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
			next_state = capture_wmp_com_state(conn);
		} catch (const _com_error &err) {
			next_state =
				unavailable_state(tchar_to_utf8(err.ErrorMessage()));
			conn.close();
		} catch (const std::exception &err) {
			next_state = unavailable_state(err.what());
			conn.close();
		} catch (...) {
			next_state =
				unavailable_state("Unknown error in WMP COM polling");
			conn.close();
		}

		{
			std::lock_guard lock(mutex_);
			state_ = std::move(next_state);
		}

		if (stop_requested_)
			break;

		HANDLE events[2] = { static_cast<HANDLE>(stop_event_), static_cast<HANDLE>(wake_event_) };
		DWORD timeout = refresh_ms;
		ULONGLONG start_tick = GetTickCount64();

		while (true) {
			DWORD wait_res = MsgWaitForMultipleObjects(2, events, FALSE, timeout, QS_ALLINPUT);
			if (wait_res == WAIT_OBJECT_0) {
				break;
			} else if (wait_res == WAIT_OBJECT_0 + 1) {
				break;
			} else if (wait_res == WAIT_OBJECT_0 + 2) {
				MSG msg;
				while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
					TranslateMessage(&msg);
					DispatchMessage(&msg);
				}
			} else if (wait_res == WAIT_TIMEOUT) {
				break;
			}

			ULONGLONG elapsed = GetTickCount64() - start_tick;
			if (elapsed >= refresh_ms)
				break;
			timeout = refresh_ms - static_cast<DWORD>(elapsed);
		}

		if (WaitForSingleObject(events[0], 0) == WAIT_OBJECT_0)
			break;
	}

	/* Clean up the persistent connection before COM teardown */
	conn.close();
	OleUninitialize();
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
