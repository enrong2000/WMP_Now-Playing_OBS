#include "wmp_monitor.hpp"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>
#include <TlHelp32.h>

#include <obs-module.h>

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

std::wstring utf8_to_wide(std::string_view value)
{
	if (value.empty())
		return {};

	const int needed = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), nullptr, 0);
	if (needed > 0) {
		std::wstring converted(static_cast<size_t>(needed), L'\0');
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
				    static_cast<int>(value.size()),
				    converted.data(), needed);
		return converted;
	}

	/* Fallback: try the system ANSI codepage */
	const int fallback_needed = MultiByteToWideChar(
		CP_ACP, 0, value.data(), static_cast<int>(value.size()),
		nullptr, 0);
	if (fallback_needed <= 0)
		return {};

	std::wstring converted(static_cast<size_t>(fallback_needed), L'\0');
	MultiByteToWideChar(CP_ACP, 0, value.data(),
			    static_cast<int>(value.size()), converted.data(),
			    fallback_needed);
	return converted;
}

std::wstring lowercase(std::wstring value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		       [](wchar_t ch) {
			       return static_cast<wchar_t>(std::towlower(ch));
		       });
	return value;
}

/* ===================================================================
 *  Process detection — check if wmplayer.exe is running
 * =================================================================== */

bool matching_wmp_process_is_running(const std::string &app_filter)
{
	std::wstring filter = lowercase(utf8_to_wide(app_filter));

	/* Normalize common filter values to wmplayer */
	if (filter.empty() || filter.find(L"wmp") != std::wstring::npos ||
	    filter.find(L"wmplayer") != std::wstring::npos ||
	    filter.find(L"windows media") != std::wstring::npos) {
		filter = L"wmplayer";
	}

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return true; /* assume running if we can't check */

	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);

	bool found = false;
	if (Process32FirstW(snapshot, &entry)) {
		do {
			const std::wstring exe = lowercase(entry.szExeFile);
			if (exe.find(filter) != std::wstring::npos) {
				found = true;
				break;
			}
		} while (Process32NextW(snapshot, &entry));
	}

	CloseHandle(snapshot);
	return found;
}

/* ===================================================================
 *  Helper: build an unavailable MediaState
 * =================================================================== */

MediaState unavailable_state(std::string message,
			     bool legacy_wmp_running = false)
{
	MediaState state;
	state.available = false;
	state.legacy_wmp_running = legacy_wmp_running;
	if (legacy_wmp_running)
		state.source_app_id = "wmplayer.exe";
	state.error_message = std::move(message);
	state.captured_at = std::chrono::steady_clock::now();
	return state;
}

/* ===================================================================
 *  Subprocess bridge — spawn wmp_bridge.exe and read JSON stdout
 *
 *  The in-process WMP COM OCX approach is unreliable inside OBS due
 *  to COM apartment conflicts with OBS's threading model.  Instead
 *  we spawn wmp_bridge.exe (a small standalone helper) that performs
 *  the COM work in a clean process and outputs JSON to stdout.
 * =================================================================== */

/**
 * Locate the wmp_bridge.exe helper binary.
 *
 * Search order:
 *   1. Beside the plugin DLL (obs-plugins/64bit/wmp_bridge.exe)
 *   2. In the plugin data directory (via obs_module_file)
 */
static std::string find_bridge_exe()
{
	/* 1. Look beside the plugin DLL itself */
	HMODULE hmod = nullptr;
	if (GetModuleHandleExW(
		    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		    reinterpret_cast<LPCWSTR>(&find_bridge_exe), &hmod)) {
		wchar_t dll_path[MAX_PATH];
		if (GetModuleFileNameW(hmod, dll_path, MAX_PATH)) {
			std::filesystem::path candidate =
				std::filesystem::path(dll_path).parent_path() /
				"wmp_bridge.exe";
			std::error_code ec;
			if (std::filesystem::exists(candidate, ec))
				return candidate.string();
		}
	}

	/* 2. Look in the plugin data directory (obs_module_file) */
	char *module_path = obs_module_file("wmp_bridge.exe");
	if (module_path) {
		std::filesystem::path p(module_path);
		bfree(module_path);
		std::error_code ec;
		p = std::filesystem::absolute(p, ec);
		if (!ec && std::filesystem::exists(p, ec))
			return p.string();
	}

	return {};
}

