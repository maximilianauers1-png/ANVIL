<#
.SYNOPSIS
  Runs the M1 gate benchmarks on this machine and writes bench\reports\M1-windows-<date>.{json,md}.

.DESCRIPTION
  G1  viewport frame time, BS-5M scene (5,018,460 triangles), 3 x 1800 frames orbit
  G2  radial menu visible after <= 1 frame (from the GUI self-test)
  G3  cold start: process creation -> first frame, median of 10 runs
  plus the CPU core benchmarks (anvil-bench: validation, BVH, snapping, STL).
  Requires a built package (scripts\build-windows.ps1) and numpy for the scene generator.
#>
param([int]$Frames = 1800, [int]$Runs = 3, [int]$StartupRuns = 10)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist\ANVIL"
$exe = Join-Path $dist "ANVIL.exe"
if (-not (Test-Path $exe)) { throw "Build first: scripts\build-windows.ps1" }
$out = Join-Path $root "bench\out"
$scene = Join-Path $out "bs5m.stl"
if (-not (Test-Path $scene)) {
  & py -3.11 (Join-Path $root "bench\scene\generate_scene.py") --out $out
}
$stamp = Get-Date -Format "yyyyMMdd-HHmm"
$reports = Join-Path $root "bench\reports"
New-Item -ItemType Directory -Force -Path $reports | Out-Null
$res = [ordered]@{ date = (Get-Date).ToString("s"); machine = $env:COMPUTERNAME }
$res.gpu = (& nvidia-smi --query-gpu=name,memory.total,driver_version,compute_cap --format=csv,noheader) 2>$null
$res.cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
Add-Type -AssemblyName System.Windows.Forms
$scr = [System.Windows.Forms.Screen]::PrimaryScreen
$res.display = "{0}x{1}" -f $scr.Bounds.Width, $scr.Bounds.Height

# G3 cold start (median of N)
$starts = @()
for ($i = 0; $i -lt $StartupRuns; $i++) {
  $f = Join-Path $env:TEMP "anvil_start_$i.txt"
  Start-Process -FilePath $exe -ArgumentList "--measure-startup", $f -Wait
  $starts += [double](Get-Content $f)
}
$res.G3_startup_ms = @{ runs = $starts; median = ($starts | Sort-Object)[[int]($starts.Count / 2)]; target = 3000 }

# G1 viewport frame times
$g1 = @()
for ($r = 0; $r -lt $Runs; $r++) {
  $j = Join-Path $env:TEMP "anvil_g1_$r.json"
  Start-Process -FilePath $exe -ArgumentList "--bench", $Frames, "--bench-out", $j, "--bench-scene", $scene -Wait
  $g1 += (Get-Content $j | ConvertFrom-Json)
}
$res.G1_viewport = $g1

# G2 + full UI acceptance
$st = Join-Path $env:TEMP "anvil_selftest"
Start-Process -FilePath $exe -ArgumentList "--selftest", $st -Wait
$rep = Get-Content (Join-Path $st "report.json") | ConvertFrom-Json
$res.G2_radial_frames = $rep.radial_frames_to_visible
$res.selftest_ok = $rep.ok

# Core CPU benchmarks
$cj = Join-Path $env:TEMP "anvil_core.json"
& (Join-Path $dist "anvil-bench.exe") $scene --repeat 3 --out $cj | Out-Host
$res.core = Get-Content $cj | ConvertFrom-Json

$json = Join-Path $reports "M1-windows-$stamp.json"
$res | ConvertTo-Json -Depth 6 | Set-Content $json
$p99 = ($g1 | ForEach-Object { $_.cpu_present_interval_ms.p99 } | Measure-Object -Maximum).Maximum
$md = @"
# M1 Benchmark (Windows) $stamp

| Gate | Messwert | Ziel | Status |
|---|---|---|---|
| G1 Viewport p99 (BS-5M, $Frames Frames x $Runs) | $([math]::Round($p99,2)) ms | <= 16.7 ms | $(if ($p99 -le 16.7) {'erfuellt'} else {'VERFEHLT'}) |
| G2 Radialmenue sichtbar | $($res.G2_radial_frames) Frame(s) | <= 1 | $(if ($res.G2_radial_frames -le 1) {'erfuellt'} else {'VERFEHLT'}) |
| G3 Kaltstart (Median) | $($res.G3_startup_ms.median) ms | < 3000 ms | $(if ($res.G3_startup_ms.median -lt 3000) {'erfuellt'} else {'VERFEHLT'}) |
| UI-Abnahme Magnetbasis | $($res.selftest_ok) | true | |

GPU: $($res.gpu)  ·  CPU: $($res.cpu)  ·  Display: $($res.display)
Rohdaten: $(Split-Path -Leaf $json)
"@
Set-Content (Join-Path $reports "M1-windows-$stamp.md") $md
Write-Host $md
