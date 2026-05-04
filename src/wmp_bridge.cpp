/* wmp_bridge.exe — Standalone helper that connects to a running
   Windows Media Player (Legacy) instance via COM Remote mode and
   outputs the current media state as a single JSON object.

   This runs as a separate process to avoid COM apartment conflicts
   with OBS's in-process threading model.

   Usage:
     wmp_bridge.exe                     — write JSON to stdout
     wmp_bridge.exe --output <path>     — write JSON to file (for
                                          de-elevation IPC where
                                          pipes can't be inherited)
*/

#include <Windows.h>
#include <ObjBase.h>
#include <OleCtl.h>
#include <comdef.h>

#import "wmp.dll" rename_namespace("WMPLib") named_guids

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <io.h>
#include <fcntl.h>

/* ---- COM host site for WMP Remote mode ---- */

class RemoteHostSite final : public IOleClientSite,
			     public IServiceProvider,
			     public WMPLib::IWMPRemoteMediaServices {
public:
	ULONG __stdcall AddRef() override
	{
		return InterlockedIncrement(&refs_);
	}
	ULONG __stdcall Release() override
	{
		const ULONG r = InterlockedDecrement(&refs_);
		if (r == 0)
			delete this;
		return r;
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
	HRESULT __stdcall SaveObject() override { return E_NOTIMPL; }
	HRESULT __stdcall GetMoniker(DWORD, DWORD, IMoniker **) override
	{
		return E_NOTIMPL;
	}
	HRESULT __stdcall GetContainer(IOleContainer **c) override
	{
		if (c)
			*c = nullptr;
		return E_NOINTERFACE;
	}
	HRESULT __stdcall ShowObject() override { return S_OK; }
	HRESULT __stdcall OnShowWindow(BOOL) override { return S_OK; }
	HRESULT __stdcall RequestNewObjectLayout() override
	{
		return E_NOTIMPL;
	}
	HRESULT __stdcall QueryService(REFGUID s, REFIID r, void **o) override
	{
		if (!o)
			return E_POINTER;
		*o = nullptr;
		if (s == __uuidof(WMPLib::IWMPRemoteMediaServices) ||
		    r == __uuidof(WMPLib::IWMPRemoteMediaServices))
			return QueryInterface(r, o);
		return E_NOINTERFACE;
	}
	HRESULT __stdcall raw_GetServiceType(BSTR *t) override
	{
		if (!t)
			return E_POINTER;
		*t = SysAllocString(L"Remote");
		return *t ? S_OK : E_OUTOFMEMORY;
	}
	HRESULT __stdcall raw_GetApplicationName(BSTR *n) override
	{
		if (!n)
			return E_POINTER;
		*n = SysAllocString(L"OBS WMP Bridge");
		return *n ? S_OK : E_OUTOFMEMORY;
	}
	HRESULT __stdcall raw_GetScriptableObject(BSTR *n,
						  IDispatch **d) override
	{
		if (n)
			*n = nullptr;
		if (d)
			*d = nullptr;
		return E_NOTIMPL;
	}
	HRESULT __stdcall raw_GetCustomUIMode(BSTR *f) override
	{
		if (f)
			*f = nullptr;
		return E_NOTIMPL;
	}

private:
	volatile LONG refs_ = 1;
};

/* ---- String helpers ---- */

static std::string wide_to_utf8(const wchar_t *value)
{
	if (!value || !*value)
		return {};
	const int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr,
					       0, nullptr, nullptr);
	if (needed <= 1)
		return {};
	std::string out(static_cast<size_t>(needed - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), needed, nullptr,
			    nullptr);
	return out;
}

static std::string bstr_to_utf8(const _bstr_t &bs)
{
	return wide_to_utf8(static_cast<const wchar_t *>(bs));
}

static std::string json_escape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		switch (c) {
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
			if (static_cast<unsigned char>(c) < 0x20)
				continue;
			out += c;
		}
	}
	return out;
}

/* ---- Playlist item serialization ---- */

struct TrackInfo {
	int index;
	std::string title;
	std::string artist;
	std::string album;
	double duration_sec;
	std::string source_url;
};

