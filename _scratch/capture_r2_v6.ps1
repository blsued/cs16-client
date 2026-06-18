# ============================================================================
# capture_r2_v6.ps1  --  R2 blackfog volumetric-flashlight evidence capture
#
#   !!! DO NOT RUN WITHOUT THE ENGINE LOCK !!!
#   This script launches xash3d.exe against D:\csoz-run-r2-blackfog. Only the
#   orchestrator/operator may run it, and only while holding the engine lock.
#   It is authored here for the LATER locked capture phase.
#
# What this fixes vs capture_r2.ps1 (the broken chain):
#   1. STAGES + HASHES the client.dll. The old script ran whatever DLL was
#      already deployed and never verified it. This one copies the canonical
#      build output to the deploy path and asserts source SHA == deployed SHA,
#      aborting on mismatch. Provenance is written to R2_EVIDENCE.
#   2. CAMERA via csz_debugcam (FCVAR_CLIENTDLL cvars registered by the client
#      DLL: csz_view.cpp:176-190). setpos / setang are PROVEN UNSUPPORTED in
#      this xash3d build ("Unknown command" in all 7 prior runs) and are NOT
#      used here. A runtime proof gate scans the copied engine.log for
#      "Unknown command" to confirm the camera command was accepted.
#   3. FPS-valid: does NOT pin fps_max 200 (which clamped the "200 FPS" reading
#      to meaninglessness). FPS-mode sets fps_max 1000 so true headroom shows.
#   4. Drives csz_volumetric_quality 0|1|2|3 (OFF/LOW/MED/HIGH) and the master
#      csz_flashlight_volumetric 0|1 for A/B and per-tier perf runs.
#
# Single source of truth for the DLL:
#   build\cl_dll\Release\client.dll  (cmake --build build --config Release
#   --target client writes here; confirmed via CMakeCache client_BINARY_DIR
#   + client.vcxproj OutDir). build\client\ is a SEPARATE STALE tree -- ignore.
#
# Camera interface (csz_view.cpp):
#   csz_debugcam      0=off(engine view)  1=explicit pos/ang  2=orbit testlight
#   csz_debugcam_pos  "x y z"            (mode 1; empty/garbage => no override)
#   csz_debugcam_ang  "pitch yaw roll"   (mode 1)
#   csz_debugcam_dist / _side / _height  (mode 2 orbit framing)
#   Mode 2 needs csz_testlight 1 (spot at the T spawn, registry key -2).
#
# Volumetric tiers (csz_volumetric.cpp:307-329):
#   csz_volumetric_quality 0=OFF(no shaft) 1=LOW(12 steps) 2=MED(24) 3=HIGH(48+shadow-carve)
#   csz_flashlight_volumetric 0|1  master on/off for the shaft
#
# Perf readout: with developer 1, SampleFps (csz_renderer.cpp:102-160) emits
#   once/sec to engine.log:
#     [fps] fps=... avg_ms=... worst_ms=... frames=...
#     [fps] pass-ms avg: ... volume=X.XX ...   <- kTmVolume (volumetric CPU ms)
#   Both are grepped out of the copied engine log; no in-game GPU timer query.
# ============================================================================

param(
  # Scenario names to run; empty = all.
  [string[]]$Only,
  # FPS mode: sets fps_max 1000 and a long hold so multiple fps/pass-ms lines land.
  [switch]$FpsMode
)
$ErrorActionPreference = "Stop"

# ---- Paths -----------------------------------------------------------------
$RunDir      = "D:\csoz-run-r2-blackfog"
$ClientDir   = Join-Path $RunDir "client"
$CstrikeDir  = Join-Path $ClientDir "cstrike"
$Exe         = Join-Path $ClientDir "xash3d.exe"
$Scrshots    = Join-Path $CstrikeDir "scrshots"
$EngineLog   = Join-Path $ClientDir "engine.log"
$EvidenceDir = "D:\csoz-wt\blackfog\R2_EVIDENCE"
$Port        = 27021

# Single source of truth for the client DLL + the deploy target.
$SrcDll = "D:\csoz-wt\blackfog\build\cl_dll\Release\client.dll"
$DstDll = "D:\csoz-run-r2-blackfog\client\cstrike\cl_dlls\client.dll"

