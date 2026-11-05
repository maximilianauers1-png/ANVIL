<#
.SYNOPSIS
  Builds OpenCASCADE (only the modules anvil-geom needs) as its own CMake
  project and installs it to .deps\occt. Runs once; later calls return at once.

.DESCRIPTION
  OCCT 8 cannot be built as a FetchContent sub-project (its generated
  Standard_Version.hxx ends up outside the include path), so it is built
  stand-alone and found with find_package(OpenCASCADE). The tag comes from
  cmake\Versions.cmake (ANVIL_OCCT_TAG). Source: github.com/Open-Cascade-SAS/OCCT
  (LGPL-2.1 + OCCT exception, linked dynamically, only by anvil-geom).
  Must run inside a VS developer shell (cl.exe, ninja on PATH).
#>
param([string]$Dest = "")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Dest) { $Dest = Join-Path $root ".deps\occt" }
if (Test-Path (Join-Path $Dest "cmake\OpenCASCADEConfig.cmake")) { Write-Host "OCCT already built: $Dest"; return }

$tag = (Select-String -Path (Join-Path $root "cmake\Versions.cmake") -Pattern 'ANVIL_OCCT_TAG\s+(\S+)\)').Matches[0].Groups[1].Value
$src = Join-Path $root ".deps\occt-src"
$bld = Join-Path $root ".deps\occt-build"
if (-not (Test-Path (Join-Path $src "CMakeLists.txt"))) {
  git clone --depth 1 --branch $tag https://github.com/Open-Cascade-SAS/OCCT.git $src
  if ($LASTEXITCODE) { throw "git clone of OCCT $tag failed" }
}
$opts = @(
  "-G", "Ninja", "-S", $src, "-B", $bld,
  "-DCMAKE_BUILD_TYPE=Release", "-DINSTALL_DIR=$Dest", "-DBUILD_LIBRARY_TYPE=Shared",
  "-DBUILD_MODULE_ApplicationFramework=OFF", "-DBUILD_MODULE_Draw=OFF", "-DBUILD_MODULE_Visualization=OFF",
  "-DBUILD_MODULE_DETools=OFF", "-DBUILD_MODULE_DataExchange=ON", "-DBUILD_MODULE_ModelingAlgorithms=ON",
  "-DBUILD_MODULE_ModelingData=ON", "-DBUILD_MODULE_FoundationClasses=ON", "-DBUILD_DOC_Overview=OFF",
  "-DUSE_FREETYPE=OFF", "-DUSE_TK=OFF", "-DUSE_TCL=OFF", "-DUSE_OPENGL=OFF", "-DUSE_GLES2=OFF", "-DUSE_D3D=OFF",
  "-DUSE_XLIB=OFF", "-DUSE_RAPIDJSON=OFF", "-DUSE_DRACO=OFF", "-DUSE_TBB=OFF", "-DUSE_VTK=OFF"
)
cmake @opts
if ($LASTEXITCODE) { throw "OCCT configure failed" }
cmake --build $bld
if ($LASTEXITCODE) { throw "OCCT build failed" }
cmake --install $bld
if ($LASTEXITCODE) { throw "OCCT install failed" }
Write-Host "OCCT $tag installed to $Dest" -ForegroundColor Green
