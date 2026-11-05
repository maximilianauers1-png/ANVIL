<#
.SYNOPSIS
  Builds, tests and packages ANVIL on Windows (MSVC + Ninja).

.DESCRIPTION
  1. configure + build (preset windows-release) inside a VS developer shell
  2. ctest (unit, acceptance; the GUI end-to-end test runs on the real GPU)
  3. package into dist\ANVIL: executables, Qt (windeployqt), an isolated
     Python 3.11 runtime WITHOUT socket/ssl modules (embedded Python cannot
     open network connections), the anvil package, examples.
  The result runs without installation: dist\ANVIL\ANVIL.exe

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1
  powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -SkipTests
#>
param([switch]$SkipTests, [switch]$SkipPackage)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$envFile = Join-Path $root ".deps\env.json"
if (-not (Test-Path $envFile) -and -not $env:QT_ROOT_DIR) { throw "Run scripts\setup-windows.ps1 first." }
if (Test-Path $envFile) {
  $cfg = Get-Content $envFile | ConvertFrom-Json
  if (-not $env:QT_ROOT_DIR) { $env:QT_ROOT_DIR = $cfg.QtDir }
  $pyExe = $cfg.Python
} else {
  $pyExe = (Get-Command python).Source
}

# Enter the VS developer environment (cl.exe, ninja) if not already there
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
  Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
}
$env:Path = (Join-Path $env:QT_ROOT_DIR "bin") + ";" + $env:Path

Push-Location $root
try {
  # OpenCASCADE for anvil-geom: built once as its own project (scripts\build-occt.ps1)
  & (Join-Path $PSScriptRoot "build-occt.ps1")
  $occt = Join-Path $root ".deps\occt"
  cmake --preset windows-release "-DPython3_EXECUTABLE=$pyExe" "-DOpenCASCADE_DIR=$occt\cmake"
  if ($LASTEXITCODE) { throw "configure failed" }
  cmake --build --preset windows-release
  if ($LASTEXITCODE) { throw "build failed" }

  if (-not $SkipTests) {
    ctest --preset windows-release
    if ($LASTEXITCODE) { throw "tests failed" }
  }
  if ($SkipPackage) { return }

  $dist = Join-Path $root "dist\ANVIL"
  if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
  cmake --install build-win --prefix $dist --component anvil
  if ($LASTEXITCODE) { throw "install failed" }
  # OCCT license texts (LGPL-2.1 + exception) travel with the DLLs
  $lic = Join-Path $dist "licenses\OCCT"
  New-Item -ItemType Directory -Force -Path $lic | Out-Null
  Get-ChildItem $occt -Recurse -Include "LICENSE_LGPL_21.txt", "OCCT_LGPL_EXCEPTION.txt" -ErrorAction SilentlyContinue |
    Select-Object -First 2 | ForEach-Object { Copy-Item $_.FullName $lic }

  # Qt runtime (dynamically linked, LGPL: DLLs stay replaceable)
  & (Join-Path $env:QT_ROOT_DIR "bin\windeployqt.exe") --release --no-translations --no-system-d3d-compiler `
      --no-opengl-sw --qmldir (Join-Path $root "app\qml") (Join-Path $dist "ANVIL.exe")
  if ($LASTEXITCODE) { throw "windeployqt failed" }

  # Isolated Python runtime from the local Python 3.11 installation (no download)
  $pyHome = & $pyExe -c "import sys; print(sys.base_prefix)"
  $ver = & $pyExe -c "import sys; print(f'{sys.version_info.major}{sys.version_info.minor}')"
  Copy-Item (Join-Path $pyHome "python$ver.dll") $dist
  Copy-Item (Join-Path $pyHome "python3.dll") $dist
  # Visual C++ runtime, app-local (no separate redistributable install needed)
  $crt = $null
  if ($env:VCToolsRedistDir) {
    $crt = Get-ChildItem (Join-Path $env:VCToolsRedistDir "x64") -Directory -Filter "Microsoft.VC*.CRT" | Select-Object -First 1
  }
  if ($crt) { Copy-Item (Join-Path $crt.FullName "*.dll") $dist }
  else { Get-ChildItem $pyHome -Filter "vcruntime140*.dll" | ForEach-Object { Copy-Item $_.FullName $dist } }
  $zip = Join-Path $dist "python$ver.zip"
  & $pyExe (Join-Path $root "scripts\make_python_zip.py") (Join-Path $pyHome "Lib") $zip
  if ($LASTEXITCODE) { throw "stdlib zip failed" }
  $dlls = Join-Path $dist "DLLs"
  New-Item -ItemType Directory -Force -Path $dlls | Out-Null
  # Extension modules, except networking: no _socket/_ssl/libssl/libcrypto => no sockets from embedded Python.
  Get-ChildItem (Join-Path $pyHome "DLLs") -Include *.pyd, *.dll -Recurse |
    Where-Object { $_.Name -notmatch '^(_socket|_ssl|_hashlib|libssl|libcrypto|_tkinter|tcl|tk)' } |
    ForEach-Object { Copy-Item $_.FullName $dlls }
  # ._pth: isolated module search path (ignores PYTHONPATH and user site-packages)
  $pth = "python$ver.zip`r`nDLLs`r`npython`r`n."
  Set-Content -Path (Join-Path $dist "ANVIL._pth") -Value $pth -Encoding ascii
  Set-Content -Path (Join-Path $dist "python$ver._pth") -Value $pth -Encoding ascii
  Copy-Item (Join-Path $root "THIRD_PARTY.md") $dist
  Copy-Item (Join-Path $root "scripts\LIESMICH.txt") $dist
  # Fail the build if a runtime DLL is missing (what users would otherwise see as a start error)
  foreach ($need in @("ANVIL.exe", "anvil-cli.exe", "python$ver.dll", "python$ver.zip", "vcruntime140.dll", "msvcp140.dll", "Qt6Quick.dll", "anvil-geom.exe", "TKernel.dll")) {
    if (-not (Test-Path (Join-Path $dist $need))) { throw "package incomplete: $need missing" }
  }
  Write-Host "`nPackaged: $dist\ANVIL.exe" -ForegroundColor Green
} finally {
  Pop-Location
}
