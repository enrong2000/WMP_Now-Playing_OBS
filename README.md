# OBS WMP Legacy Now-Playing

OBS Studio source plugin that displays now-playing metadata from **Windows Media Player (Legacy)** via COM automation. Includes a premium HTML/CSS overlay for live streams.

> **Note:** This plugin targets Windows Media Player (Legacy) (`wmplayer.exe`) exclusively. It does **not** support the new Media Player app shipped with Windows 11.

## What It Does

- Registers an OBS input source named **Windows Media Player (Legacy) Now Playing**.
- Connects to a running `wmplayer.exe` instance using COM (`IWMPRemoteMediaServices` remote mode).
- Reads title, artist, album, album artist, composer, playback state, position, duration, and the full playlist.
- Renders the **rich glassmorphism Now-Playing overlay directly inside the OBS source** via an embedded private Browser Source. **No separate Browser Source is required.**
- Also writes real-time media state to a JSON file for use by external overlays / consumers.

## How It Works

Windows Media Player (Legacy) does not register itself in the Running Object Table (ROT), so `GetActiveObject` cannot find it. This plugin uses the `IWMPRemoteMediaServices` approach via a **subprocess architecture**:

1. A background worker thread in the plugin spawns `wmp_bridge.exe` — a small standalone helper — once per poll cycle.
2. The bridge process creates an in-process WMP OCX via `CoCreateInstance` in a clean COM environment.
3. It sets a client site implementing `IWMPRemoteMediaServices` with service type `"Remote"`, causing the OCX to attach to the already-running `wmplayer.exe` process.
4. The bridge extracts all media metadata (including the full source playlist via `IWMPPlaylistCollection`) and writes a JSON object to a temp file, then exits.
5. The plugin reads the JSON from the temp file and updates the OBS source.

> **Why a subprocess?** Running WMP COM automation in-process inside OBS is unreliable due to COM apartment conflicts with OBS's threading model and its embedded Chromium (CEF) browser. The subprocess approach guarantees a clean COM environment on every poll.

> **Why temp files instead of pipes?** The plugin uses temp file IPC (`--output <path>`) instead of stdout pipes because `CreateProcessWithTokenW` (used for de-elevation) cannot inherit pipe handles across security boundaries.

### Administrator Compatibility

When OBS runs as Administrator (common for game capture), spawned child processes inherit the elevated token. Since WMP typically runs as a standard user, cross-integrity-level COM connections fail.

The plugin detects elevation and de-elevates the bridge through a **three-tier fallback chain**:

1. **Explorer-shell trick** (preferred): the plugin asks the running `explorer.exe` (which itself runs at medium integrity in the user's session) to launch the bridge via `IShellDispatch2::ShellExecute`. This is the most reliable approach because the resulting child receives both a true medium-IL primary token *and* the user's normal `WindowStation\Desktop` — both of which WMP's COM Remote-mode handshake actually requires. (`CreateProcessWithTokenW` alone leaves the child attached to the elevated parent's locked-down desktop, which silently breaks the WMP attach even though the call returns success.)
2. **`CreateProcessWithTokenW`** with the linked non-elevated token — used as a secondary fallback if the shell trick is unavailable.
3. Plain **`CreateProcessW`** — used when OBS is not elevated.

## Now-Playing Overlay

The rich Now-Playing overlay (glassmorphism design, animated EQ bars, gradient progress bar) is rendered **inside the plugin source itself** via an embedded private OBS Browser Source. You do **not** need to add a second Browser Source.

The overlay can run as a compact minimized window (default) or as a larger maximized player. In maximized mode it shows the previous track and a configurable number of upcoming tracks from the current playlist. In minimized mode it briefly expands the current playlist when the track changes so viewers can see the current position in the queue.

If you prefer text output, choose **Template Text** or **UI Card (Text)** under the source's *Display mode* setting.

The overlay HTML/CSS files are still installed to `<OBS>/data/obs-plugins/obs-wmp-legacy/overlay/` and can be loaded by an external Browser Source if you want to use them outside of OBS.

## Installation

### From Release Zip (Recommended)

1. Download the latest release zip from [Releases](../../releases).
2. Extract the zip. You will get a folder containing:
   ```
   obs-plugins/
     64bit/
       obs-wmp-legacy.dll
       wmp_bridge.exe
   data/
     obs-plugins/
       obs-wmp-legacy/
         locale/
           en-US.ini
           zh-CN.ini
         overlay/
           index.html
           style.css
   install.ps1
   ```
3. Run `install.ps1` in PowerShell:
   ```powershell
   .\install.ps1
   # or specify OBS path directly:
   .\install.ps1 -ObsPath "C:\Program Files\obs-studio"
   ```

The installer will:
- Auto-detect your OBS Studio installation (or prompt for the path)
- Copy `obs-wmp-legacy.dll` and `wmp_bridge.exe` to `<OBS>/obs-plugins/64bit/`
- Copy overlay files to `<OBS>/data/obs-plugins/obs-wmp-legacy/overlay/`
- Create the JSON output directory at `%APPDATA%/obs-wmp-legacy/`

### Manual Installation