static std::string serialize_track_list(const std::vector<TrackInfo> &tracks)
{
	std::string items;
	for (size_t i = 0; i < tracks.size(); ++i) {
		const auto &t = tracks[i];
		char buf[64];
		snprintf(buf, sizeof(buf), "%.3f", t.duration_sec);
		if (!items.empty())
			items += ",";
		items += "{\"index\":" + std::to_string(t.index) +
			 ",\"title\":\"" + json_escape(t.title) +
			 "\",\"artist\":\"" + json_escape(t.artist) +
			 "\",\"album\":\"" + json_escape(t.album) +
			 "\",\"duration_sec\":" + std::string(buf) + "}";
	}
	return "[" + items + "]";
}

static std::vector<TrackInfo>
enumerate_playlist(WMPLib::IWMPPlaylistPtr pl,
		   const std::string &current_source_url, int &out_index)
{
	std::vector<TrackInfo> tracks;
	out_index = -1;
	if (!pl)
		return tracks;

	long count = 0;
	try {
		count = pl->count;
	} catch (...) {
		return tracks;
	}

	for (long i = 0; i < count; ++i) {
		WMPLib::IWMPMediaPtr pm;
		try {
			HRESULT phr = pl->get_Item(i, &pm);
			if (FAILED(phr) || !pm)
				continue;
		} catch (...) {
			continue;
		}

		TrackInfo t;
		t.index = static_cast<int>(i);
		try {
			t.title = bstr_to_utf8(pm->name);
		} catch (...) {
		}
		try {
			t.artist = bstr_to_utf8(
				pm->getItemInfo(_bstr_t(L"Author")));
		} catch (...) {
		}
		try {
			t.album = bstr_to_utf8(
				pm->getItemInfo(_bstr_t(L"WM/AlbumTitle")));
		} catch (...) {
		}
		try {
			t.duration_sec = pm->duration;
		} catch (...) {
		}
		try {
			t.source_url = bstr_to_utf8(pm->sourceURL);
		} catch (...) {
		}

		if (out_index < 0 && !current_source_url.empty() &&
		    t.source_url == current_source_url)
			out_index = static_cast<int>(i);

		tracks.push_back(std::move(t));
	}
	return tracks;
}

/* ---- Try to find the full source playlist via IWMPPlaylistCollection ---- */

static std::vector<TrackInfo>
find_full_playlist(WMPLib::IWMPPlayer4Ptr player,
		   const std::string &current_playlist_name,
		   const std::string &current_source_url,
		   int &out_full_index)
{
	out_full_index = -1;
	std::vector<TrackInfo> result;

	if (!player || current_playlist_name.empty())
		return result;

	try {
		WMPLib::IWMPPlaylistCollectionPtr plcol =
			player->playlistCollection;
		if (!plcol)
			return result;

		/* Search for a playlist with the same name as the current one */
		std::wstring wname;

		/* Convert the playlist name from UTF-8 to wide string */
		int needed = MultiByteToWideChar(
			CP_UTF8, 0, current_playlist_name.c_str(),
			static_cast<int>(current_playlist_name.size()), nullptr,
			0);
		if (needed > 0) {
			wname.resize(static_cast<size_t>(needed));
			MultiByteToWideChar(
				CP_UTF8, 0, current_playlist_name.c_str(),
				static_cast<int>(current_playlist_name.size()),
				wname.data(), needed);
		}

		WMPLib::IWMPPlaylistArrayPtr arr =
			plcol->getByName(_bstr_t(wname.c_str()));
		if (!arr)
			return result;

		long arr_count = arr->count;
		for (long a = 0; a < arr_count; ++a) {
			WMPLib::IWMPPlaylistPtr full_pl;
			try {
				full_pl = arr->Item(a);
			} catch (...) {
				continue;
			}
			if (!full_pl)
				continue;

			long full_count = 0;
			try {
				full_count = full_pl->count;
			} catch (...) {
				continue;
			}

			/* Use the playlist with the most items
			   (the source playlist should be larger than
			   the truncated current queue) */
			if (full_count > static_cast<long>(result.size())) {
				result = enumerate_playlist(
					full_pl, current_source_url,
					out_full_index);
			}
		}
	} catch (...) {
	}

	return result;
}