# ---- DLL stage + hash verify (HARD GATE) -----------------------------------
# Pin the PATH as the source of truth and verify copy integrity source-vs-deployed.
# No hardcoded expected-hash literal (that is exactly what went stale before).
function Stage-VerifyDll {
  New-Item -ItemType Directory -Force $EvidenceDir | Out-Null

  if (-not (Test-Path -LiteralPath $SrcDll)) {
    Write-Error "FATAL: canonical client.dll not found at $SrcDll. Build it first: cmake --build D:/csoz-wt/blackfog/build --config Release --target client"
    exit 1
  }

  $srcHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $SrcDll).Hash
  $srcSize = (Get-Item -LiteralPath $SrcDll).Length

  $dstDir = Split-Path -Parent $DstDll
  New-Item -ItemType Directory -Force $dstDir | Out-Null
  Copy-Item -LiteralPath $SrcDll -Destination $DstDll -Force

  $dstHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $DstDll).Hash
  $dstSize = (Get-Item -LiteralPath $DstDll).Length

  Write-Host "DLL SRC  $SrcDll"
  Write-Host "    sha256=$srcHash size=$srcSize"
  Write-Host "DLL DST  $DstDll"
  Write-Host "    sha256=$dstHash size=$dstSize"

  if ($srcHash -ne $dstHash) {
    Write-Error "FATAL: deployed DLL SHA ($dstHash) != source SHA ($srcHash). Copy integrity failed -- ABORTING, no capture."
    exit 2
  }

  # Record provenance so every screenshot in this batch is traceable to a SHA.
  $stamp = Get-Date -Format "yyyy-MM-ddTHH:mm:ss"
  $prov  = @(
    "client.dll deployed SHA256 (verified source==deployed)",
    "when:   $stamp",
    "src:    $SrcDll",
    "dst:    $DstDll",
    "sha256: $dstHash",
    "size:   $dstSize bytes"
  )
  Set-Content -LiteralPath (Join-Path $EvidenceDir "client.deployed.sha256.txt") -Value $prov -Encoding ASCII
  Write-Host "DLL VERIFIED: source SHA == deployed SHA ($dstHash)"
  return $dstHash
}

# ---- helpers ---------------------------------------------------------------
function Wait-Lines([int]$Count) {
  $l = New-Object System.Collections.Generic.List[string]
  for ($i = 0; $i -lt $Count; $i++) { $l.Add("wait") }
  return $l
}

function Stop-WorkerEngines {
  $escaped = [regex]::Escape($RunDir)
  $procs = Get-CimInstance Win32_Process -Filter "Name = 'xash3d.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match $escaped }
  foreach ($proc in $procs) {
    try { Stop-Process -Id ([int]$proc.ProcessId) -Force -ErrorAction Stop } catch {}
  }
}

# Unique markers bracketing the camera command. The copied engine.log is later
# scanned: if "Unknown command" appears between these markers the camera was
# rejected (the setpos/setang failure mode) and the run is flagged invalid.
$CamMarkBegin = "echo CSZCAM_BEGIN"
$CamMarkEnd   = "echo CSZCAM_END"