/* ===================================================================
 *  Elevation detection and de-elevation helpers
 *
 *  When OBS runs as Administrator, spawned child processes inherit
 *  the elevated token.  WMP COM Remote mode fails when an elevated
 *  process tries to connect to a non-elevated WMP instance due to
 *  COM cross-integrity-level restrictions.  To fix this, we detect
 *  elevation and spawn the bridge with a de-elevated (medium-
 *  integrity) token obtained from the linked token.
 * =================================================================== */

static bool is_process_elevated()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return false;

	TOKEN_ELEVATION elev = {};
	DWORD size = sizeof(elev);
	BOOL result = GetTokenInformation(token, TokenElevation, &elev,
					  sizeof(elev), &size);
	CloseHandle(token);
	return result && elev.TokenIsElevated;
}

/**
 * Obtain a non-elevated primary token suitable for CreateProcessWithTokenW.
 *
 * When the current process is elevated via UAC, its token has a "linked
 * token" that represents the original non-elevated user identity.  We
 * retrieve it and duplicate it as a primary token.
 *
 * Returns nullptr if the linked token cannot be obtained.
 */
static HANDLE get_non_elevated_token()
{
	HANDLE process_token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE,
			      &process_token))
		return nullptr;

	TOKEN_LINKED_TOKEN linked = {};
	DWORD size = sizeof(linked);
	BOOL ok = GetTokenInformation(process_token, TokenLinkedToken,
				      &linked, sizeof(linked), &size);
	CloseHandle(process_token);

	if (!ok || !linked.LinkedToken)
		return nullptr;

	/* The linked token is an identification-level impersonation token;
	   duplicate it as a primary token for CreateProcessWithTokenW. */
	HANDLE primary = nullptr;
	ok = DuplicateTokenEx(linked.LinkedToken,
			      TOKEN_QUERY | TOKEN_DUPLICATE |
				      TOKEN_ASSIGN_PRIMARY |
				      TOKEN_ADJUST_DEFAULT |
				      TOKEN_ADJUST_SESSIONID,
			      nullptr, SecurityImpersonation, TokenPrimary,
			      &primary);
	CloseHandle(linked.LinkedToken);
	return ok ? primary : nullptr;
}

/**
 * Spawn the bridge process and capture its stdout output.
 *
 * When the host process (OBS) is running elevated, the bridge is
 * launched with a de-elevated token so it can connect to the user's
 * non-elevated WMP instance via COM Remote mode.
 *
 * Returns the raw JSON string, or empty on failure.
 */
static std::string run_bridge_process(const std::string &exe_path)
{
	SECURITY_ATTRIBUTES sa = {};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	HANDLE read_pipe = nullptr, write_pipe = nullptr;
	if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
		return {};
	/* Only the write end should be inherited by the child */
	SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.hStdOutput = write_pipe;
	si.hStdError = write_pipe;
	si.hStdInput = nullptr;
	si.wShowWindow = SW_HIDE;

	PROCESS_INFORMATION pi = {};

	std::wstring wpath = utf8_to_wide(exe_path);
	BOOL ok = FALSE;

	/* Try de-elevated launch first when running as Administrator */
	HANDLE deelev_token = nullptr;
	if (is_process_elevated()) {
		deelev_token = get_non_elevated_token();
	}

	if (deelev_token) {
		ok = CreateProcessWithTokenW(
			deelev_token, 0 /* dwLogonFlags */,
			wpath.c_str(), nullptr /* lpCommandLine */,
			CREATE_NO_WINDOW, nullptr /* lpEnvironment */,
			nullptr /* lpCurrentDirectory */, &si, &pi);

		if (!ok) {
			blog(LOG_WARNING,
			     "[obs-wmp-legacy] de-elevated launch failed "
			     "(0x%08lX), falling back to normal launch",
			     (unsigned long)GetLastError());
		}
		CloseHandle(deelev_token);
	}

	if (!ok) {
		/* Normal launch (not elevated, or de-elevation failed) */
		ok = CreateProcessW(wpath.c_str(), nullptr, nullptr, nullptr,
				    TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
				    &si, &pi);
	}

	/* Close the write end in our process so ReadFile can detect EOF */
	CloseHandle(write_pipe);

	if (!ok) {
		CloseHandle(read_pipe);
		return {};
	}

	/* Read stdout with a 5-second timeout */
	std::string output;
	output.reserve(8192);
	char buf[4096];
	const ULONGLONG deadline = GetTickCount64() + 5000;

	for (;;) {
		if (GetTickCount64() >= deadline)
			break;

		/* If the process has exited, drain remaining output */
		if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
			DWORD bytes = 0;
			while (ReadFile(read_pipe, buf, sizeof(buf), &bytes,
					nullptr) &&
			       bytes > 0) {
				output.append(buf, bytes);
			}
			break;
		}

		DWORD avail = 0;
		if (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &avail,
				  nullptr) &&
		    avail > 0) {
			DWORD bytes = 0;
			DWORD to_read = static_cast<DWORD>(
				(std::min<size_t>)(avail, sizeof(buf)));
			if (ReadFile(read_pipe, buf, to_read, &bytes,
				     nullptr) &&
			    bytes > 0) {
				output.append(buf, bytes);
			}
		} else {
			Sleep(10);
		}
	}

	/* Kill the process if it hasn't exited yet */
	if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) {
		TerminateProcess(pi.hProcess, 1);
		WaitForSingleObject(pi.hProcess, 1000);
	}

	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CloseHandle(read_pipe);

	return output;
}