1. Copy `obs-wmp-legacy.dll` and `wmp_bridge.exe` to `<OBS>/obs-plugins/64bit/`
2. Copy `data/locale/` to `<OBS>/data/obs-plugins/obs-wmp-legacy/locale/`
3. Copy the `overlay/` folder to `<OBS>/data/obs-plugins/obs-wmp-legacy/overlay/`

## Now-Playing Overlay Setup

After installing the plugin:

1. In OBS, add a source -> **Windows Media Player (Legacy) Now Playing**.
2. That's it — the rich glassmorphism overlay renders inside that single source. Position and resize it on your scene.

(Width / height of the embedded overlay can be adjusted on the source's **Properties** panel.)

If you want to use the overlay outside the plugin (e.g. on a separate Browser Source or another machine), it is still installed to:

```
<OBS>/data/obs-plugins/obs-wmp-legacy/overlay/index.html
```

and the JSON state is written to `%APPDATA%/obs-wmp-legacy/now-playing.json`. Pass `?json=PATH_TO_JSON` in the Browser Source URL or create a `config.json` next to `index.html` with `{"jsonUrl":"http://absolute/.../now-playing.json"}`.

Optional URL/config values for external Browser Sources:

- `mode=minimized|maximized`
- `upcoming=3` or `{"upcomingTracks":3}`
- `reveal_ms=3000` or `{"compactRevealMs":3000}`

## Build

Requirements:

- Windows 10 or later
- Visual Studio 2022 with the Desktop C++ workload
- OBS Studio development package or an OBS build tree that exports `libobsConfig.cmake`

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -Dlibobs_DIR="C:\path\to\obs\cmake\libobs"
cmake --build build --config RelWithDebInfo
cmake --install build --config RelWithDebInfo --prefix "C:\Program Files\obs-studio"
```

If your OBS package exposes a different CMake path, point `libobs_DIR` at the directory containing `libobsConfig.cmake`.

## GitHub Actions Release

The repository includes `.github/workflows/windows-build-release.yml`.

- Pushes and pull requests build a Windows x64 artifact.
- Pushing a version tag such as `v0.3.0` or `0.3.0` creates or updates a GitHub Release and uploads the plugin zip.
- Manual runs support an `obs_version` input. The default is OBS Studio `32.1.2`.
- The release artifact includes the overlay files, locale files, bridge helper, and installer script.

## Source Settings

| Setting | Description |
|---------|-------------|
| **App filter** | Substring to identify the WMP process. Default: `wmplayer`. |
| **Display mode** | `Embedded Overlay` (rich Browser-Source overlay rendered inside the plugin source; default), `Template Text`, or `UI Card (Text)`. |
| **Overlay work mode** | `Minimized Window` (default compact layout) or `Maximized Window` (larger player with previous/upcoming tracks). |
| **Embedded overlay width/height** | Pixel dimensions of the embedded overlay (default 520x520 for the compact minimized window and its reveal animation; maximized mode automatically uses at least 570px wide and enough height for the selected upcoming count). |
| **Upcoming tracks** | Number of tracks after the current one to show in the maximized playlist window. Default: 3. |
| **Compact playlist reveal** | How long the minimized window expands the playlist after a track change. Default: 3000 ms. |
| **Format** | Output template (used in Template Text mode). |
| **Progress width** | Character width of `{progress_bar}`. |
| **Show composer** | Display composer information in UI Card (Text) mode when available. |
| **Show full playlist** | Show all tracks from the source playlist (via `IWMPPlaylistCollection`). When off, only the current playback queue is shown. Default: on. |
| **Refresh interval** | COM polling interval in milliseconds. Default: 500 ms. |
| **Hide when no media** | Render nothing when WMP is not running or has no media loaded. |
| **JSON output path** | File path for the JSON data file consumed by the embedded overlay (and any external overlay). Default: `%APPDATA%/obs-wmp-legacy/now-playing.json`. |

### Format Tokens

| Token | Description |
|-------|-------------|
| `{title}` | Track title |
| `{artist}` | Artist name |
| `{album}` | Album title |
| `{album_artist}` | Album artist |
| `{composer}` | Composer |
| `{backend}` | Backend identifier (`WMP Legacy COM`) |
| `{source_app_id}` | Source application ID (`wmplayer.exe`) |
| `{status}` | Playback status (Playing, Paused, Stopped, etc.) |
| `{position}` | Current position formatted as `m:ss` or `h:mm:ss` |
| `{duration}` | Total duration formatted as `m:ss` or `h:mm:ss` |
| `{remaining}` | Remaining time |
| `{progress_percent}` | Progress as percentage (e.g. `42%`) |
| `{progress_bar}` | ASCII progress bar (e.g. `[####--------]`) |
| `{playlist_count}` | Number of tracks in current playlist |
| `{playlist}` | Full playlist text |
| `{diagnostic}` | Error/diagnostic message (if any) |

### Display Modes

- **Embedded Overlay** (default): rich glassmorphism Now-Playing panel rendered inside the source via an embedded private OBS Browser Source. Supports minimized and maximized window layouts with no second source required.
- **UI Card (Text)**: structured now-playing text panel with icons, composer, and playlist display.
- **Template Text**: customizable token template mode for full control over output format.