function Write-CaptureAutoexec([hashtable]$Scenario) {
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add("// GENERATED capture_r2_v6 for $($Scenario.Name)")
  $lines.Add("setinfo _vgui_menus 0")
  $lines.Add("developer 1")            # enables [fps]/[testlight] dev log lines
  $lines.Add("con_notifytime 0")
  if ($FpsMode) { $lines.Add("fps_max 1000") }   # NOT 200: observe true headroom
  $lines.Add("sv_cheats 1")
  $lines.Add("sv_lan 1")
  $lines.Add("mp_consistency 0")
  $lines.Add("maxplayers 4")
  $lines.Add("mp_freezetime 0")
  $lines.Add("bot_quota 0")
  $lines.Add("bot_join_after_player 0")
  $lines.Add("hud_draw 0")
  $lines.Add("crosshair 0")
  $lines.Add("csz_renderer 1")
  $lines.Add("csz_sky_phase 0.5")
  $lines.Add("map $($Scenario.Map)")
  (Wait-Lines 650) | ForEach-Object { $lines.Add($_) }
  $lines.Add("setinfo _vgui_menus 0")
  $lines.Add("jointeam 2")
  $lines.Add("joinclass 1")
  (Wait-Lines 260) | ForEach-Object { $lines.Add($_) }

  # noclip kept for free movement (proven supported). Camera is set via
  # csz_debugcam, NOT setpos/setang.
  $lines.Add("noclip")

  # --- volumetric / flashlight feature state ---
  $lines.Add("csz_flashlight $($Scenario.Flashlight)")
  $lines.Add("csz_flashlight_volumetric $($Scenario.VolMaster)")
  $lines.Add("csz_volumetric_quality $($Scenario.Quality)")
  if ($Scenario.ContainsKey("Shadow")) { $lines.Add("csz_light_shadow $($Scenario.Shadow)") }
  if ($Scenario.ContainsKey("TestLight")) { $lines.Add("csz_testlight $($Scenario.TestLight)") }
  foreach ($cmd in $Scenario.ExtraCmds) { $lines.Add($cmd) }

  # --- camera (csz_debugcam) bracketed by proof markers ---
  $lines.Add($CamMarkBegin)
  if ($Scenario.CamMode -eq 2) {
    # Orbit the testlight (requires csz_testlight 1 above).
    $lines.Add("csz_debugcam_dist $($Scenario.CamDist)")
    $lines.Add("csz_debugcam_side $($Scenario.CamSide)")
    $lines.Add("csz_debugcam_height $($Scenario.CamHeight)")
    $lines.Add("csz_debugcam 2")
  } else {
    # Explicit deterministic pos/ang. Space-containing cvar value must be quoted.
    $lines.Add("csz_debugcam_pos `"$($Scenario.Pos)`"")
    $lines.Add("csz_debugcam_ang `"$($Scenario.Ang)`"")
    $lines.Add("csz_debugcam 1")
  }
  $lines.Add($CamMarkEnd)

  (Wait-Lines 320) | ForEach-Object { $lines.Add($_) }
  $lines.Add("screenshot")
  $hold = 80
  if ($FpsMode) { $hold = 1800 }       # ~30s+ so several fps/pass-ms lines land
  if ($Scenario.ContainsKey("HoldWaits")) { $hold = $Scenario.HoldWaits }
  (Wait-Lines $hold) | ForEach-Object { $lines.Add($_) }
  $lines.Add("quit")
  Set-Content -LiteralPath (Join-Path $CstrikeDir "autoexec.cfg") -Value $lines -Encoding ASCII
}

# Scan the copied engine log for the camera proof gate + report fps/pass-ms.
function Inspect-EngineLog([string]$LogPath, [string]$Name) {
  if (-not (Test-Path -LiteralPath $LogPath)) {
    Write-Warning "  [$Name] no engine log copied -- cannot prove camera acceptance"
    return
  }
  $log = Get-Content -LiteralPath $LogPath
  $bi = ($log | Select-String -SimpleMatch "CSZCAM_BEGIN" | Select-Object -First 1).LineNumber
  $ei = ($log | Select-String -SimpleMatch "CSZCAM_END"   | Select-Object -First 1).LineNumber
  if ($bi -and $ei -and $ei -gt $bi) {
    $between = $log[($bi)..($ei - 2)]   # lines strictly between the two markers
    $bad = $between | Select-String -SimpleMatch "Unknown command"
    if ($bad) {
      Write-Warning "  [$Name] CAMERA REJECTED: 'Unknown command' between markers -- INVALID shot:"
      $bad | ForEach-Object { Write-Warning "      $_" }
    } else {
      Write-Host    "  [$Name] camera proof gate PASSED (no 'Unknown command' between markers)"
    }
  } else {
    Write-Warning "  [$Name] camera markers not found in log -- cannot confirm camera acceptance"
  }
  # Surface a sample of the perf readout (kTmVolume = volume=...).
  $fps = $log | Select-String -SimpleMatch "fps=" | Select-Object -Last 3
  $vol = $log | Select-String -SimpleMatch "pass-ms avg" | Select-Object -Last 3
  if ($fps) { Write-Host "  [$Name] fps tail:"; $fps | ForEach-Object { Write-Host "      $_" } }
  if ($vol) { Write-Host "  [$Name] pass-ms tail (volume=kTmVolume):"; $vol | ForEach-Object { Write-Host "      $_" } }
}

function Invoke-CaptureScenario([hashtable]$Scenario) {
  New-Item -ItemType Directory -Force $Scrshots | Out-Null
  Get-ChildItem -LiteralPath $Scrshots -Filter *.png -ErrorAction SilentlyContinue | Remove-Item -Force
  if (Test-Path -LiteralPath $EngineLog) { Remove-Item -LiteralPath $EngineLog -Force }

  Write-CaptureAutoexec -Scenario $Scenario

  $args = @("-log","-game","cstrike","-windowed","-width","1280","-height","720","-noborder","+port",[string]$Port,"-dev","2")
  Write-Host "CAPTURE $($Scenario.Name) map=$($Scenario.Map) fpsMode=$FpsMode"
  $proc = Start-Process -FilePath $Exe -ArgumentList $args -WorkingDirectory $ClientDir -WindowStyle Hidden -PassThru

  $timeout = 120
  if ($FpsMode) { $timeout = 180 }
  if ($Scenario.ContainsKey("TimeoutSec")) { $timeout = $Scenario.TimeoutSec }
  $deadline = (Get-Date).AddSeconds($timeout)

  # PROVEN-GOOD pattern (kept from v1): poll for the PNG to appear AND wait for
  # the engine process to exit (so engine.log is fully flushed) before copying.
  $shot = $null
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 1500
    $shot = Get-ChildItem -LiteralPath $Scrshots -Filter *.png -ErrorAction SilentlyContinue |
      Sort-Object LastWriteTime | Select-Object -Last 1
    if ($shot) {
      while ((Get-Date) -lt $deadline -and -not $proc.HasExited) { Start-Sleep -Milliseconds 1000 }
      break
    }
  }

  try { if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force } } catch {}
  Start-Sleep -Milliseconds 800

  $copiedShot = $null
  $shot = Get-ChildItem -LiteralPath $Scrshots -Filter *.png -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime | Select-Object -Last 1
  if ($shot) {
    $copiedShot = Join-Path $EvidenceDir "$($Scenario.Name).png"
    Copy-Item -LiteralPath $shot.FullName -Destination $copiedShot -Force
  }
  $copiedLog = $null
  if (Test-Path -LiteralPath $EngineLog) {
    $copiedLog = Join-Path $EvidenceDir "engine_$($Scenario.Name).log"
    Copy-Item -LiteralPath $EngineLog -Destination $copiedLog -Force
  }
  Write-Host "  shot=$copiedShot"
  if ($copiedLog) { Inspect-EngineLog -LogPath $copiedLog -Name $Scenario.Name }
}