/* ===================================================================
 *  Minimal JSON parsers — no external dependency required
 * =================================================================== */

static std::string json_string_value(const std::string &json,
				     const std::string &key)
{
	std::string search = "\"" + key + "\":\"";
	auto pos = json.find(search);
	if (pos == std::string::npos)
		return {};
	pos += search.size();
	std::string result;
	for (size_t i = pos; i < json.size(); ++i) {
		if (json[i] == '"' && (i == pos || json[i - 1] != '\\'))
			break;
		if (json[i] == '\\' && i + 1 < json.size()) {
			char next = json[i + 1];
			if (next == '"' || next == '\\') {
				result += next;
				++i;
				continue;
			}
			if (next == 'n') {
				result += '\n';
				++i;
				continue;
			}
		}
		result += json[i];
	}
	return result;
}

static int64_t json_int_value(const std::string &json, const std::string &key)
{
	std::string search = "\"" + key + "\":";
	auto pos = json.find(search);
	if (pos == std::string::npos)
		return 0;
	pos += search.size();
	while (pos < json.size() && json[pos] == ' ')
		++pos;
	return std::strtoll(json.c_str() + pos, nullptr, 10);
}

static double json_double_value(const std::string &json,
				const std::string &key)
{
	std::string search = "\"" + key + "\":";
	auto pos = json.find(search);
	if (pos == std::string::npos)
		return 0.0;
	pos += search.size();
	while (pos < json.size() && json[pos] == ' ')
		++pos;
	return std::strtod(json.c_str() + pos, nullptr);
}

static bool json_bool_value(const std::string &json, const std::string &key)
{
	std::string search = "\"" + key + "\":";
	auto pos = json.find(search);
	if (pos == std::string::npos)
		return false;
	pos += search.size();
	while (pos < json.size() && json[pos] == ' ')
		++pos;
	return pos < json.size() && json[pos] == 't';
}

static std::vector<PlaylistItem> parse_playlist_json(const std::string &json)
{
	std::vector<PlaylistItem> items;
	auto start = json.find("\"playlist\":[");
	if (start == std::string::npos)
		return items;
	start += 12; /* skip past "playlist":[ */

	size_t pos = start;
	auto list_end = json.find(']', start);
	while (pos < json.size() &&
	       (list_end == std::string::npos || pos < list_end)) {
		auto obj_start = json.find('{', pos);
		if (obj_start == std::string::npos)
			break;
		if (list_end != std::string::npos && obj_start > list_end)
			break;
		auto obj_end = json.find('}', obj_start);
		if (obj_end == std::string::npos)
			break;

		std::string obj =
			json.substr(obj_start, obj_end - obj_start + 1);

		PlaylistItem item;
		item.index = static_cast<int>(json_int_value(obj, "index"));
		item.title = json_string_value(obj, "title");
		item.artist = json_string_value(obj, "artist");
		item.album = json_string_value(obj, "album");
		item.duration_sec = json_double_value(obj, "duration_sec");
		items.push_back(std::move(item));

		pos = obj_end + 1;
	}
	return items;
}

/* ===================================================================
 *  Build MediaState from bridge JSON output
 * =================================================================== */

