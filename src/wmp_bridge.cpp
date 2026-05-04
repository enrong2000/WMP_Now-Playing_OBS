/* wmp_bridge.exe — Standalone helper that connects to a running
   Windows Media Player (Legacy) instance via COM Remote mode and
   outputs the current media state as a single JSON object to stdout.
   
   This runs as a separate process to avoid COM apartment conflicts
   with OBS's in-process threading model. */

#include <Windows.h>
#include <ObjBase.h>
#include <OleCtl.h>
#include <comdef.h>

#import "wmp.dll" rename_namespace("WMPLib") named_guids

#include <cstdio>
#include <string>
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

/* ---- Main ---- */

int wmain()
{
	/* Ensure stdout is in binary mode for clean UTF-8 output */
	_setmode(_fileno(stdout), _O_BINARY);

	HRESULT hr = OleInitialize(nullptr);
	if (FAILED(hr)) {
		fprintf(stdout,
			"{\"error\":\"OleInitialize failed: 0x%08lX\"}\n",
			(unsigned long)hr);
		fflush(stdout);
		return 1;
	}

	auto *site = new RemoteHostSite();
	IOleObject *ole = nullptr;
	hr = CoCreateInstance(__uuidof(WMPLib::WindowsMediaPlayer), nullptr,
			     CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ole));
	if (FAILED(hr) || !ole) {
		fprintf(stdout,
			"{\"error\":\"CoCreateInstance failed: 0x%08lX\"}\n",
			(unsigned long)hr);
		fflush(stdout);
		site->Release();
		OleUninitialize();
		return 1;
	}

	hr = ole->SetClientSite(site);
	if (FAILED(hr)) {
		fprintf(stdout,
			"{\"error\":\"SetClientSite failed: 0x%08lX\"}\n",
			(unsigned long)hr);
		fflush(stdout);
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
		fprintf(stdout,
			"{\"error\":\"QueryInterface IWMPPlayer4 failed: 0x%08lX\"}\n",
			(unsigned long)hr);
		fflush(stdout);
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
		fprintf(stdout, "{\"available\":false,\"status\":\"%s\","
				"\"diagnostic\":\"no currentMedia\"}\n",
			status_str.c_str());
		fflush(stdout);
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

	/* Extract playlist */
	std::string playlist_json = "[]";
	int current_index = -1;
	try {
		WMPLib::IWMPPlaylistPtr pl = player->currentPlaylist;
		if (pl) {
			long count = pl->count;
			std::string source_url;
			try {
				source_url = bstr_to_utf8(media->sourceURL);
			} catch (...) {
			}

			std::string items;
			for (long i = 0; i < count; ++i) {
				WMPLib::IWMPMediaPtr pm;
				try {
					HRESULT phr = pl->get_Item(i, &pm);
					if (FAILED(phr) || !pm)
						continue;
				} catch (...) {
					continue;
				}

				std::string pt, pa, pal;
				double pdur = 0;
				try {
					pt = bstr_to_utf8(pm->name);
				} catch (...) {
				}
				try {
					pa = bstr_to_utf8(pm->getItemInfo(
						_bstr_t(L"Author")));
				} catch (...) {
				}
				try {
					pal = bstr_to_utf8(pm->getItemInfo(
						_bstr_t(L"WM/AlbumTitle")));
				} catch (...) {
				}
				try {
					pdur = pm->duration;
				} catch (...) {
				}

				if (current_index < 0 && !source_url.empty()) {
					try {
						std::string su = bstr_to_utf8(
							pm->sourceURL);
						if (su == source_url)
							current_index =
								static_cast<int>(i);
					} catch (...) {
					}
				}

				if (!items.empty())
					items += ",";
				char buf[64];
				snprintf(buf, sizeof(buf), "%.3f", pdur);
				items += "{\"index\":" + std::to_string(i) +
					 ",\"title\":\"" + json_escape(pt) +
					 "\",\"artist\":\"" +
					 json_escape(pa) +
					 "\",\"album\":\"" +
					 json_escape(pal) +
					 "\",\"duration_sec\":" +
					 std::string(buf) + "}";
			}
			playlist_json = "[" + items + "]";
		}
	} catch (...) {
	}

	/* Format position/duration as ms */
	long long pos_ms = static_cast<long long>(position * 1000.0);
	long long dur_ms = static_cast<long long>(duration * 1000.0);
	double progress = duration > 0 ? position / duration : 0;

	/* Output JSON */
	fprintf(stdout,
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
		"\"current_playlist_index\":%d,"
		"\"playlist\":%s}\n",
		json_escape(title).c_str(), json_escape(artist).c_str(),
		json_escape(album).c_str(),
		json_escape(album_artist).c_str(),
		json_escape(composer).c_str(), status_str.c_str(), pos_ms,
		dur_ms, progress, current_index, playlist_json.c_str());
	fflush(stdout);

	/* Cleanup */
	media = nullptr;
	player = nullptr;
	ole->Close(OLECLOSE_NOSAVE);
	ole->Release();
	site->Release();
	OleUninitialize();
	return 0;
}
