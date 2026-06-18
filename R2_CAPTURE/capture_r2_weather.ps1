<#
================================================================================
 capture_r2_weather.ps1
================================================================================
 Non-interactive screenshot + FPS capture for the CSOZ Xash3D weather/water
 renderer (R2 round).

 PURPOSE
   For each scenario in the matrix it:
     1. Overlays the fresh WIP client.dll into the (already packaged) run dir.
     2. Generates a per-scenario autoexec.cfg that loads the map (no team join
        -> observer, so no center/join HUD text), aims the view via the C++
        debugcam cvars (csz_debugcam*), hardens all clean-shot cvars POST-load,
        lifts the FPS cap (markers + readback), sets the weather/water cvars,
        takes a screenshot, then
        holds the live scene ~65s so the engine accumulates >=55 per-second
        [CSZ:fps] log samples between echo markers (for parse_fps.py), then quits.
     3. Launches xash3d.exe, waits for the screenshot + a sane-fps spawn
        confirmation, then kills the engine.
     4. Copies the png + engine.log into the evidence dir, scrapes turb-surface
        counts (aztec water) and avg fps, and records a per-scenario result.
   Finally writes capture_results.json + a SUMMARY block.

 PROVENANCE
   Adapted from the proven sequence in capture_serial.ps1
   (C:\Users\...\work\claude_dispatch\capture_serial.ps1). Reuses its
   anti-menu wait counts, Stop-WorkerEngines / autoexec generation, and the
   screenshot-deadline poll loop. Extended with: wall-clock FPS hold + echo
   markers, spawn sanity-gate, turb-surface scrape, BLOCKER stopwatch.

 ENGINE FACTS (verified from source on 2026-06-18)
   cvars (read from cl_dll\cszrender):
     csz_weather            0 off / 1 rain / 2 snow   (weather\csz_weather.cpp:165)
     csz_weather_intensity  0..1 strength             (csz_weather.cpp:168, default 0.7)
     csz_weather_quality    0 low / 1 med / 2 high     (csz_weather.cpp:171, default 1)
     csz_water              1 on (custom turb pass) / 0 off baseline  (csz_renderer.cpp:198)
     csz_renderer           1 = takeover renderer on   (csz_renderer.cpp:192)
     csz_sky_phase          0..1 day phase             (always registered)
     csz_showfps            on-screen fps text (hidden so it doesn't overlap shot)
   log lines (require launch flag -log; Dev lines also require `developer 1`):
     [CSZ:fps] Dev: fps=.. avg_ms=.. worst_ms=.. frames=..     (csz_renderer.cpp:138)
     [CSZ:fps] Dev: pass-ms avg: shadow=.. ... water=.. ... weather=..  (line 143)
     [CSZ:water] Info: built <map>: N turb surfaces, M verts   (csz_water.cpp:227)
   parse window markers (parse_fps.py defaults): MATPERF-FPS-START / MATPERF-FPS-END.

 USAGE
   powershell -NoProfile -ExecutionPolicy Bypass -File capture_r2_weather.ps1
   powershell ... -File capture_r2_weather.ps1 -Scenarios water_on_de_aztec,water_rain_de_aztec
   powershell ... -File capture_r2_weather.ps1 -AcquireLock $false   # orchestrator already holds lock

 PowerShell 5.1 compatible: no && chaining, no ternary, no null-coalescing.
================================================================================
#>

param(
  [string]   $RunDir        = 'D:\csoz-run-r1-weather',
  [string]   $DllSource     = 'D:\csoz-wt\weather\build\client\cl_dll\Release\client.dll',
  [string]   $EvidenceDir   = 'D:\csoz-wt\weather\R2_EVIDENCE_V5',
  [int]      $Port          = 27022,
  [string]   $Owner         = 'weather',
  [bool]     $AcquireLock   = $true,
  [int]      $FpsHoldSeconds= 65,
  [string[]] $Scenarios     = @(),

  # --- Run identity: every engine log / analyzer JSON gets a UNIQUE name keyed
  #     on this id so re-runs NEVER overwrite prior evidence. Operator may pass
  #     -RunId to label a run; default is a wall-clock stamp. ----------------
  [string]   $RunId         = (Get-Date -Format 'yyyyMMdd_HHmmss'),

  # --- Fullscreen fallback. When set, launch with -fullscreen instead of
  #     -windowed -noborder (same width/height). Lets the operator test whether
  #     the observed ~101fps pin is a DESKTOP-COMPOSITOR vsync cap (windowed):
  #     exclusive fullscreen + gl_vsync 0 should break a compositor cap. -----
  [switch]   $Fullscreen,

  # --- FPS-unlock tunables (operator changes these BETWEEN runs without
  #     rewriting the cfg). Emitted in the post-load UNLOCK block. ----------
  [int]      $FpsMax        = 1000,
  [int]      $HostMaxFps    = 1000,
  [int]      $UpdateRate    = 1000,
  [int]      $CmdRate       = 1000,
  [int]      $SysTicRate    = 1000,

  # --- "Cap defeated" threshold. If the max observed [CSZ:fps] inside the
  #     window does NOT clearly exceed this, the cap is still active and the
  #     scenario is classified FPS_CAP_STILL_ACTIVE (never reported as
  #     headroom). 110 sits safely above the 99/100/101 limiter signature. --
  [double]   $FpsUncapMin   = 110.0,

  # --- Sky/day phase. csz_sky_phase maps 0.0=sunset, 0.5=MIDNIGHT (darkest),
  #     1.0=full daylight. The prior capture wrongly pinned 0.5 (midnight) so the
  #     scenes came out ~37/255 dark. Default to well-lit daylight. The capture
  #     operator may tune this between 0.88 and 1.0 by viewing frames between runs
  #     (no cfg edit needed). Emitted POST-load (authoritative) for ALL scenarios.
  [double]   $SkyPhase      = 0.95
)

$ErrorActionPreference = 'Stop'

# --- Fixed paths -------------------------------------------------------------
$LockDir       = 'C:\Users\Administrator\Documents\Codex\2026-06-17\claude-max20-xash3d-200\work\claude_dispatch\engine_lock'
$AcquireScript = Join-Path $LockDir 'Acquire-CsozEngineLock.ps1'
$ReleaseScript = Join-Path $LockDir 'Release-CsozEngineLock.ps1'

$ClientDir     = Join-Path $RunDir   'client'
$CstrikeDir    = Join-Path $ClientDir 'cstrike'
$Exe           = Join-Path $ClientDir 'xash3d.exe'
$DllTarget     = Join-Path $CstrikeDir 'cl_dlls\client.dll'
$Scrshots      = Join-Path $CstrikeDir 'scrshots'
$EngineLog     = Join-Path $ClientDir 'engine.log'

# Deadlines (wall clock). The screenshot must appear within ScreenshotDeadline;
# the FPS hold then runs on top of that. The hard 180s no-progress abort rule
# is enforced separately via $progressDeadline below.
$ScreenshotDeadlineSeconds = 120
$ProgressGraceSeconds      = 180   # 3-minute rule: must see proc alive AND a png by now

# ============================================================================
#  SCENARIO MATRIX. Self-contained table of objects -- one per deliverable
#  shot. CAMERA NOW USES THE NEW C++ debugcam cvars (setpos/setang are
#  "Unknown command" in this build and produced wall shots).
#
#  debugcam contract (conform EXACTLY -- C++ added in parallel):
#    csz_debugcam 1                  enable; renderer overrides view to pose
#    csz_debugcam_pos "X Y Z"        world position
#    csz_debugcam_ang "PITCH YAW ROLL"   (angle order/sign TBD by other agent)
#  When active the renderer logs a line containing 'debugcam' and
#  'active pos=(...) ang=(...)'.  The log inspector asserts that line appears
#  AND that no 'Unknown command:' line names the debugcam cvars.
#
#  >>> DebugPos / DebugAng are TUNABLE. The capture operator iterates them by
#  >>> viewing frames. de_aztec water seed frames the water body (NOT a wall):
#  >>>   water centroid (328 346 -530); AABB min (-3776 -1856 -544)
#  >>>   max (2208 1984 -344). Seed pos (328 -200 -400) ang (20 90 0):
#  >>>   camera ~130u above water, south of centroid, pitch 20deg down,
#  >>>   yaw toward +Y. The 3 aztec shots SHARE this pose (A/B control).
#
#  Per-scenario fields:
#    Name        scenario id (used in log/json filenames)
#    Map         bsp to load
#    WeatherMode 0 off / 1 rain / 2 snow  -> csz_weather
#    Intensity   0..1 -> csz_weather_intensity ('' = leave default)
#    Quality     0/1/2 -> csz_weather_quality ('' = leave default)
#    WaterCvar   0 / 1 -> csz_water ; 'none' = don't touch
#    DebugPos    "X Y Z"      world position  (TUNABLE)
#    DebugAng    "PITCH YAW ROLL"             (TUNABLE)
#    Hold        $true => run the FpsHoldSeconds wall-clock FPS window
#    ExpectWater $true => engine.log must show N>0 turb surfaces for this map
#    OutPng      EXACT deliverable filename copied into R2_EVIDENCE_V5
# ============================================================================
$AllScenarios = @(
  # --- de_aztec water A/B: csz_debugcam 2 auto-frames the water body in-engine
  #     (self-correcting legit pose), csz_water 0 vs 1 (MUST differ). The blind
  #     hand pose is gone -- the renderer picks a legit eye over the water. ----
  @{ Name='water_off_de_aztec'; Map='de_aztec'; WeatherMode=0; Intensity=''; Quality='';
     WaterCvar=0;     AutoFrameWater=$true; DebugPos='328 -600 -300'; DebugAng='8 90 0';
     Hold=$false; ExpectWater=$true;  OutPng='weather_v5_00_water_off_surface.png' },

  @{ Name='water_on_de_aztec';  Map='de_aztec'; WeatherMode=0; Intensity=''; Quality='';
     WaterCvar=1;     AutoFrameWater=$true; DebugPos='328 -600 -300'; DebugAng='8 90 0';
     Hold=$false; ExpectWater=$true;  OutPng='weather_v5_01_water_on_surface.png' },

  # --- de_dust2 weather clean shots --------------------------------------------
  @{ Name='rain_clean_de_dust2'; Map='de_dust2'; WeatherMode=1; Intensity='0.7'; Quality='2';
     WaterCvar=1;     DebugPos='-700 -1550 140'; DebugAng='2 90 0';
     Hold=$false; ExpectWater=$false; OutPng='weather_v5_02_rain_clean.png' },

  @{ Name='snow_cover_de_dust2'; Map='de_dust2'; WeatherMode=2; Intensity='0.7'; Quality='2';
     WaterCvar=1;     DebugPos='-700 -1550 90'; DebugAng='12 90 0';
     Hold=$false; ExpectWater=$false; OutPng='weather_v5_03_snow_cover_clean.png' },

  @{ Name='wet_ground_de_dust2'; Map='de_dust2'; WeatherMode=1; Intensity='1.0'; Quality='2';
     WaterCvar=1;     DebugPos='-700 -1550 50'; DebugAng='8 90 0';
     Hold=$false; ExpectWater=$false; OutPng='weather_v5_04_wet_ground_clean.png' },

  # --- worst-case FPS stress: heaviest load (high-quality, max-intensity rain
  #     + water on) for the FPS headroom shot. THIS is the hold/FPS-window shot.
  @{ Name='worstcase_fps_de_aztec'; Map='de_aztec'; WeatherMode=1; Intensity='1.0'; Quality='2';
     WaterCvar=1;     AutoFrameWater=$true; DebugPos='328 -600 -300'; DebugAng='8 90 0';
     Hold=$true;  ExpectWater=$true;  OutPng='weather_v5_05_worstcase_fps.png' }
)

# ----------------------------------------------------------------------------
#  Helpers
# ----------------------------------------------------------------------------

function New-WaitLines([int]$Count) {
  # Returns $Count "wait" command strings (one engine frame each).
  $list = New-Object System.Collections.Generic.List[string]
  for ($i = 0; $i -lt $Count; $i++) { $list.Add('wait') }
  return $list
}

function Stop-RunDirEngines {
  # Kill any xash3d.exe whose command line points at THIS run dir, then any
  # stragglers by name. Belt-and-suspenders so the next launch is clean.
  $escaped = [regex]::Escape($RunDir)
  $procs = Get-CimInstance Win32_Process -Filter "Name = 'xash3d.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match $escaped }
  foreach ($p in $procs) {
    try { Stop-Process -Id ([int]$p.ProcessId) -Force -ErrorAction Stop }
    catch { Write-Warning "could not stop xash3d pid=$($p.ProcessId): $($_.Exception.Message)" }
  }
  # Final sweep for any remaining by-name (in case CommandLine was unavailable).
  Get-Process xash3d -ErrorAction SilentlyContinue | ForEach-Object {
    try { Stop-Process -Id $_.Id -Force -ErrorAction Stop } catch {}
  }
  Start-Sleep -Milliseconds 600
}

function Add-ConsistencyOff {
  # Disable consistency / lan gating in the standard cfgs so a listen map loads
  # without a server kicking us. Mirrors capture_serial.ps1.
  $files = @('server.cfg','listenserver.cfg','gamemode.cfg') |
    ForEach-Object { Join-Path $CstrikeDir $_ }
  foreach ($f in $files) {
    if (-not (Test-Path -LiteralPath $f)) { New-Item -ItemType File -Path $f -Force | Out-Null }
    Add-Content -LiteralPath $f -Encoding ASCII -Value @(
      '', '// R2 capture override', 'mp_consistency 0', 'sv_consistency 0'
    )
  }
}

function Get-EffectCmds([hashtable]$Sc) {
  # Translate the self-contained scenario fields into the effect cvar lines.
  # Order: water -> quality/intensity -> weather LAST (weather mode re-reads
  # quality/intensity at toggle). '' / 'none' means "leave default / untouched".
  $cmds = New-Object System.Collections.Generic.List[string]
  if ("$($Sc.WaterCvar)" -ne 'none' -and "$($Sc.WaterCvar)" -ne '') { $cmds.Add("csz_water $($Sc.WaterCvar)") }
  if ("$($Sc.Quality)"   -ne '')                                    { $cmds.Add("csz_weather_quality $($Sc.Quality)") }
  if ("$($Sc.Intensity)" -ne '')                                    { $cmds.Add("csz_weather_intensity $($Sc.Intensity)") }
  $cmds.Add("csz_weather $($Sc.WeatherMode)")
  return $cmds
}

function Write-ScenarioAutoexec([hashtable]$Sc, [int]$HoldSeconds) {
  # Build the autoexec.cfg for one scenario. This is the bulletproof anti-menu
  # + spawn + frame + FPS-window sequence.
  $L = New-Object System.Collections.Generic.List[string]

  $L.Add("// GENERATED by capture_r2_weather.ps1 for $($Sc.Name) runid=$RunId")
  # --- pre-map global setup ---
  $L.Add('setinfo _vgui_menus 0')      # kill VGUI team/class menus BEFORE map
  $L.Add('developer 1')                # REQUIRED: enables [CSZ:fps] Dev log lines
  $L.Add('con_notifytime 0')
  $L.Add('fps_max 200')                # cap (not a fake) -- never host_framerate
  $L.Add('sv_cheats 1')                # debugcam needs cheats
  $L.Add('sv_lan 1')
  $L.Add('mp_consistency 0')
  $L.Add('sv_consistency 0')
  $L.Add('maxplayers 4')
  $L.Add('mp_freezetime 0')
  $L.Add('bot_quota 0')
  $L.Add('bot_join_after_player 0')
  $L.Add('csz_renderer 1')             # ensure custom renderer takeover
  $L.Add("csz_sky_phase $SkyPhase")    # pre-load seed; re-asserted POST-load (authoritative) below
  # --- load map ---
  $L.Add("map $($Sc.Map)")
  (New-WaitLines 650) | ForEach-Object { $L.Add($_) }   # world load (proven count)

  # --- get into the world WITHOUT joining a team for clean visual shots -------
  # The center "Only 1 team change is allowed" text and the top-right join text
  # come from joining a team. We stay as OBSERVER: the world still renders, the
  # debugcam overrides the view to the framed pose, and no team-change/join HUD
  # text appears. (jointeam/joinclass/menuselect removed.) If a future build
  # refuses to render the world without a team, re-add jointeam here.
  $L.Add('setinfo _vgui_menus 0')      # re-assert after load
  (New-WaitLines 260) | ForEach-Object { $L.Add($_) }   # settle on the connect

  # ==========================================================================
  #  POST-LOAD HARDENING -- runs AFTER the userconfig.cfg map-connect re-exec
  #  so nothing it sets can be reset. ALL clean-shot cvars live HERE (the prior
  #  run set them pre-load and they were clobbered, leaving viewmodel/HUD/center
  #  text on the frame).
  # ==========================================================================
  # --- clean-shot cvars (no viewmodel/hands, no crosshair, no HUD, no center
  #     team text, no join/notify text, no console, no on-screen fps) ---------
  $L.Add('r_drawviewmodel 0')
  $L.Add('cl_drawviewmodel 0')
  $L.Add('crosshair 0')
  $L.Add('hud_draw 0')
  $L.Add('cl_drawhud 0')
  $L.Add('scr_centertime 0')     # kill the center "team change" / round text
  $L.Add('cl_showfps 0')
  $L.Add('csz_showfps 0')
  $L.Add('con_notifytime 0')     # no top-left chat/notify echo on the frame
  $L.Add('hud_centerid 0')       # no center entity-id text

  # --- day/sky phase (AUTHORITATIVE, post-load so nothing clobbers it). All
  #     scenarios use the same well-lit daylight phase ($SkyPhase, default 0.95).
  #     Operator tunes -SkyPhase between 0.88..1.0 by viewing frames. ----------
  $L.Add("csz_sky_phase $SkyPhase")

  # ==========================================================================
  #  AUTHORITATIVE FPS-CAP LIFT (POST map-connect) with markers + readback.
  #  IMPORTANT: fps_max <value> (NOT 0): engine binds
  #     fps = bound(20, fps_max, fps_override?1000:200)
  #  so fps_max 0 would clamp to 20. Issuing a BARE cvar name prints its current
  #  value to the -log => the readback. Values come from script params so the
  #  operator can retune updaterate/fps between runs without editing the cfg.
  # ==========================================================================
  $L.Add('echo MATPERF-FPS-UNLOCK-BEGIN')
  $L.Add('gl_vsync 0')
  $L.Add('fps_override 1')
  $L.Add("fps_max $FpsMax")
  $L.Add("host_maxfps $HostMaxFps")
  $L.Add("cl_updaterate $UpdateRate")
  $L.Add("cl_cmdrate $CmdRate")
  $L.Add("sys_ticrate $SysTicRate")
  $L.Add('echo MATPERF-FPS-READBACK')
  $L.Add('gl_vsync')             # bare names => engine prints current values
  $L.Add('fps_override')
  $L.Add('fps_max')
  $L.Add('host_maxfps')
  $L.Add('cl_updaterate')
  $L.Add('sys_ticrate')
  $L.Add('echo MATPERF-FPS-UNLOCK-END')

  # --- camera via debugcam cvars (setpos/setang are Unknown command here) -----
  #   mode 2 = renderer auto-frames the map water (self-correcting legit pose);
  #   the manual pos/ang are ignored by the renderer but still set as a fallback
  #   record. mode 1 = blind manual pose from the table.
  $camMode = if ($Sc.AutoFrameWater) { 2 } else { 1 }
  $L.Add("csz_debugcam $camMode")
  $L.Add("csz_debugcam_pos `"$($Sc.DebugPos)`"")
  $L.Add("csz_debugcam_ang `"$($Sc.DebugAng)`"")

  # --- effect cvars (from the self-contained table fields) --------------------
  foreach ($c in (Get-EffectCmds $Sc)) { $L.Add($c) }

  # re-assert the debugcam pose AFTER effect cvars (in case any reset the view):
  $L.Add("csz_debugcam_pos `"$($Sc.DebugPos)`"")
  $L.Add("csz_debugcam_ang `"$($Sc.DebugAng)`"")
  (New-WaitLines 2500) | ForEach-Object { $L.Add($_) }   # let effects settle + any join/notify HUD lines fade before the shot
  $L.Add("csz_debugcam_pos `"$($Sc.DebugPos)`"")
  $L.Add("csz_debugcam_ang `"$($Sc.DebugAng)`"")
  (New-WaitLines 40)  | ForEach-Object { $L.Add($_) }

  # --- FPS sampling window START (echo lands in -log output) ---
  $L.Add('echo MATPERF-FPS-START')

  # --- screenshot ---
  $L.Add('screenshot')
  (New-WaitLines 60) | ForEach-Object { $L.Add($_) }

  # NOTE: the wall-clock FPS hold is enforced by the LAUNCHER (it simply lets
  # the process keep rendering for $HoldSeconds before the END marker / quit is
  # reached). We pad with 'wait' lines as a fallback so even if the launcher's
  # timing is off the cfg itself spends real frames before quitting. The END
  # marker + quit are intentionally placed AFTER a large wait padding so the
  # per-second [CSZ:fps] lines accumulate inside the window. For Hold scenarios
  # we pad enough waits to comfortably exceed HoldSeconds of frames even if the
  # launcher kills early; for non-hold scenarios we still emit an END marker.
  # IMPORTANT: do NOT pad thousands of 'wait' lines here. A ~100KB autoexec.cfg
  # (15k+ wait lines) overflows Xash3D's command buffer (cmd_text), so 'exec'
  # truncates and the 'map' line never runs -> engine sits at the menu and the
  # log dies right after "Game started". The wall-clock FPS hold is enforced by
  # the LAUNCHER (Phase 2 keeps the process rendering for $HoldSeconds), so the
  # cfg only needs a small pad. For Hold scenarios we deliberately DO NOT emit
  # 'quit' from the cfg -- the launcher kills the engine after the wall-clock
  # hold, which lets the per-second [CSZ:fps] lines accumulate. Get-FpsStats
  # collects from MATPERF-FPS-START to EOF when no END marker is present.
  if ($Sc.Hold) {
    (New-WaitLines 200) | ForEach-Object { $L.Add($_) }
    # no MATPERF-FPS-END / no quit: launcher holds wall-clock then kills.
  } else {
    (New-WaitLines 120) | ForEach-Object { $L.Add($_) }
    $L.Add('echo MATPERF-FPS-END')
    (New-WaitLines 30) | ForEach-Object { $L.Add($_) }
    $L.Add('quit')
  }

  Set-Content -LiteralPath (Join-Path $CstrikeDir 'autoexec.cfg') -Value $L -Encoding ASCII
}

function Get-NewestPng {
  $shot = Get-ChildItem -LiteralPath $Scrshots -Filter *.png -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime | Select-Object -Last 1
  return $shot
}

function Get-TurbCount([string]$LogPath, [string]$Map) {
  # Scrape "[CSZ:water] Info: built <map>: N turb surfaces" -> int N, or -1.
  if (-not (Test-Path -LiteralPath $LogPath)) { return -1 }
  $count = -1
  $rx = [regex]"\[CSZ:water\].*?built\s+\S*$([regex]::Escape($Map))\S*:\s+(\d+)\s+turb"
  foreach ($line in (Get-Content -LiteralPath $LogPath -ErrorAction SilentlyContinue)) {
    $m = $rx.Match($line)
    if ($m.Success) { $count = [int]$m.Groups[1].Value }
  }
  return $count
}

function Get-FpsStats([string]$LogPath) {
  # Returns @{ samples=int; avg=double; sane=bool }. Scans the window between
  # MATPERF-FPS-START and MATPERF-FPS-END if present, else whole log. "sane"
  # means at least one fps sample in 30..1000 (menu/idle gives ~100000 bug).
  $res = @{ samples = 0; avg = 0.0; sane = $false }
  if (-not (Test-Path -LiteralPath $LogPath)) { return $res }
  $lines = Get-Content -LiteralPath $LogPath -ErrorAction SilentlyContinue
  $inWindow = $false
  $seenStart = $false
  $vals = New-Object System.Collections.Generic.List[double]
  $rxFps = [regex]"\[CSZ:fps\].*?\bfps\s*=\s*([\d.]+)"
  foreach ($ln in $lines) {
    if ($ln -match 'MATPERF-FPS-START') { $inWindow = $true; $seenStart = $true; continue }
    if ($ln -match 'MATPERF-FPS-END')   { $inWindow = $false; continue }
    if ($seenStart -and -not $inWindow) { continue }  # only collect inside window once start seen
    $m = $rxFps.Match($ln)
    if ($m.Success) { $vals.Add([double]$m.Groups[1].Value) }
  }
  # If markers never appeared, fall back to scanning ALL fps lines.
  if (-not $seenStart) {
    foreach ($ln in $lines) {
      $m = $rxFps.Match($ln)
      if ($m.Success) { $vals.Add([double]$m.Groups[1].Value) }
    }
  }
  if ($vals.Count -gt 0) {
    $res.samples = $vals.Count
    $sum = 0.0; foreach ($v in $vals) { $sum += $v }
    $res.avg = [math]::Round($sum / $vals.Count, 1)
    foreach ($v in $vals) { if ($v -ge 30 -and $v -le 1000) { $res.sane = $true; break } }
  }
  return $res
}

function Test-FpsUnlock([string]$LogPath) {
  # Inspect the engine log for the FPS-unlock evidence. Returns a hashtable:
  #   UnlockEndPresent  : bool   MATPERF-FPS-UNLOCK-END echoed
  #   Readback          : string[]  cvar lines printed between READBACK and END
  #   MaxFpsInWindow    : double  highest [CSZ:fps] fps= between START and END/EOF
  #   FpsCapStillActive : bool   max observed fps did NOT clearly exceed FpsUncapMin
  # NEVER reports a capped number (99/100/101/200) as headroom: if the max
  # stays at/below FpsUncapMin we flag the cap as still active.
  $res = @{ UnlockEndPresent = $false; Readback = @(); MaxFpsInWindow = 0.0; FpsCapStillActive = $true }
  if (-not (Test-Path -LiteralPath $LogPath)) { return $res }
  $lines = Get-Content -LiteralPath $LogPath -ErrorAction SilentlyContinue

  # (a) unlock-end marker present?
  foreach ($ln in $lines) { if ($ln -match 'MATPERF-FPS-UNLOCK-END') { $res.UnlockEndPresent = $true; break } }

  # (b) readback lines: everything between MATPERF-FPS-READBACK and the next
  #     MATPERF-FPS-UNLOCK-END that is NOT itself a marker echo.
  $inRb = $false
  $rb = New-Object System.Collections.Generic.List[string]
  foreach ($ln in $lines) {
    if ($ln -match 'MATPERF-FPS-READBACK')   { $inRb = $true;  continue }
    if ($ln -match 'MATPERF-FPS-UNLOCK-END')  { $inRb = $false; continue }
    if ($inRb) {
      $t = $ln.Trim()
      if ($t -ne '' -and $t -notmatch 'MATPERF-') { $rb.Add($t) }
    }
  }
  $res.Readback = $rb.ToArray()

  # (c) max [CSZ:fps] fps= inside the FPS window (START..END/EOF).
  $rxFps = [regex]"\[CSZ:fps\].*?\bfps\s*=\s*([\d.]+)"
  $inWin = $false; $seenStart = $false; $maxFps = 0.0
  foreach ($ln in $lines) {
    if ($ln -match 'MATPERF-FPS-START') { $inWin = $true; $seenStart = $true; continue }
    if ($ln -match 'MATPERF-FPS-END')   { $inWin = $false; continue }
    if ($seenStart -and -not $inWin)    { continue }
    $m = $rxFps.Match($ln)
    if ($m.Success) { $v = [double]$m.Groups[1].Value; if ($v -gt $maxFps) { $maxFps = $v } }
  }
  if (-not $seenStart) {
    foreach ($ln in $lines) {
      $m = $rxFps.Match($ln)
      if ($m.Success) { $v = [double]$m.Groups[1].Value; if ($v -gt $maxFps) { $maxFps = $v } }
    }
  }
  $res.MaxFpsInWindow = [math]::Round($maxFps, 1)
  # Cap defeated ONLY if observed fps clearly exceeds the limiter threshold.
  $res.FpsCapStillActive = -not ($maxFps -gt $FpsUncapMin)
  return $res
}

function Test-DebugCam([string]$LogPath, [hashtable]$Sc) {
  # Validate the debugcam aim took effect. Returns a hashtable:
  #   UnknownCommand : bool   a 'Unknown command:' line names a csz_debugcam cvar
  #   ActivePresent  : bool   a 'debugcam' ... 'active pos=' line is present
  #   ActiveLine     : string the matched active line (for the operator)
  #   Valid          : bool   ActivePresent AND NOT UnknownCommand
  # If invalid the shot is marked INVALID (not silently shipped).
  $res = @{ UnknownCommand = $false; ActivePresent = $false; ActiveLine = ''; Valid = $false }
  if (-not (Test-Path -LiteralPath $LogPath)) { return $res }
  $lines = Get-Content -LiteralPath $LogPath -ErrorAction SilentlyContinue
  foreach ($ln in $lines) {
    if ($ln -match 'Unknown command' -and $ln -match 'csz_debugcam') { $res.UnknownCommand = $true }
    if ($ln -match 'debugcam' -and $ln -match 'active\s+pos=') { $res.ActivePresent = $true; $res.ActiveLine = $ln.Trim() }
  }
  $res.Valid = ($res.ActivePresent -and -not $res.UnknownCommand)
  return $res
}

function Test-CamGate([string]$LogPath) {
  # Parse the renderer camera-legitimacy gate + on-screen water evidence.
  # Returns a hashtable:
  #   Present        : bool   a '[CSZ:camgate] ... legit=' line was found
  #   Pos / Ang      : string the parsed eye pos / ang
  #   Contents       : string EMPTY/SOLID/SKY/WATER/...
  #   InsideWorld    : int    0/1 (-1 = not parsed)
  #   Legit          : int    0/1 (-1 = not parsed)
  #   AutoFrameFailed: bool    an 'auto-frame FAILED' or 'NO WATER IN MAP' line
  #   VisibleFaces   : int    [CSZ:water] visible_water_faces=N (-1 = not parsed)
  #   VisibleTotal   : int    the 'of M' total (-1 = not parsed)
  $res = @{ Present=$false; Pos=''; Ang=''; Contents=''; InsideWorld=-1; Legit=-1;
            AutoFrameFailed=$false; VisibleFaces=-1; VisibleTotal=-1 }
  if (-not (Test-Path -LiteralPath $LogPath)) { return $res }
  $lines = Get-Content -LiteralPath $LogPath -ErrorAction SilentlyContinue

  $rxGate = [regex]'\[CSZ:camgate\].*?pos=\(([^)]*)\)\s+ang=\(([^)]*)\)\s+contents=(\S+)\s+inside_world=(\d).*?legit=(\d)'
  $rxVis  = [regex]'\[CSZ:water\].*?visible_water_faces=(\d+)\s+of\s+(\d+)'
  foreach ($ln in $lines) {
    if ($ln -match 'auto-frame FAILED' -or $ln -match 'NO WATER IN MAP') { $res.AutoFrameFailed = $true }
    $m = $rxGate.Match($ln)
    if ($m.Success) {
      $res.Present     = $true
      $res.Pos         = $m.Groups[1].Value.Trim()
      $res.Ang         = $m.Groups[2].Value.Trim()
      $res.Contents    = $m.Groups[3].Value
      $res.InsideWorld = [int]$m.Groups[4].Value
      $res.Legit       = [int]$m.Groups[5].Value
    }
    $v = $rxVis.Match($ln)
    if ($v.Success) {
      $res.VisibleFaces = [int]$v.Groups[1].Value
      $res.VisibleTotal = [int]$v.Groups[2].Value
    }
  }
  return $res
}

function Write-Blocker([string]$Reason) {
  $blk = Join-Path $EvidenceDir 'BLOCKER.txt'
  $msg = @(
    "R2 CAPTURE BLOCKER",
    "time: $((Get-Date).ToString('o'))",
    "reason: $Reason",
    "RunDir: $RunDir",
    "Exe: $Exe"
  )
  Set-Content -LiteralPath $blk -Value $msg -Encoding ASCII
  Write-Host "BLOCKER written: $blk -- $Reason"
}

# ----------------------------------------------------------------------------
#  Per-scenario capture
# ----------------------------------------------------------------------------
function Invoke-Scenario([hashtable]$Sc, [System.Diagnostics.Stopwatch]$ProgressSw) {
  Write-Host ""
  Write-Host ("=" * 70)
  Write-Host "SCENARIO $($Sc.Name)  map=$($Sc.Map)  hold=$($Sc.Hold)"
  Write-Host ("=" * 70)

  $result = [ordered]@{
    name             = $Sc.Name
    runid            = $RunId
    map              = $Sc.Map
    png              = $null
    out_png          = $null
    log              = $null
    turb_count       = -1
    fps_samples      = 0
    fps_avg          = 0.0
    fps_sane         = $false
    # FPS-unlock inspection
    unlock_end       = $false
    fps_readback     = @()
    fps_max_observed = 0.0
    fps_cap_active   = $true
    # debugcam inspection
    debugcam_valid   = $false
    debugcam_active  = ''
    # camera-legitimacy gate + on-screen water evidence
    camgate_present       = $false
    camgate_pos           = ''
    camgate_ang           = ''
    camgate_contents      = ''
    camgate_inside_world  = -1
    camgate_legit         = -1
    camgate_autoframe_failed = $false
    visible_water_faces   = -1
    visible_water_total   = -1
    # classification + outcome
    classification   = ''
    success          = $false
    note             = ''
  }

  # Clean slate.
  Stop-RunDirEngines
  New-Item -ItemType Directory -Force $Scrshots | Out-Null
  Get-ChildItem -LiteralPath $Scrshots -Filter *.png -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
  if (Test-Path -LiteralPath $EngineLog) { Remove-Item -LiteralPath $EngineLog -Force -ErrorAction SilentlyContinue }

  Write-ScenarioAutoexec -Sc $Sc -HoldSeconds $FpsHoldSeconds

  # Monitor runs at 500 Hz (RTX 5070 Ti native 1920x1080@500). The steady-state
  # ~99fps pin is therefore NOT vsync (vsync would pin near 500/250/166/125, not
  # 99) -- it is a SOFTWARE fps cap being re-applied AFTER the autoexec block.
  # Fix: pass the fps cvars on the COMMAND LINE (parsed late, authoritative) so
  # the cap is lifted before/around the config re-exec, AND re-issue fps_override
  # LAST in the autoexec. gl_vsync 0 belt-and-suspenders.
  # Windowed (default) vs exclusive fullscreen (-Fullscreen). Fullscreen lets
  # the operator test whether the ~101fps pin is a desktop-compositor vsync cap:
  # windowed shares the compositor's vsync; exclusive fullscreen + gl_vsync 0
  # bypasses it and should break a compositor-imposed cap.
  if ($Fullscreen) {
    $displayArgs = @('-fullscreen','-width','1280','-height','720')
    Write-Host "  launch mode: FULLSCREEN (compositor-vsync test)"
  } else {
    $displayArgs = @('-windowed','-width','1280','-height','720','-noborder')
    Write-Host "  launch mode: windowed -noborder"
  }
  $launchArgs = @(
    '-log','-game','cstrike'
  ) + $displayArgs + @(
    '+port',[string]$Port,
    '+gl_vsync','0','+fps_override','1','+fps_max','400'
  )
  Write-Host "launching: $Exe $($launchArgs -join ' ')"
  $proc = Start-Process -FilePath $Exe -ArgumentList $launchArgs -WorkingDirectory $ClientDir -WindowStyle Hidden -PassThru

  # --- Phase 1: wait for the screenshot to appear (deadline) ---
  $shotDeadline = (Get-Date).AddSeconds($ScreenshotDeadlineSeconds)
  $shot = $null
  while ((Get-Date) -lt $shotDeadline) {
    Start-Sleep -Milliseconds 1500

    # 3-minute no-progress rule: process must be alive until we have a png.
    if ($ProgressSw.Elapsed.TotalSeconds -gt $ProgressGraceSeconds) {
      $existing = Get-NewestPng
      $procAlive = $false
      try { $procAlive = -not $proc.HasExited } catch {}
      if (-not $existing -and -not $procAlive) {
        $result.note = 'no progress: engine dead and no screenshot within 180s'
        return $result   # caller treats null-png as failure; abort handled by caller
      }
    }

    $shot = Get-NewestPng
    if ($shot) { Start-Sleep -Milliseconds 1200; break }

    # If the process died before producing a shot, stop waiting early.
    $dead = $false
    try { $dead = $proc.HasExited } catch { $dead = $true }
    if ($dead) {
      Start-Sleep -Milliseconds 800
      $shot = Get-NewestPng
      break
    }
  }

  if (-not $shot) {
    $result.note = 'screenshot never appeared within deadline'
    Write-Host "  WARN: no screenshot. killing engine."
    Stop-RunDirEngines
  } else {
    Write-Host "  screenshot detected: $($shot.Name)"

    # --- Phase 2: FPS wall-clock hold (heavy scenarios only) ---
    if ($Sc.Hold) {
      Write-Host "  holding live scene ~$FpsHoldSeconds s for FPS samples..."
      $holdEnd = (Get-Date).AddSeconds($FpsHoldSeconds)
      while ((Get-Date) -lt $holdEnd) {
        $dead = $false
        try { $dead = $proc.HasExited } catch { $dead = $true }
        if ($dead) { Write-Host "  engine exited during hold (cfg pad ran out / quit)"; break }
        Start-Sleep -Milliseconds 2000
      }
    }
    # Done: kill the engine (the cfg also issues quit, but ensure clean exit).
    Stop-RunDirEngines
  }

  Start-Sleep -Milliseconds 800

  # --- Collect evidence (UNIQUE filenames keyed on RunId -- never overwrite) ---
  $finalShot = Get-NewestPng
  if ($finalShot) {
    # raw engine shot, run-tagged so re-runs don't clobber:
    $rawDst = Join-Path $EvidenceDir "$($Sc.Name)_$RunId.png"
    Copy-Item -LiteralPath $finalShot.FullName -Destination $rawDst -Force
    $result.png = $rawDst
    Write-Host "  png (raw) -> $rawDst"
    # deliverable name (fixed, per the supervisor's required 6 PNGs):
    if ($Sc.OutPng) {
      $outDst = Join-Path $EvidenceDir $Sc.OutPng
      Copy-Item -LiteralPath $finalShot.FullName -Destination $outDst -Force
      $result.out_png = $outDst
      Write-Host "  png (deliverable) -> $outDst"
    }
  }
  if (Test-Path -LiteralPath $EngineLog) {
    $logDst = Join-Path $EvidenceDir "engine_$($Sc.Name)_$RunId.log"
    Copy-Item -LiteralPath $EngineLog -Destination $logDst -Force
    $result.log = $logDst
  }

  # --- Scrape metrics from the copied log ---
  if ($result.log) {
    if ($Sc.ExpectWater) {
      $result.turb_count = Get-TurbCount -LogPath $result.log -Map $Sc.Map
      Write-Host "  turb surfaces ($($Sc.Map)): $($result.turb_count)"
    }
    $fps = Get-FpsStats -LogPath $result.log
    $result.fps_samples = $fps.samples
    $result.fps_avg     = $fps.avg
    $result.fps_sane    = $fps.sane
    Write-Host "  fps: samples=$($fps.samples) avg=$($fps.avg) sane=$($fps.sane)"

    # --- FPS-unlock inspection (markers + readback + max observed) -----------
    $unlock = Test-FpsUnlock -LogPath $result.log
    $result.unlock_end       = $unlock.UnlockEndPresent
    $result.fps_readback     = $unlock.Readback
    $result.fps_max_observed = $unlock.MaxFpsInWindow
    $result.fps_cap_active   = $unlock.FpsCapStillActive
    Write-Host "  fps-unlock: end_marker=$($unlock.UnlockEndPresent) max_observed=$($unlock.MaxFpsInWindow) cap_still_active=$($unlock.FpsCapStillActive)"
    if ($unlock.Readback.Count -gt 0) {
      Write-Host "  fps-readback cvar values:"
      foreach ($rb in $unlock.Readback) { Write-Host "      $rb" }
    } else {
      Write-Host "  fps-readback: (no cvar lines captured between READBACK and UNLOCK-END)"
    }

    # --- debugcam inspection (Unknown command abort + active line) -----------
    $cam = Test-DebugCam -LogPath $result.log -Sc $Sc
    $result.debugcam_valid  = $cam.Valid
    $result.debugcam_active = $cam.ActiveLine
    if ($cam.UnknownCommand) { Write-Host "  debugcam: INVALID -- 'Unknown command' for csz_debugcam cvars" }
    elseif (-not $cam.ActivePresent) { Write-Host "  debugcam: INVALID -- no 'debugcam ... active pos=' line found" }
    else { Write-Host "  debugcam: OK -- $($cam.ActiveLine)" }

    # --- camera-legitimacy gate (eye contents + world bounds + auto-frame) ----
    $gate = Test-CamGate -LogPath $result.log
    $result.camgate_present       = $gate.Present
    $result.camgate_pos           = $gate.Pos
    $result.camgate_ang           = $gate.Ang
    $result.camgate_contents      = $gate.Contents
    $result.camgate_inside_world  = $gate.InsideWorld
    $result.camgate_legit         = $gate.Legit
    $result.camgate_autoframe_failed = $gate.AutoFrameFailed
    $result.visible_water_faces   = $gate.VisibleFaces
    $result.visible_water_total   = $gate.VisibleTotal
    if ($gate.Present) {
      Write-Host "  camgate: contents=$($gate.Contents) inside_world=$($gate.InsideWorld) legit=$($gate.Legit) (pos=$($gate.Pos))"
    } else {
      Write-Host "  camgate: no [CSZ:camgate] line found"
    }
    if ($Sc.ExpectWater) {
      Write-Host "  visible_water_faces: $($gate.VisibleFaces) of $($gate.VisibleTotal)"
    }
  }

  # --- Success gate + classification ---------------------------------------
  #   * a png was produced AND
  #   * the spawn looked sane (>=1 fps sample in 30..1000) -- menu-block guard.
  #   * water-expecting scenarios: turb_count must be > 0.
  #   * debugcam must be VALID (active line present, no Unknown command), else
  #     the shot is INVALID and must NOT be silently shipped.
  # Classification is reported so a capped FPS reading is never sold as headroom.
  $ok = $true
  $notes = @()
  if (-not $result.png)          { $ok = $false; $notes += 'no png' }
  if (-not $result.fps_sane)     { $ok = $false; $notes += 'no sane fps sample (possible menu block / no world)' }
  if ($Sc.ExpectWater -and $result.turb_count -le 0) { $ok = $false; $notes += "turb_count<=0 on $($Sc.Map)" }
  if (-not $result.debugcam_valid) { $ok = $false; $notes += 'debugcam INVALID (no active line or Unknown command) -- shot not trustworthy' }

  # --- camera-legitimacy gate (ADD, never relax) ----------------------------
  #   * camgate line must be present (renderer evaluated the eye) AND legit=1,
  #     else the eye is in SOLID/SKY or outside the world AABB (the V5 over-the-
  #     edge skybox failure) -- mark INVALID, never silently ship.
  if (-not $result.camgate_present) {
    $ok = $false; $notes += 'camgate MISSING ([CSZ:camgate] line absent) -- eye legitimacy unverified'
  } elseif ($result.camgate_legit -ne 1) {
    $ok = $false; $notes += "camgate ILLEGITIMATE (contents=$($result.camgate_contents) inside_world=$($result.camgate_inside_world) legit=$($result.camgate_legit))"
  }
  if ($result.camgate_autoframe_failed) {
    $ok = $false; $notes += 'auto-frame FAILED / NO WATER IN MAP -- no legit water pose found'
  }
  # Water scenarios additionally require REAL on-screen water faces in frustum.
  if ($Sc.ExpectWater) {
    if ($result.visible_water_faces -lt 0) {
      $ok = $false; $notes += 'visible_water_faces MISSING -- on-screen water not proven'
    } elseif ($result.visible_water_faces -eq 0) {
      $ok = $false; $notes += 'visible_water_faces=0 -- water exists but none in the shot'
    }
  }

  # Classification (independent of pass/fail; surfaces the cap honestly).
  if (-not $result.png)                 { $result.classification = 'NO_SHOT' }
  elseif (-not $result.debugcam_valid)  { $result.classification = 'DEBUGCAM_INVALID' }
  elseif ((-not $result.camgate_present) -or ($result.camgate_legit -ne 1) -or $result.camgate_autoframe_failed) { $result.classification = 'CAMGATE_ILLEGITIMATE' }
  elseif ($Sc.ExpectWater -and ($result.visible_water_faces -le 0)) { $result.classification = 'NO_WATER_ON_SCREEN' }
  elseif ($result.fps_cap_active)       { $result.classification = 'FPS_CAP_STILL_ACTIVE' }
  else                                  { $result.classification = 'OK' }

  if ($notes.Count -gt 0) { $result.note = ($notes -join '; ') }
  $result.success = $ok
  if ($ok) { Write-Host "  RESULT: SUCCESS  [$($result.classification)]" } else { Write-Host "  RESULT: FAIL ($($result.note))  [$($result.classification)]" }

  return $result
}

# ============================================================================
#  MAIN
# ============================================================================
New-Item -ItemType Directory -Force $EvidenceDir | Out-Null

# Resolve the scenario subset.
$selected = $AllScenarios
if ($Scenarios -and $Scenarios.Count -gt 0) {
  $selected = $AllScenarios | Where-Object { $Scenarios -contains $_.Name }
  $missing = $Scenarios | Where-Object { ($AllScenarios | ForEach-Object { $_.Name }) -notcontains $_ }
  if ($missing) { Write-Warning "unknown scenario name(s): $($missing -join ', ')" }
}
if (-not $selected -or $selected.Count -eq 0) {
  Write-Blocker "no matching scenarios selected"
  throw "no scenarios to run"
}

Write-Host "R2 weather/water capture"
Write-Host "  RunDir       : $RunDir"
Write-Host "  DllSource    : $DllSource"
Write-Host "  EvidenceDir  : $EvidenceDir"
Write-Host "  Port         : $Port"
Write-Host "  AcquireLock  : $AcquireLock (owner=$Owner)"
Write-Host "  FpsHoldSecs  : $FpsHoldSeconds"
Write-Host "  RunId        : $RunId"
Write-Host "  Fullscreen   : $([bool]$Fullscreen)"
Write-Host "  SkyPhase     : $SkyPhase (0.5=midnight, 1.0=daylight; tune 0.88..1.0)"
Write-Host "  FPS unlock   : fps_max=$FpsMax host_maxfps=$HostMaxFps updaterate=$UpdateRate cmdrate=$CmdRate sys_ticrate=$SysTicRate (uncap_min=$FpsUncapMin)"
Write-Host "  scenarios    : $(( $selected | ForEach-Object { $_.Name }) -join ', ')"

# Pre-flight existence checks (fail fast with a BLOCKER, before lock).
if (-not (Test-Path -LiteralPath $Exe))       { Write-Blocker "xash3d.exe not found: $Exe"; throw "missing exe" }
if (-not (Test-Path -LiteralPath $DllSource)) { Write-Blocker "DllSource not found: $DllSource"; throw "missing dll" }

# Stopwatch governs the 3-minute no-progress rule (starts at lock acquisition).
$progressSw = [System.Diagnostics.Stopwatch]::StartNew()

$lockHeld = $false
$results = @()
try {
  if ($AcquireLock) {
    Write-Host "acquiring engine lock..."
    & powershell -NoProfile -ExecutionPolicy Bypass -File $AcquireScript -Owner $Owner -TimeoutSeconds 7200
    if ($LASTEXITCODE -ne 0) { Write-Blocker "failed to acquire engine lock"; throw "lock acquire failed" }
    $lockHeld = $true
    Write-Host "lock acquired."
  } else {
    Write-Host "AcquireLock=`$false -- assuming caller holds the lock."
  }

  # Restart the progress stopwatch now that we actually hold the engine.
  $progressSw.Restart()

  # --- Overlay fresh WIP dll into the packaged run dir ---
  Stop-RunDirEngines
  $dllParent = Split-Path -Parent $DllTarget
  if (-not (Test-Path -LiteralPath $dllParent)) {
    Write-Blocker "dll target dir missing: $dllParent"
    throw "missing cl_dlls dir"
  }
  Copy-Item -LiteralPath $DllSource -Destination $DllTarget -Force
  $srcHash = (Get-FileHash -LiteralPath $DllSource -Algorithm SHA256).Hash
  $dstHash = (Get-FileHash -LiteralPath $DllTarget -Algorithm SHA256).Hash
  if ($srcHash -ne $dstHash) { Write-Blocker "dll overlay hash mismatch"; throw "dll copy mismatch" }
  Write-Host "overlaid client.dll  sha256=$dstHash"

  Add-ConsistencyOff

  # --- Run scenarios serially ---
  foreach ($sc in $selected) {
    $r = Invoke-Scenario -Sc $sc -ProgressSw $progressSw
    $results += [pscustomobject]$r

    # Hard 3-minute rule: if after the FIRST scenario we still produced nothing
    # at all AND the elapsed time exceeded the grace, abort the whole run.
    if (-not $r.success -and $r.note -like 'no progress*') {
      Write-Blocker "3-minute no-progress rule tripped on $($sc.Name): $($r.note)"
      break
    }
  }
}
finally {
  # ALWAYS release the lock (if we took it) and never leave an engine running.
  Stop-RunDirEngines
  if ($lockHeld) {
    Write-Host "releasing engine lock..."
    try {
      & powershell -NoProfile -ExecutionPolicy Bypass -File $ReleaseScript -Owner $Owner
    } catch {
      Write-Warning "lock release failed: $($_.Exception.Message)"
    }
  }
}

# --- Write results JSON (run-tagged so re-runs never overwrite) ---
$resultsPath = Join-Path $EvidenceDir "capture_results_$RunId.json"
$results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $resultsPath -Encoding UTF8
Write-Host ""
Write-Host "results JSON -> $resultsPath"

# --- SUMMARY block ---
Write-Host ""
Write-Host ("#" * 78)
Write-Host "# R2 CAPTURE SUMMARY  (runid=$RunId)"
Write-Host ("#" * 78)
$failCount    = 0
$capActiveAny = $false
foreach ($r in $results) {
  $state = 'FAIL'
  if ($r.success) { $state = 'SUCCESS' } else { $failCount++ }
  if ($r.fps_cap_active) { $capActiveAny = $true }
  $line = ("{0,-26} {1,-8} {2,-22} turb={3,-5} fps_max_obs={4,-7} cap_active={5,-6} samples={6,-4}" -f `
            $r.name, $state, $r.classification, $r.turb_count, $r.fps_max_observed, $r.fps_cap_active, $r.fps_samples)
  Write-Host $line
  Write-Host ("    deliverable: {0}" -f $r.out_png)
  if ($r.note) { Write-Host ("    note: {0}" -f $r.note) }
}
Write-Host ("#" * 78)
Write-Host ("# {0}/{1} succeeded" -f ($results.Count - $failCount), $results.Count)
if ($capActiveAny) {
  Write-Host "# WARNING: FPS_CAP_STILL_ACTIVE on >=1 scenario -- observed fps did NOT exceed"
  Write-Host ("#          {0}. Do NOT report these as headroom. Try -Fullscreen and/or retune" -f $FpsUncapMin)
  Write-Host "#          -FpsMax / -UpdateRate, then re-run with a fresh -RunId."
}
Write-Host ("#" * 78)

if ($failCount -gt 0) { exit 1 }
exit 0
