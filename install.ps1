<#
.SYNOPSIS
    OBS Windows Media Player (Legacy) Now-Playing Plugin Installer

.DESCRIPTION
    Installs the obs-wmp-legacy plugin and its Now-Playing overlay into the
    specified OBS Studio installation directory.

.PARAMETER ObsPath
    Path to the OBS Studio installation root (e.g. "C:\Program Files\obs-studio").
    If not provided, the script will attempt auto-detection or prompt the user.

.EXAMPLE
    .\install.ps1
    .\install.ps1 -ObsPath "D:\OBS Studio"
#>

param(
    [string]$ObsPath
)

$ErrorActionPreference = 'Stop'

# ---- Output helpers ----
function Write-Step  { param([string]$Msg) Write-Host "  [*] $Msg" -ForegroundColor Cyan }
function Write-Ok    { param([string]$Msg) Write-Host "  [OK] $Msg" -ForegroundColor Green }
function Write-Warn  { param([string]$Msg) Write-Host "  [!!] $Msg" -ForegroundColor Yellow }
function Write-Fail  { param([string]$Msg) Write-Host "  [FAIL] $Msg" -ForegroundColor Red }

# ---- Banner ----
Write-Host ""
Write-Host "  +------------------------------------------------+" -ForegroundColor Magenta
Write-Host "  |  OBS WMP (Legacy) -- Now-Playing Plugin Setup   |" -ForegroundColor Magenta
Write-Host "  +------------------------------------------------+" -ForegroundColor Magenta
Write-Host ""

# ---- Locate OBS ----
function Find-ObsInstallation {
    $candidates = @(
        "$env:ProgramFiles\obs-studio",
        "${env:ProgramFiles(x86)}\obs-studio",
        "$env:LOCALAPPDATA\Programs\obs-studio",
        "C:\Program Files\obs-studio",
        "D:\Program Files\obs-studio"
    )
    foreach ($path in $candidates) {
        if (Test-Path (Join-Path $path "bin\64bit\obs64.exe")) { return $path }
        if (Test-Path (Join-Path $path "bin\obs64.exe"))       { return $path }
    }
    return $null
}

if (-not $ObsPath) {
    Write-Step "Auto-detecting OBS Studio installation..."
    $ObsPath = Find-ObsInstallation
    if ($ObsPath) {
        Write-Ok "Found OBS at: $ObsPath"
        $confirm = Read-Host "  Use this path? [Y/n]"
        if ($confirm -eq 'n' -or $confirm -eq 'N') {
            $ObsPath = $null
        }
    }
}

if (-not $ObsPath) {
    $ObsPath = Read-Host "  Enter OBS Studio installation path"
}

