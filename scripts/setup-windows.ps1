<#
.SYNOPSIS
  Prepares a Windows 11 machine to build ANVIL (one-time, needs network).

.DESCRIPTION
  Checks: Visual Studio 2022 Build Tools (C++), CMake >= 3.24, Ninja, Git,
  Python 3.11 (py launcher). Installs Qt (LGPL, official binaries) with
  aqtinstall into .deps\Qt - no Qt account needed. Nothing is installed
  system-wide except what you confirm. ANVIL itself never uses the network;
  this script is the only build step that downloads (Qt, and the pinned git
  dependencies during the first CMake configure).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\setup-windows.ps1
#>
param(
  [string]$QtVersion = "6.8.3",
  [string]$QtArch = "win64_msvc2022_64",
  [switch]$Yes
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$deps = Join-Path $root ".deps"

function Need($name, $ok, $hint) {
  if ($ok) { Write-Host ("  [ok]   " + $name) -ForegroundColor Green }
  else { Write-Host ("  [miss] " + $name + "  ->  " + $hint) -ForegroundColor Yellow; $script:missing += $name }
}
$missing = @()
Write-Host "ANVIL build prerequisites" -ForegroundColor Cyan

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = $null
if (Test-Path $vswhere) {
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
Need "Visual Studio 2022 (C++ build tools)" ([bool]$vs) "winget install Microsoft.VisualStudio.2022.BuildTools --override '--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'"
Need "CMake >= 3.24" ([bool](Get-Command cmake -ErrorAction SilentlyContinue)) "winget install Kitware.CMake"
Need "Git" ([bool](Get-Command git -ErrorAction SilentlyContinue)) "winget install Git.Git"
$py = $null
try { $py = (& py -3.11 -c "import sys; print(sys.executable)") 2>$null } catch {}
Need "Python 3.11 (py -3.11)" ([bool]$py) "winget install Python.Python.3.11"

# GPU check (informational; ANVIL repeats it at every start)
$smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
if ($smi) {
  $gpu = & nvidia-smi --query-gpu=name,memory.total,driver_version,compute_cap --format=csv,noheader
  Write-Host ("  [info] GPU: " + $gpu)
} else {
  Write-Host "  [info] nvidia-smi not found (NVIDIA driver missing?). ANVIL needs a Vulkan 1.1+ driver." -ForegroundColor Yellow
}

if ($missing.Count -gt 0) {
  Write-Host "`nInstall the missing tools above, then run this script again." -ForegroundColor Yellow
  exit 1
}

$qtDir = Join-Path $deps "Qt\$QtVersion\msvc2022_64"
if (-not (Test-Path (Join-Path $qtDir "bin\qmake.exe"))) {
  if (-not $Yes) {
    $a = Read-Host "Download Qt $QtVersion ($QtArch, ~1 GB, LGPL) into .deps\Qt via aqtinstall? [y/N]"
    if ($a -ne "y") { Write-Host "Aborted."; exit 1 }
  }
  New-Item -ItemType Directory -Force -Path $deps | Out-Null
  & py -3.11 -m venv (Join-Path $deps "aqt-venv")
  $aqtPy = Join-Path $deps "aqt-venv\Scripts\python.exe"
  & $aqtPy -m pip install --quiet "aqtinstall==3.3.0"
  & $aqtPy -m aqt install-qt windows desktop $QtVersion $QtArch --outputdir (Join-Path $deps "Qt")
}
Write-Host ("  [ok]   Qt " + $QtVersion + " at " + $qtDir) -ForegroundColor Green

# Remember paths for the build script
@{ QtDir = $qtDir; Python = $py; VsPath = $vs } | ConvertTo-Json | Set-Content (Join-Path $deps "env.json")
Write-Host "`nNext:  powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1" -ForegroundColor Cyan
