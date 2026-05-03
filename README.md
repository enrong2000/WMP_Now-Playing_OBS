# OBS WMP SMTC

Native OBS source plugin for showing media metadata from Windows Media Player (Legacy) through the Windows SMTC API, with a beautiful Now-Playing overlay for live streams.

## What it does

- Registers an OBS input source named `WMP Legacy Now Playing (SMTC)`.
- Reads the active Windows media session with `Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager`.
- Displays title, artist, album, playback state, position, duration, and a text progress bar.
- Uses OBS' bundled Windows text source internally, so the plugin does not implement font rendering itself.
- **Writes real-time media state to a JSON file**, enabling a beautiful HTML/CSS overlay loaded via OBS Browser Source.
- Includes a **premium Now-Playing overlay** with glassmorphism design, animated vinyl disc, EQ visualizer bars, and a glowing gradient progress bar.

## WMP Legacy Integration

SMTC exposes media session metadata and timeline data only for applications that publish an SMTC session. Windows Media Player (Legacy) may not publish one; in that case the plugin uses a multi-layer fallback strategy:

1. **SMTC API** — preferred; reads metadata and timeline from the system media session.
2. **WMP COM (ROT)** — connects to a running `wmplayer.exe` instance via `GetActiveObject` and reads `IWMPPlayer4` data (title, artist, album, composer, position, duration).
3. **WMP COM (Embedded)** — creates an embedded WMP COM instance via `CreateInstance` as a secondary fallback.
4. **Window title detection** — finds visible `wmplayer.exe` windows and extracts the media title from the window title bar.

## Installation

### Automated (recommended)

1. Download the latest release zip from [Releases](../../releases).
2. Extract the zip.
3. Run `install.ps1` in PowerShell:

```powershell
.\install.ps1
# or specify OBS path directly:
.\install.ps1 -ObsPath "C:\Program Files\obs-studio"
```

The installer will:
- Copy the plugin DLL to `obs-plugins/64bit/`
- Copy overlay files to `data/obs-plugins/obs-wmp-smtc/overlay/`
- Create the JSON output directory at `%APPDATA%/obs-wmp-smtc/`

### Manual

1. Copy `obs-wmp-smtc.dll` to `<OBS>/obs-plugins/64bit/`
2. Copy the `overlay/` folder to `<OBS>/data/obs-plugins/obs-wmp-smtc/overlay/`

## Now-Playing Overlay Setup

After installing the plugin:

1. In OBS, add a source → **WMP Legacy Now Playing (SMTC)** (this activates the plugin and starts writing JSON data).
2. Add another source → **Browser**:
   - Check **Local file**
   - Path: `<OBS>/data/obs-plugins/obs-wmp-smtc/overlay/index.html`
   - Width: `480`, Height: `200`
   - Custom CSS: *(leave empty)*
3. Position the overlay wherever you like on your scene.

The overlay reads from `%APPDATA%/obs-wmp-smtc/now-playing.json`, which is updated by the plugin in real time.

### Overlay Customization

You can pass query parameters to the overlay URL to customize behavior:

- `?json=<path>` — custom JSON file path
- `?appdata=<path>` — custom AppData path

## Build

Requirements:

- Windows 10 1809 or later.
- Visual Studio 2022 with the Desktop C++ workload.
- OBS Studio development package or an OBS build tree that exports `libobsConfig.cmake`.

Example:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -Dlibobs_DIR="C:\path\to\obs\cmake\libobs"
cmake --build build --config RelWithDebInfo
cmake --install build --config RelWithDebInfo --prefix "C:\Program Files\obs-studio"
```

If your OBS package exposes a different CMake path, point `libobs_DIR` at the directory containing `libobsConfig.cmake`.

## GitHub Actions Release

The repository includes `.github/workflows/windows-build-release.yml`.

- Pushes and pull requests build a Windows x64 artifact.
- Pushing a version tag such as `v0.1.0` or `0.1.0` creates or updates a GitHub Release and uploads the plugin zip.
- Manual runs support an `obs_version` input. The default is OBS Studio `32.1.2`.
- The release artifact includes the overlay files and installer script.

## Source Settings

- `App filter`: substring used to select the SMTC session. The default `wmplayer` targets Windows Media Player when its SMTC app id includes that text. Clear it to use the current system media session.
- `Display mode`: choose between `UI Card` (structured panel) or `Template Text` (custom format string).
- `Format`: output template (used in Template Text mode).
- `Progress width`: character width of `{progress_bar}`.
- `Show composer in UI Card mode`: display composer information when available.
- `Refresh interval`: SMTC polling interval in milliseconds.
- `Enable WMP Legacy fallbacks (COM + window title)`: when no matching SMTC session exists, try WMP COM automation first, then fall back to detecting a visible `wmplayer.exe` window title.
- `Hide when no media`: render nothing when no matching session is available.
- `JSON output path`: file path for the JSON data file consumed by the overlay (default: `%APPDATA%/obs-wmp-smtc/now-playing.json`).

Available format tokens:

- `{title}`
- `{artist}`
- `{album}`
- `{album_artist}`
- `{composer}`
- `{subtitle}`
- `{genres}`
- `{backend}`
- `{source_app_id}`
- `{status}`
- `{position}`
- `{duration}`
- `{remaining}`
- `{progress_percent}`
- `{progress_bar}`
- `{sessions}`
- `{diagnostic}`
- `{wmp_windows}`


Additional metadata in legacy COM mode:
- Composer (`WM/Composer`)
- Album (`WM/AlbumTitle`)
- Playback position and duration

Display modes:
- UI Card (default): card-style structured now-playing panel
- Template Text: legacy token template mode
