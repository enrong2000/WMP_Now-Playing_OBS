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

# ---- Summary ----
Write-Host ""
Write-Host "  +------------------------------------------------+" -ForegroundColor Green
Write-Host "  |           Installation Complete!                |" -ForegroundColor Green
Write-Host "  +------------------------------------------------+" -ForegroundColor Green
Write-Host ""
Write-Host "  Next steps:" -ForegroundColor White
Write-Host "  1. Open OBS Studio" -ForegroundColor Gray
Write-Host "  2. Add source -> 'Windows Media Player (Legacy) Now Playing'" -ForegroundColor Gray
Write-Host "     (This activates the plugin and starts writing JSON data)" -ForegroundColor DarkGray
Write-Host ""
Write-Host "  3. For the Now-Playing overlay:" -ForegroundColor Gray
Write-Host "     Add source -> 'Browser' -> check 'Local file'" -ForegroundColor Gray
Write-Host "     Path: $overlayDir\index.html" -ForegroundColor Yellow
Write-Host "     Width: 480   Height: 200" -ForegroundColor DarkGray
Write-Host ""
Write-Host "  JSON data file:" -ForegroundColor Gray
Write-Host "     $jsonDir\now-playing.json" -ForegroundColor Yellow
Write-Host ""