# ============================================================================
# Scenarios
#   Pos/Ang are placeholders to be pinned for de_dust2 during the locked phase
#   (noclip + a known landmark), then frozen here. The wall-bleed scenario needs
#   the camera so an opaque wall sits between camera and the lit cone.
#   CamMode 1 = explicit pos/ang; CamMode 2 = orbit the testlight.
# ============================================================================
$DefPos = "-700 -1550 110"     # placeholder; pin during locked phase
$DefAng = "0 90 0"

$scenarios = @(
  # --- Decisive wall-between-camera-and-cone: expect NO shaft bleed through wall.
  @{ Name="bf_v6_wall_bleed_on"; Map="de_dust2"; CamMode=1; Pos=$DefPos; Ang=$DefAng;
     Flashlight=1; VolMaster=1; Quality=3; Shadow=1; ExtraCmds=@("csz_fog_default_density 0.0018") }

  # --- A/B for the SAME camera: volumetric ON vs OFF.
  @{ Name="bf_v6_ab_on";  Map="de_dust2"; CamMode=1; Pos=$DefPos; Ang=$DefAng;
     Flashlight=1; VolMaster=1; Quality=2; Shadow=0; ExtraCmds=@("csz_fog_default_density 0.0018") }
  @{ Name="bf_v6_ab_off"; Map="de_dust2"; CamMode=1; Pos=$DefPos; Ang=$DefAng;
     Flashlight=1; VolMaster=0; Quality=0; Shadow=0; ExtraCmds=@("csz_fog_default_density 0.0018") }

  # --- Per-tier visual + perf. Run these with -FpsMode for valid FPS/pass-ms.
  @{ Name="bf_v6_tier0_off";  Map="de_dust2"; CamMode=1; Pos=$DefPos; Ang=$DefAng;
     Flashlight=1; VolMaster=1; Quality=0; Shadow=0; ExtraCmds=@() }
  @{ Name="bf_v6_tier1_low";  Map="de_dust2"; CamMode=1; Pos=$DefPos; Ang=$DefAng;
     Flashlight=1; VolMaster=1; Quality=1; Shadow=0; ExtraCmds=@() }
  @{ Name="bf_v6_tier2_med";  Map="de_dust2"; CamMode=1; Pos=$DefPos; Ang=$DefAng;
     Flashlight=1; VolMaster=1; Quality=2; Shadow=0; ExtraCmds=@() }
  @{ Name="bf_v6_tier3_high"; Map="de_dust2"; CamMode=1; Pos=$DefPos; Ang=$DefAng;
     Flashlight=1; VolMaster=1; Quality=3; Shadow=1; ExtraCmds=@() }

  # --- Orbit the testlight cone side-on (CamMode 2) for a framed shaft beauty shot.
  @{ Name="bf_v6_orbit_high"; Map="de_dust2"; CamMode=2; TestLight=1;
     CamDist=260; CamSide=260; CamHeight=80;
     Flashlight=1; VolMaster=1; Quality=3; Shadow=1; ExtraCmds=@() }
)

# ---- run -------------------------------------------------------------------
Stop-WorkerEngines
$dllSha = Stage-VerifyDll      # HARD GATE: aborts (exit 1/2) before any capture
foreach ($sc in $scenarios) {
  if ($Only -and ($Only -notcontains $sc.Name)) { continue }
  Invoke-CaptureScenario -Scenario $sc
}
Stop-WorkerEngines
Write-Host "ALL DONE (dll sha256=$dllSha)"
