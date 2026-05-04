# OBS WMP Legacy 正在播放

OBS Studio 源插件，通过 COM 自动化显示 **Windows Media Player (Legacy)** 的正在播放元数据。包含适用于直播的精美 HTML/CSS 叠加层。

> **注意：** 本插件仅支持 Windows Media Player (Legacy)（`wmplayer.exe`），**不支持** Windows 11 自带的新版"媒体播放器"应用。

## 功能概述

- 注册名为 **Windows Media Player (Legacy) Now Playing** 的 OBS 输入源。
- 通过 COM 远程模式（`IWMPRemoteMediaServices`）连接到正在运行的 `wmplayer.exe` 实例。
- 读取曲目标题、艺术家、专辑、专辑艺术家、作曲家、播放状态、播放位置、时长及完整播放列表。
- 使用 OBS 内置的 Windows 文字源渲染文本显示。
- **实时写入媒体状态到 JSON 文件**，支持通过 OBS 浏览器源加载的精美 HTML/CSS 叠加层。
- 包含 **正在播放叠加层**，采用毛玻璃风格设计，配有动态 EQ 可视化条和发光渐变进度条。

## 工作原理

Windows Media Player (Legacy) 不会将自身注册到运行对象表（ROT），因此 `GetActiveObject` 无法找到它。本插件采用 `IWMPRemoteMediaServices` 方式，通过**子进程架构**实现：

1. 插件中的后台工作线程在每个轮询周期启动 `wmp_bridge.exe`——一个独立的小型辅助程序。
2. 桥接进程在干净的 COM 环境中通过 `CoCreateInstance` 创建进程内 WMP OCX 实例。
3. 它设置一个实现了 `IWMPRemoteMediaServices` 的客户端站点，服务类型为 `"Remote"`，使 OCX 附加到已运行的 `wmplayer.exe` 进程。
4. 桥接程序提取所有媒体元数据（包括通过 `IWMPPlaylistCollection` 获取的完整源播放列表），将 JSON 对象写入临时文件，然后退出。
5. 插件从临时文件读取 JSON 数据并更新 OBS 源。

> **为什么使用子进程？** 在 OBS 进程内直接运行 WMP COM 自动化是不可靠的，因为 OBS 的线程模型及其内嵌的 Chromium（CEF）浏览器会导致 COM 单元冲突。子进程方式保证每次轮询都在干净的 COM 环境中运行。

> **为什么使用临时文件而非管道？** 插件使用临时文件 IPC（`--output <路径>`）而非标准输出管道，因为 `CreateProcessWithTokenW`（用于降权）无法跨安全边界继承管道句柄。

### 管理员权限兼容

当 OBS 以管理员身份运行时（游戏直播捕获窗口常需如此），子进程会自动继承提升的令牌。由于 WMP 通常以普通用户权限运行，跨完整性级别的 COM 连接会失败。插件通过检测提升状态并使用 `CreateProcessWithTokenW` 以降权后的中等完整性令牌启动桥接进程来解决此问题，确保桥接程序能正确连接到用户的 WMP 实例。

## 安装

### 从发布压缩包安装（推荐）

1. 从 [Releases](../../releases) 页面下载最新的发布压缩包。
2. 解压后将得到以下文件结构：
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
3. 在 PowerShell 中运行 `install.ps1`：
   ```powershell
   .\install.ps1
   # 或直接指定 OBS 路径：
   .\install.ps1 -ObsPath "C:\Program Files\obs-studio"
   ```

安装程序将：
- 自动检测 OBS Studio 安装位置（或提示输入路径）
- 复制 `obs-wmp-legacy.dll` 和 `wmp_bridge.exe` 到 `<OBS>/obs-plugins/64bit/`
- 复制叠加层文件到 `<OBS>/data/obs-plugins/obs-wmp-legacy/overlay/`
- 在 `%APPDATA%/obs-wmp-legacy/` 创建 JSON 输出目录

### 手动安装

1. 复制 `obs-wmp-legacy.dll` 和 `wmp_bridge.exe` 到 `<OBS>/obs-plugins/64bit/`
2. 复制 `data/locale/` 到 `<OBS>/data/obs-plugins/obs-wmp-legacy/locale/`
3. 复制 `overlay/` 文件夹到 `<OBS>/data/obs-plugins/obs-wmp-legacy/overlay/`

## 正在播放叠加层设置