/* ---- Output helpers ---- */

static FILE *open_output(const char *output_path)
{
	if (output_path && output_path[0]) {
		/* Convert to wide for proper Unicode path support */
		int needed = MultiByteToWideChar(
			CP_UTF8, 0, output_path,
			static_cast<int>(strlen(output_path)), nullptr, 0);
		if (needed > 0) {
			std::wstring wpath(static_cast<size_t>(needed), L'\0');
			MultiByteToWideChar(
				CP_UTF8, 0, output_path,
				static_cast<int>(strlen(output_path)),
				wpath.data(), needed);
			FILE *f = nullptr;
			if (_wfopen_s(&f, wpath.c_str(), L"wb") == 0 && f)
				return f;
		}
		/* Fallback: try ANSI */
		FILE *f = nullptr;
		if (fopen_s(&f, output_path, "wb") == 0 && f)
			return f;
	}
	return stdout;
}

/* ---- Main ---- */

int wmain(int argc, wchar_t *argv[])
{
	/* Ensure stdout is in binary mode for clean UTF-8 output */
	_setmode(_fileno(stdout), _O_BINARY);

	/* Parse --output argument */
	std::string output_path;
	for (int i = 1; i < argc; ++i) {
		if (wcscmp(argv[i], L"--output") == 0 && i + 1 < argc) {
			output_path = wide_to_utf8(argv[i + 1]);
			++i;
		}
	}

	FILE *out = open_output(output_path.c_str());
	const bool using_file = (out != stdout);

	auto emit_error = [&](const char *fmt, unsigned long code) {
		fprintf(out, fmt, code);
		fflush(out);
		if (using_file)
			fclose(out);
	};

	HRESULT hr = OleInitialize(nullptr);
	if (FAILED(hr)) {
		emit_error(
			"{\"error\":\"OleInitialize failed: 0x%08lX\"}\n",
			(unsigned long)hr);
		return 1;
	}

	auto *site = new RemoteHostSite();
	IOleObject *ole = nullptr;
	hr = CoCreateInstance(__uuidof(WMPLib::WindowsMediaPlayer), nullptr,
			     CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ole));
	if (FAILED(hr) || !ole) {
		emit_error(
			"{\"error\":\"CoCreateInstance failed: 0x%08lX\"}\n",
			(unsigned long)hr);
		site->Release();
		OleUninitialize();
		return 1;
	}

	hr = ole->SetClientSite(site);
	if (FAILED(hr)) {
		emit_error(
			"{\"error\":\"SetClientSite failed: 0x%08lX\"}\n",
			(unsigned long)hr);
		ole->Release();
		site->Release();
		OleUninitialize();
		return 1;
	}

	ole->SetHostNames(L"OBS WMP Bridge", nullptr);
	OleSetContainedObject(ole, TRUE);
	OleRun(ole);

	WMPLib::IWMPPlayer4Ptr player;
	hr = ole->QueryInterface(__uuidof(WMPLib::IWMPPlayer4),
				 reinterpret_cast<void **>(&player));
	if (FAILED(hr) || !player) {
		emit_error(
			"{\"error\":\"QueryInterface IWMPPlayer4 failed: 0x%08lX\"}\n",
			(unsigned long)hr);
		ole->Close(OLECLOSE_NOSAVE);
		ole->Release();
		site->Release();
		OleUninitialize();
		return 1;
	}

	/* Retrieve media state */
	std::string status_str = "Unknown";
	try {
		WMPLib::WMPPlayState ps = player->playState;
		switch (ps) {
		case WMPLib::wmppsPlaying:
			status_str = "Playing";
			break;
		case WMPLib::wmppsPaused:
			status_str = "Paused";
			break;
		case WMPLib::wmppsStopped:
			status_str = "Stopped";
			break;
		case WMPLib::wmppsTransitioning:
			status_str = "Changing";
			break;
		default:
			status_str = "Opened";
			break;
		}
	} catch (...) {
	}

	WMPLib::IWMPMediaPtr media;
	try {
		media = player->currentMedia;
	} catch (...) {
	}

	if (!media) {
		fprintf(out,
			"{\"available\":false,\"status\":\"%s\","
			"\"diagnostic\":\"no currentMedia\"}\n",
			status_str.c_str());
		fflush(out);
		if (using_file)
			fclose(out);
		player = nullptr;
		ole->Close(OLECLOSE_NOSAVE);
		ole->Release();
		site->Release();
		OleUninitialize();
		return 0;
	}

	/* Extract metadata */
	std::string title, artist, album, album_artist, composer;
	double duration = 0, position = 0;
	std::string source_url;
	try {
		title = bstr_to_utf8(media->name);
	} catch (...) {
	}
	try {
		artist = bstr_to_utf8(
			media->getItemInfo(_bstr_t(L"Author")));
	} catch (...) {
	}
	try {
		album = bstr_to_utf8(
			media->getItemInfo(_bstr_t(L"WM/AlbumTitle")));
	} catch (...) {
	}
	try {
		album_artist = bstr_to_utf8(
			media->getItemInfo(_bstr_t(L"WM/AlbumArtist")));
	} catch (...) {
	}
	try {
		composer = bstr_to_utf8(
			media->getItemInfo(_bstr_t(L"WM/Composer")));
	} catch (...) {
	}
	try {
		duration = media->duration;
	} catch (...) {
	}
	try {
		WMPLib::IWMPControlsPtr controls = player->controls;
		if (controls)
			position = controls->currentPosition;
	} catch (...) {
	}
	try {
		source_url = bstr_to_utf8(media->sourceURL);
	} catch (...) {
	}

	/* Extract current playlist (the active playback queue) */
	int current_index = -1;
	std::vector<TrackInfo> current_tracks;
	std::string current_playlist_name;
	try {
		WMPLib::IWMPPlaylistPtr pl = player->currentPlaylist;
		if (pl) {
			try {
				current_playlist_name =
					bstr_to_utf8(pl->name);
			} catch (...) {
			}
			current_tracks = enumerate_playlist(pl, source_url,
							    current_index);
		}
	} catch (...) {
	}

	/* Try to enumerate the full source playlist via
	   IWMPPlaylistCollection (the currentPlaylist in COM Remote
	   mode may only contain a subset of the source playlist) */
	int full_index = -1;
	std::vector<TrackInfo> full_tracks = find_full_playlist(
		player, current_playlist_name, source_url, full_index);

	/* Format position/duration as ms */
	long long pos_ms = static_cast<long long>(position * 1000.0);
	long long dur_ms = static_cast<long long>(duration * 1000.0);
	double progress = duration > 0 ? position / duration : 0;

	/* Output JSON */
	fprintf(out,
		"{\"available\":true,"
		"\"title\":\"%s\","
		"\"artist\":\"%s\","
		"\"album\":\"%s\","
		"\"album_artist\":\"%s\","
		"\"composer\":\"%s\","
		"\"status\":\"%s\","
		"\"position_ms\":%lld,"
		"\"duration_ms\":%lld,"
		"\"progress\":%.6f,"
		"\"current_playlist_name\":\"%s\","
		"\"current_playlist_index\":%d,"
		"\"playlist\":%s,"
		"\"full_playlist_index\":%d,"
		"\"full_playlist\":%s}\n",
		json_escape(title).c_str(), json_escape(artist).c_str(),
		json_escape(album).c_str(),
		json_escape(album_artist).c_str(),
		json_escape(composer).c_str(), status_str.c_str(), pos_ms,
		dur_ms, progress,
		json_escape(current_playlist_name).c_str(), current_index,
		serialize_track_list(current_tracks).c_str(), full_index,
		serialize_track_list(full_tracks).c_str());
	fflush(out);
	if (using_file)
		fclose(out);

	/* Cleanup */
	media = nullptr;
	player = nullptr;
	ole->Close(OLECLOSE_NOSAVE);
	ole->Release();
	site->Release();
	OleUninitialize();
	return 0;
}
