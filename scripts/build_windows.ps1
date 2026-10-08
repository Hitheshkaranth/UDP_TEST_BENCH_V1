<#
.SYNOPSIS
    Builds the UDP Bandwidth Tester on Windows and (optionally) packages it into a ready-to-run zip.

.DESCRIPTION
    1. Finds Visual Studio 2022 (or its Build Tools) with the C++ workload.
    2. Configures and builds with CMake (Ninja if available, otherwise NMake) in Release mode.
    3. Runs windeployqt so the exes run without a Qt installation.
    4. With -Package: copies the Visual C++ runtime, docs and firewall script, and creates a zip.

.PARAMETER QtDir
    Qt kit folder for MSVC 2022 x64, e.g. C:\Qt\6.8.3\msvc2022_64.

.PARAMETER BuildDir
    Build folder (default: <repo>\build).

.PARAMETER Package
    Also create <repo>\dist\UdpBandwidthTester_win64.zip.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build_windows.ps1 -QtDir C:\Qt\6.8.3\msvc2022_64 -Package
#>
param(
    [string]$QtDir = "C:\Qt\6.8.3\msvc2022_64",
    [string]$BuildDir = "",
    [switch]$Package
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repo "build" }

# Prints a step heading.
function Step([string]$text) { Write-Host "`n==> $text" -ForegroundColor Cyan }

# --- 1. Check prerequisites ------------------------------------------------------------
Step "Checking prerequisites"
if (-not (Test-Path (Join-Path $QtDir "bin\qmake.exe"))) {
    throw "Qt not found in '$QtDir'. Install Qt for MSVC 2022 64-bit and pass -QtDir <folder>."
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "Visual Studio Installer not found. Install Visual Studio 2022 or its Build Tools." }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio with the 'Desktop development with C++' workload found." }
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
$env:PATH = "$(Split-Path $vswhere);$env:PATH"   # vcvars64.bat calls vswhere itself
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw "CMake not found on PATH. Install CMake 3.16 or newer." }
Write-Host "Qt:            $QtDir"
Write-Host "Visual Studio: $vsPath"
Write-Host "Build folder:  $BuildDir"

# --- 2. Configure and build ------------------------------------------------------------
Step "Configuring and building (Release)"
$generator = if (Get-Command ninja -ErrorAction SilentlyContinue) { "Ninja" } else { "NMake Makefiles" }
$cmd = "call `"$vcvars`" >nul && " +
       "cmake -S `"$repo`" -B `"$BuildDir`" -G `"$generator`" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=`"$QtDir`" && " +
       "cmake --build `"$BuildDir`""
cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit code $LASTEXITCODE)." }

# --- 3. Deploy the Qt libraries next to the exes --------------------------------------
Step "Copying Qt libraries next to the executables (windeployqt)"
$gui = Join-Path $BuildDir "UdpBandwidthTester.exe"
$cli = Join-Path $BuildDir "udpbw-cli.exe"
& (Join-Path $QtDir "bin\windeployqt.exe") --release --no-translations --no-system-d3d-compiler --no-opengl-sw $gui $cli | Out-Null
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed." }
Write-Host "Built: $gui"
Write-Host "Built: $cli"

if (-not $Package) { Step "Done (run with -Package to create the zip)"; exit 0 }

# --- 4. Package -----------------------------------------------------------------------
Step "Creating the distributable package"
$dist = Join-Path $repo "dist\UdpBandwidthTester"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item $gui, $cli $dist
& (Join-Path $QtDir "bin\windeployqt.exe") --release --no-translations --no-system-d3d-compiler --no-opengl-sw `
    (Join-Path $dist "UdpBandwidthTester.exe") (Join-Path $dist "udpbw-cli.exe") | Out-Null

# Visual C++ runtime DLLs (so the target PC needs nothing installed): newest VC\Redist\MSVC\<version>\x64\Microsoft.VC*.CRT
$crt = Get-ChildItem (Join-Path $vsPath "VC\Redist\MSVC") -Directory -ErrorAction SilentlyContinue |
       Sort-Object Name -Descending |
       ForEach-Object { Get-ChildItem (Join-Path $_.FullName "x64") -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue } |
       Select-Object -First 1
if ($crt) { Copy-Item (Join-Path $crt.FullName "*.dll") $dist } else { Write-Warning "VC++ runtime not found; target PCs need the VC++ 2015-2022 redistributable." }

Copy-Item (Join-Path $repo "README.md") $dist
Copy-Item (Join-Path $repo "docs") $dist -Recurse
Copy-Item (Join-Path $repo "scripts\allow_firewall_port_5201.bat") $dist

$zip = Join-Path $repo "dist\UdpBandwidthTester_win64.zip"
Compress-Archive -Path $dist -DestinationPath $zip -Force
Step ("Package ready: {0} ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
