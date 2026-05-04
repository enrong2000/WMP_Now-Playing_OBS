# OBS WMP Legacy Now-Playing

OBS Studio source plugin that displays now-playing metadata from **Windows Media Player (Legacy)** via COM automation. Includes a premium HTML/CSS overlay for live streams.

> **Note:** This plugin targets Windows Media Player (Legacy) (`wmplayer.exe`) exclusively. It does **not** support the new Media Player app shipped with Windows 11.

## What It Does

- Registers an OBS input source named **Windows Media Player (Legacy) Now Playing**.
- Connects to a running `wmplayer.exe` instance using COM (`IWMPRemoteMediaServices` remote mode).
- Reads title, artist, album, album artist, composer, playback state, position, duration, and the full playlist.
- Renders a text display using OBS' bundled Windows text source.
- **Writes real-time media state to a JSON file**, enabling a beautiful HTML/CSS overlay loaded via OBS Browser Source.
- Includes a **Now-Playing overlay** with glassmorphism design, animated vinyl disc, EQ visualizer bars, and a glowing gradient progress bar.

## How It Works

Windows Media Player (Legacy) does not register itself in the Running Object Table (ROT), so `GetActiveObject` cannot find it. Instead, this plugin uses the `IWMPRemoteMediaServices` approach:

1. Creates an in-process WMP OCX instance via `CoCreateInstance`.
2. Sets a client site implementing `IWMPRemoteMediaServices` with service type `"Remote"`.
3. The OCX attaches to the already-running `wmplayer.exe` process and shares its playback state.
4. A background worker thread polls the COM interface at a configurable interval and exposes the state to OBS.

## Installation

### From Release Zip (Recommended)

1. Download the latest release zip from [Releases](../../releases).
2. Extract the zip. You will get a folder containing:
   ```
   obs-plugins/
     64bit/
       obs-wmp-legacy.dll
   data/
     obs-plugins/
       obs-wmp-legacy/
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
- Copy `obs-wmp-legacy.dll` to `<OBS>/obs-plugins/64bit/`
- Copy overlay files to `<OBS>/data/obs-plugins/obs-wmp-legacy/overlay/`
- Create the JSON output directory at `%APPDATA%/obs-wmp-legacy/`

### Manual Installation

1. Copy `obs-wmp-legacy.dll` to `<OBS>/obs-plugins/64bit/`
2. Copy the `overlay/` folder to `<OBS>/data/obs-plugins/obs-wmp-legacy/overlay/`

## Now-Playing Overlay Setup

After installing the plugin:

1. In OBS, add a source -> **Windows Media Player (Legacy) Now Playing**
   (this activates the plugin and starts writing JSON data).
2. Add another source -> **Browser**:
   - Check **Local file**
   - Path: `<OBS>/data/obs-plugins/obs-wmp-legacy/overlay/index.html`
   - Width: `520`, Height: `260`
   - Custom CSS: *(leave empty)*
3. Position the overlay wherever you like on your scene.

The overlay reads from `%APPDATA%/obs-wmp-legacy/now-playing.json`, which is updated by the plugin in real time.
The installer writes `overlay/config.json` so OBS Browser Source can read that JSON file through OBS' `http://absolute/...` local-file origin.
For manual overlay installs, pass `?json=PATH_TO_JSON` in the Browser Source URL or create the same `config.json` next to `index.html`.

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
- The release artifact includes the overlay files and installer script.

## Source Settings

| Setting | Description |
|---------|-------------|
| **App filter** | Substring to identify the WMP process. Default: `wmplayer`. |
| **Display mode** | `UI Card` (structured panel) or `Template Text` (custom format string). |
| **Format** | Output template (used in Template Text mode). |
| **Progress width** | Character width of `{progress_bar}`. |
| **Show composer** | Display composer information in UI Card mode when available. |
| **Refresh interval** | COM polling interval in milliseconds. |
| **Hide when no media** | Render nothing when WMP is not running or has no media loaded. |
| **JSON output path** | File path for the JSON data file consumed by the overlay. Default: `%APPDATA%/obs-wmp-legacy/now-playing.json`. |

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

- **UI Card** (default): Structured now-playing panel with icons, composer, and playlist display.
- **Template Text**: Customizable token template mode for full control over output format.