安装插件后：

1. 在 OBS 中添加源 → **Windows Media Player (Legacy) Now Playing**
  （此操作激活插件并开始写入 JSON 数据）。
2. 再添加一个源 → **浏览器**：
   - 勾选 **本地文件**
   - 路径：`<OBS>/data/obs-plugins/obs-wmp-legacy/overlay/index.html`
   - 宽度：`520`，高度：`260`
   - 自定义 CSS：*（留空）*
3. 将叠加层放置在场景中您喜欢的位置。

叠加层从 `%APPDATA%/obs-wmp-legacy/now-playing.json` 读取数据，该文件由插件实时更新。
安装程序会写入 `overlay/config.json`，以便 OBS 浏览器源可以通过 OBS 的 `http://absolute/...` 本地文件源协议读取该 JSON 文件。
如果手动安装叠加层，请在浏览器源 URL 中传入 `?json=JSON文件路径`，或在 `index.html` 旁创建相同的 `config.json`。

## 构建

要求：

- Windows 10 或更高版本
- Visual Studio 2022（含桌面 C++ 工作负载）
- OBS Studio 开发包或导出 `libobsConfig.cmake` 的 OBS 构建树

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -Dlibobs_DIR="C:\path\to\obs\cmake\libobs"
cmake --build build --config RelWithDebInfo
cmake --install build --config RelWithDebInfo --prefix "C:\Program Files\obs-studio"
```

如果您的 OBS 包使用不同的 CMake 路径，请将 `libobs_DIR` 指向包含 `libobsConfig.cmake` 的目录。

## GitHub Actions 发布

仓库包含 `.github/workflows/windows-build-release.yml`。

- 推送和拉取请求会构建 Windows x64 构件。
- 推送如 `v0.3.0` 或 `0.3.0` 的版本标签时，会创建或更新 GitHub Release 并上传插件压缩包。
- 手动运行支持 `obs_version` 输入参数。默认使用 OBS Studio `32.1.2`。
- 发布构件包含叠加层文件、语言文件、桥接辅助程序和安装脚本。

## 源设置

| 设置 | 说明 |
|------|------|
| **App filter** | 用于识别 WMP 进程的子字符串。默认：`wmplayer`。 |
| **Display mode** | `UI Card`（结构化面板）或 `Template Text`（自定义格式字符串）。 |
| **Format** | 输出模板（在 Template Text 模式下使用）。 |
| **Progress width** | `{progress_bar}` 的字符宽度。 |
| **Show composer** | 在 UI Card 模式下显示作曲家信息（如有）。 |
| **Show full playlist** | 显示源播放列表的所有曲目（通过 `IWMPPlaylistCollection`）。关闭时仅显示当前播放队列。默认：开启。 |
| **Refresh interval** | COM 轮询间隔（毫秒）。 |
| **Hide when no media** | 当 WMP 未运行或未加载媒体时不显示任何内容。 |
| **JSON output path** | 叠加层使用的 JSON 数据文件路径。默认：`%APPDATA%/obs-wmp-legacy/now-playing.json`。 |

### 格式令牌

| 令牌 | 说明 |
|------|------|
| `{title}` | 曲目标题 |
| `{artist}` | 艺术家名称 |
| `{album}` | 专辑标题 |
| `{album_artist}` | 专辑艺术家 |
| `{composer}` | 作曲家 |
| `{backend}` | 后端标识符（`WMP Legacy COM`） |
| `{source_app_id}` | 源应用程序 ID（`wmplayer.exe`） |
| `{status}` | 播放状态（Playing、Paused、Stopped 等） |
| `{position}` | 当前位置，格式为 `m:ss` 或 `h:mm:ss` |
| `{duration}` | 总时长，格式为 `m:ss` 或 `h:mm:ss` |
| `{remaining}` | 剩余时间 |
| `{progress_percent}` | 进度百分比（如 `42%`） |
| `{progress_bar}` | ASCII 进度条（如 `[####--------]`） |
| `{playlist_count}` | 当前播放列表中的曲目数 |
| `{playlist}` | 完整播放列表文本 |
| `{diagnostic}` | 错误/诊断信息（如有） |

### 显示模式

- **UI Card**（默认）：带图标、作曲家信息和播放列表显示的结构化正在播放面板。
- **Template Text**：可自定义的令牌模板模式，完全控制输出格式。
