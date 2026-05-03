# OBS WMP SMTC

Native OBS source plugin prototype for showing media metadata from Windows Media Player (Legacy) through the Windows SMTC API.

## What it does

- Registers an OBS input source named `WMP Legacy Now Playing (SMTC)`.
- Reads the active Windows media session with `Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager`.
- Displays title, artist, album, playback state, position, duration, and a text progress bar.
- Uses OBS' bundled Windows text source internally, so the plugin does not implement font rendering itself.

## Current SMTC limitation

SMTC exposes media session metadata and timeline data. It does not expose the real playlist or queue from Windows Media Player (Legacy). This prototype shows active SMTC sessions through the `{sessions}` token, but a real WMP playlist requires an additional integration layer such as a WMP-specific COM/extension path or UI Automation.

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

The workflow checks out the matching OBS Studio tag and builds `libobs` first, then configures this plugin with the generated `libobsConfig.cmake` and the OBS build dependency paths such as `w32-pthreads`. This avoids relying on an external OBS SDK archive.

## Source Settings

- `App filter`: substring used to select the SMTC session. The default `wmplayer` targets Windows Media Player when its SMTC app id includes that text. Clear it to use the current system media session.
- `Format`: output template.
- `Progress width`: character width of `{progress_bar}`.
- `Refresh interval`: SMTC polling interval in milliseconds.
- `Hide when no media`: render nothing when no matching session is available.

Available format tokens:

- `{title}`
- `{artist}`
- `{album}`
- `{album_artist}`
- `{subtitle}`
- `{genres}`
- `{source_app_id}`
- `{status}`
- `{position}`
- `{duration}`
- `{remaining}`
- `{progress_percent}`
- `{progress_bar}`
- `{sessions}`