$ObsPath = $ObsPath.TrimEnd('\', '/')

if (-not (Test-Path $ObsPath)) {
    Write-Fail "OBS path not found: $ObsPath"
    exit 1
}

# ---- Determine plugin and data paths ----
$pluginDir = Join-Path $ObsPath "obs-plugins\64bit"
$dataDir   = Join-Path $ObsPath "data\obs-plugins\obs-wmp-legacy"

# Also support the newer obs-studio directory structure
if (-not (Test-Path (Split-Path $pluginDir))) {
    $pluginDir = Join-Path $ObsPath "bin\64bit"
}

Write-Step "Plugin directory: $pluginDir"
Write-Step "Data directory:   $dataDir"

# ---- Create directories ----
if (-not (Test-Path $pluginDir)) { New-Item -ItemType Directory -Path $pluginDir -Force | Out-Null }
if (-not (Test-Path $dataDir))   { New-Item -ItemType Directory -Path $dataDir   -Force | Out-Null }

$overlayDir = Join-Path $dataDir "overlay"
if (-not (Test-Path $overlayDir)) { New-Item -ItemType Directory -Path $overlayDir -Force | Out-Null }

# ---- Copy files ----
$scriptRoot = $PSScriptRoot

# Plugin DLL (if built)
$dllSrc = Join-Path $scriptRoot "build\RelWithDebInfo\obs-wmp-legacy.dll"
if (-not (Test-Path $dllSrc)) { $dllSrc = Join-Path $scriptRoot "build\Release\obs-wmp-legacy.dll" }
if (-not (Test-Path $dllSrc)) { $dllSrc = Join-Path $scriptRoot "build\Debug\obs-wmp-legacy.dll" }

# Also check if the DLL is placed alongside the script (release zip layout)
if (-not (Test-Path $dllSrc)) { $dllSrc = Join-Path $scriptRoot "obs-plugins\64bit\obs-wmp-legacy.dll" }

if (Test-Path $dllSrc) {
    Write-Step "Copying plugin DLL..."
    Copy-Item $dllSrc -Destination $pluginDir -Force
    Write-Ok "obs-wmp-legacy.dll installed"
} else {
    Write-Warn "Plugin DLL not found (not built yet?). Skipping DLL copy."
    Write-Warn "Build the plugin first, then re-run this script."
}

# Bridge helper exe (wmp_bridge.exe)
$bridgeSrc = Join-Path $scriptRoot "build\RelWithDebInfo\wmp_bridge.exe"
if (-not (Test-Path $bridgeSrc)) { $bridgeSrc = Join-Path $scriptRoot "build\Release\wmp_bridge.exe" }
if (-not (Test-Path $bridgeSrc)) { $bridgeSrc = Join-Path $scriptRoot "build\Debug\wmp_bridge.exe" }
if (-not (Test-Path $bridgeSrc)) { $bridgeSrc = Join-Path $scriptRoot "obs-plugins\64bit\wmp_bridge.exe" }

if (Test-Path $bridgeSrc) {
    Write-Step "Copying bridge helper..."
    Copy-Item $bridgeSrc -Destination $pluginDir -Force
    Write-Ok "wmp_bridge.exe installed"
} else {
    Write-Warn "wmp_bridge.exe not found. Plugin will not function without it."
}

# Overlay files
$overlaySrc = Join-Path $scriptRoot "overlay"
# Also check release zip layout
if (-not (Test-Path $overlaySrc)) {
    $overlaySrc = Join-Path $scriptRoot "data\obs-plugins\obs-wmp-legacy\overlay"
}

if (Test-Path $overlaySrc) {
    Write-Step "Copying overlay files..."
    Copy-Item (Join-Path $overlaySrc "index.html") -Destination $overlayDir -Force
    Copy-Item (Join-Path $overlaySrc "style.css")  -Destination $overlayDir -Force
    Write-Ok "Overlay files installed to: $overlayDir"
} else {
    Write-Warn "Overlay directory not found. Skipping overlay copy."
}

# ---- Create APPDATA directory for JSON output ----
$jsonDir = Join-Path $env:APPDATA "obs-wmp-legacy"
if (-not (Test-Path $jsonDir)) {
    New-Item -ItemType Directory -Path $jsonDir -Force | Out-Null
}
Write-Ok "JSON output directory: $jsonDir"

# ---- Create overlay config for OBS Browser Source ----
function ConvertTo-ObsAbsoluteUrl {
    param([Parameter(Mandatory = $true)][string]$Path)

    $normalized = $Path.Replace('\', '/')
    $segments = $normalized -split '/'
    $encoded = for ($i = 0; $i -lt $segments.Count; $i++) {
        if ($i -eq 0 -and $segments[$i] -match '^[A-Za-z]:$') {
            $segments[$i]
        } else {
            [System.Uri]::EscapeDataString($segments[$i])
        }
    }

    return "http://absolute/$($encoded -join '/')"
}

$jsonPath = Join-Path $jsonDir "now-playing.json"
$overlayConfigPath = Join-Path $overlayDir "config.json"
$overlayConfig = [ordered]@{
    jsonPath = $jsonPath
    jsonUrl = ConvertTo-ObsAbsoluteUrl $jsonPath
} | ConvertTo-Json

Set-Content -Path $overlayConfigPath -Value $overlayConfig -Encoding UTF8
Write-Ok "Overlay config written: $overlayConfigPath"

# ---- Summary ----
Write-Host ""
Write-Host "  +------------------------------------------------+" -ForegroundColor Green
Write-Host "  |           Installation Complete!                |" -ForegroundColor Green
Write-Host "  +------------------------------------------------+" -ForegroundColor Green
Write-Host ""
Write-Host "  Next steps:" -ForegroundColor White
Write-Host "  1. Open OBS Studio" -ForegroundColor Gray
Write-Host "  2. Add source -> 'Windows Media Player (Legacy) Now Playing'" -ForegroundColor Gray
Write-Host "     The rich Now-Playing overlay is embedded directly in the source." -ForegroundColor DarkGray
Write-Host "     (No separate Browser Source needed.)" -ForegroundColor DarkGray
Write-Host ""
Write-Host "  Embedded overlay size can be adjusted in the source's Properties." -ForegroundColor Gray
Write-Host ""
Write-Host "  External overlay HTML (optional, for non-OBS use):" -ForegroundColor Gray
Write-Host "     $overlayDir\index.html" -ForegroundColor Yellow
Write-Host ""
Write-Host "  JSON data file:" -ForegroundColor Gray
Write-Host "     $jsonPath" -ForegroundColor Yellow
Write-Host ""