MediaState capture_via_bridge(const std::string &bridge_exe,
			      const std::string &app_filter)
{
	if (!matching_wmp_process_is_running(app_filter))
		return unavailable_state(
			"Windows Media Player (Legacy) is not running");

	if (bridge_exe.empty())
		return unavailable_state(
			"wmp_bridge.exe not found in plugin directory", true);

	std::string json = run_bridge_process(bridge_exe);
	if (json.empty())
		return unavailable_state(
			"wmp_bridge.exe failed to produce output", true);

	std::string error = json_string_value(json, "error");
	if (!error.empty())
		return unavailable_state("Bridge: " + error, true);

	bool available = json_bool_value(json, "available");
	if (!available) {
		std::string diag = json_string_value(json, "diagnostic");
		return unavailable_state(
			diag.empty() ? "WMP reported no media" : diag, true);
	}

	MediaState state;
	state.available = true;
	state.legacy_wmp_running = true;
	state.backend = "WMP Legacy COM";
	state.source_app_id = "wmplayer.exe";

	state.title = json_string_value(json, "title");
	state.artist = json_string_value(json, "artist");
	state.album = json_string_value(json, "album");
	state.album_artist = json_string_value(json, "album_artist");
	state.composer = json_string_value(json, "composer");

	std::string status = json_string_value(json, "status");
	if (status == "Playing")
		state.playback_status = PlaybackStatus::playing;
	else if (status == "Paused")
		state.playback_status = PlaybackStatus::paused;
	else if (status == "Stopped")
		state.playback_status = PlaybackStatus::stopped;
	else if (status == "Changing")
		state.playback_status = PlaybackStatus::changing;
	else if (status == "Opened")
		state.playback_status = PlaybackStatus::opened;
	else
		state.playback_status = PlaybackStatus::unknown;

	state.position_ms = json_int_value(json, "position_ms");
	state.end_ms = json_int_value(json, "duration_ms");
	state.start_ms = 0;
	state.timeline_available = state.end_ms > 0;
	state.captured_at = std::chrono::steady_clock::now();

	state.current_playlist_index =
		static_cast<int>(json_int_value(json, "current_playlist_index"));
	state.playlist = parse_playlist_json(json);

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

	if (worker_.joinable())
		worker_.join();

	{
		std::lock_guard lock(mutex_);
		started_ = false;
	}
}

void WmpMonitor::configure(std::string app_filter, uint32_t refresh_ms)
{
	std::lock_guard lock(mutex_);
	app_filter_ = std::move(app_filter);
	refresh_ms_ = refresh_ms;

	if (wake_event_)
		SetEvent(static_cast<HANDLE>(wake_event_));
}

MediaState WmpMonitor::snapshot() const
{
	std::lock_guard lock(mutex_);
	return state_;
}

/* ===================================================================
 *  Worker thread — spawns wmp_bridge.exe each poll cycle
 * =================================================================== */

void WmpMonitor::run()
{
	const std::string bridge_exe = find_bridge_exe();
	if (bridge_exe.empty()) {
		blog(LOG_WARNING,
		     "[obs-wmp-legacy] wmp_bridge.exe not found — "
		     "media state will not be available");
	} else {
		blog(LOG_INFO, "[obs-wmp-legacy] using bridge: %s",
		     bridge_exe.c_str());
	}

	if (is_process_elevated()) {
		blog(LOG_INFO,
		     "[obs-wmp-legacy] OBS is running elevated; "
		     "bridge will be de-elevated for WMP COM access");
	}

	for (;;) {
		std::string app_filter;
		uint32_t refresh_ms = 1000;

		{
			std::lock_guard lock(mutex_);
			if (stop_requested_)
				break;
			app_filter = app_filter_;
			refresh_ms = refresh_ms_;
		}

		MediaState next_state;

		try {
			next_state =
				capture_via_bridge(bridge_exe, app_filter);
		} catch (const std::exception &err) {
			next_state = unavailable_state(err.what());
		} catch (...) {
			next_state = unavailable_state(
				"Unknown error in WMP bridge");
		}

		{
			std::lock_guard lock(mutex_);
			state_ = std::move(next_state);
		}

		if (stop_requested_)
			break;

		HANDLE events[2] = {static_cast<HANDLE>(stop_event_),
				    static_cast<HANDLE>(wake_event_)};
		DWORD wait = WaitForMultipleObjects(2, events, FALSE,
						    refresh_ms);
		if (wait == WAIT_OBJECT_0)
			break;
	}
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
