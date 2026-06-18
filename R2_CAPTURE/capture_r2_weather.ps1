<#
================================================================================
 capture_r2_weather.ps1
================================================================================
 Non-interactive screenshot + FPS capture for the CSOZ Xash3D weather/water
 renderer (R2 round).

 PURPOSE
   For each scenario in the matrix it:
     1. Overlays the fresh WIP client.dll into the (already packaged) run dir.
     2. Generates a per-scenario autoexec.cfg that loads the map, forces the
        player into a team/class (bulletproof anti-menu sequence), positions a
        noclip camera, sets the weather/water cvars, takes a screenshot, then
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
  [string]   $EvidenceDir   = 'D:\csoz-wt\weather\R2_EVIDENCE',
  [int]      $Port          = 27022,
  [string]   $Owner         = 'weather',
  [bool]     $AcquireLock   = $true,
  [int]      $FpsHoldSeconds= 65,
  [string[]] $Scenarios     = @()
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
#  SCENARIO MATRIX  (8 shots). Names == required output png filenames.
#  Camera coords: de_aztec water-channel coords reuse the known-good aztec
#  position from capture_serial.ps1 (setpos 320 -640 240 / setang 25 90 0),
#  which framed the aztec scene there. de_dust2 coords reuse the known-good
#  dust2 spot from capture_serial.ps1 blackfog scenarios (setpos -700 -1550 110).
#  Variants (ground / cover / far) nudge pitch/height only. EDIT HERE if a
#  shot frames poorly -- coords are intentionally collected in one table.
#
#  Per-scenario fields:
#    Name      output filename stem
#    Map       bsp to load
#    Pos       "setpos X Y Z"
#    Ang       "setang PITCH YAW ROLL"   (positive pitch looks DOWN)
#    Cmds      effect cvars issued just before the screenshot
#    Hold      $true => run the FpsHoldSeconds wall-clock FPS window (heavy shots)
#    ExpectWater $true => engine.log must show N>0 turb surfaces for this map
# ============================================================================
$AllScenarios = @(
  @{ Name='rain_far_de_dust2';         Map='de_dust2'; Pos='setpos -700 -1550 140'; Ang='setang 2 90 0';
     Cmds=@('csz_water 1','csz_weather_quality 2','csz_weather_intensity 1.0','csz_weather 1');
     Hold=$true;  ExpectWater=$false },

  @{ Name='rain_wet_ground_de_dust2';  Map='de_dust2'; Pos='setpos -700 -1550 90';  Ang='setang 35 90 0';
     Cmds=@('csz_water 1','csz_weather_quality 2','csz_weather_intensity 1.0','csz_weather 1');
     Hold=$false; ExpectWater=$false },

  @{ Name='snow_fall_de_dust2';        Map='de_dust2'; Pos='setpos -700 -1550 140'; Ang='setang 5 90 0';
     Cmds=@('csz_water 1','csz_weather_quality 2','csz_weather_intensity 1.0','csz_weather 2');
     Hold=$false; ExpectWater=$false },

  @{ Name='snow_cover_de_dust2';       Map='de_dust2'; Pos='setpos -700 -1550 90';  Ang='setang 40 90 0';
     Cmds=@('csz_water 1','csz_weather_quality 2','csz_weather_intensity 1.0','csz_weather 2');
     Hold=$false; ExpectWater=$false },

  @{ Name='weather_off_baseline_de_dust2'; Map='de_dust2'; Pos='setpos -700 -1550 140'; Ang='setang 5 90 0';
     Cmds=@('csz_water 1','csz_weather 0');
     Hold=$false; ExpectWater=$false },

  @{ Name='water_off_de_aztec';        Map='de_aztec'; Pos='setpos 320 -640 240';   Ang='setang 25 90 0';
     Cmds=@('csz_weather 0','csz_water 0');
     Hold=$false; ExpectWater=$true },   # turb surfaces still BUILT (count logged); custom pass skipped

  @{ Name='water_on_de_aztec';         Map='de_aztec'; Pos='setpos 320 -640 240';   Ang='setang 25 90 0';
     Cmds=@('csz_weather 0','csz_water 1');
     Hold=$true;  ExpectWater=$true },

  @{ Name='water_rain_de_aztec';       Map='de_aztec'; Pos='setpos 320 -640 240';   Ang='setang 25 90 0';
     Cmds=@('csz_water 1','csz_weather_quality 2','csz_weather_intensity 1.0','csz_weather 1');
     Hold=$true;  ExpectWater=$true }
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

function Write-ScenarioAutoexec([hashtable]$Sc, [int]$HoldSeconds) {
  # Build the autoexec.cfg for one scenario. This is the bulletproof anti-menu
  # + spawn + frame + FPS-window sequence.
  $L = New-Object System.Collections.Generic.List[string]

  $L.Add("// GENERATED by capture_r2_weather.ps1 for $($Sc.Name)")
  # --- pre-map global setup ---
  $L.Add('setinfo _vgui_menus 0')      # kill VGUI team/class menus BEFORE map
  $L.Add('developer 1')                # REQUIRED: enables [CSZ:fps] Dev log lines
  $L.Add('con_notifytime 0')
  $L.Add('fps_max 200')                # cap (not a fake) -- never host_framerate
  $L.Add('sv_cheats 1')                # noclip/setpos need cheats
  $L.Add('sv_lan 1')
  $L.Add('mp_consistency 0')
  $L.Add('sv_consistency 0')
  $L.Add('maxplayers 4')
  $L.Add('mp_freezetime 0')
  $L.Add('bot_quota 0')
  $L.Add('bot_join_after_player 0')
  $L.Add('csz_renderer 1')             # ensure custom renderer takeover
  $L.Add('csz_sky_phase 0.5')          # midday-ish, stable lighting
  # --- load map ---
  $L.Add("map $($Sc.Map)")
  (New-WaitLines 650) | ForEach-Object { $L.Add($_) }   # world load (proven count)

  # --- bulletproof get-into-map: belt + suspenders ---
  $L.Add('setinfo _vgui_menus 0')      # re-assert after load
  $L.Add('jointeam 2')                 # CT
  $L.Add('joinclass 1')                # rifleman
  # text-menu fallbacks in case jointeam/joinclass aren't bound this build:
  $L.Add('menuselect 2')               # team menu: CT
  $L.Add('menuselect 1')               # class menu: first class
  $L.Add('chooseteam')
  $L.Add('menuselect 2')
  $L.Add('menuselect 1')
  (New-WaitLines 260) | ForEach-Object { $L.Add($_) }   # class spawn

  # --- hide HUD / crosshair / on-screen fps so they don't overlap the shot ---
  $L.Add('hud_draw 0')
  $L.Add('cl_drawhud 0')
  $L.Add('crosshair 0')
  $L.Add('csz_showfps 0')

  # --- camera + effect cvars ---
  $L.Add('noclip')
  $L.Add($Sc.Pos)
  $L.Add($Sc.Ang)
  foreach ($c in $Sc.Cmds) { $L.Add($c) }
  # re-assert position/angle AFTER cvars (noclip drift / spawn shove):
  $L.Add($Sc.Pos)
  $L.Add($Sc.Ang)
  (New-WaitLines 120) | ForEach-Object { $L.Add($_) }   # let effects settle
  $L.Add($Sc.Pos)
  $L.Add($Sc.Ang)
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
    name        = $Sc.Name
    map         = $Sc.Map
    png         = $null
    log         = $null
    turb_count  = -1
    fps_samples = 0
    fps_avg     = 0.0
    fps_sane    = $false
    success     = $false
    note        = ''
  }

  # Clean slate.
  Stop-RunDirEngines
  New-Item -ItemType Directory -Force $Scrshots | Out-Null
  Get-ChildItem -LiteralPath $Scrshots -Filter *.png -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
  if (Test-Path -LiteralPath $EngineLog) { Remove-Item -LiteralPath $EngineLog -Force -ErrorAction SilentlyContinue }

  Write-ScenarioAutoexec -Sc $Sc -HoldSeconds $FpsHoldSeconds

  $launchArgs = @(
    '-log','-game','cstrike',
    '-windowed','-width','1280','-height','720',
    '-noborder',
    '+port',[string]$Port
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

  # --- Collect evidence ---
  $finalShot = Get-NewestPng
  if ($finalShot) {
    $dst = Join-Path $EvidenceDir "$($Sc.Name).png"
    Copy-Item -LiteralPath $finalShot.FullName -Destination $dst -Force
    $result.png = $dst
    Write-Host "  png -> $dst"
  }
  if (Test-Path -LiteralPath $EngineLog) {
    $logDst = Join-Path $EvidenceDir "engine_$($Sc.Name).log"
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
  }

  # --- Success gate ---
  #   * a png was produced AND
  #   * the spawn looked sane (at least one fps sample in 30..1000) -- this is
  #     the menu-block guard; menu/idle gives the absurd-fps bug or no fps lines.
  #   * for water-expecting scenarios, turb_count must be > 0.
  $ok = $true
  $notes = @()
  if (-not $result.png)       { $ok = $false; $notes += 'no png' }
  if (-not $result.fps_sane)  { $ok = $false; $notes += 'no sane fps sample (possible menu block / no world)' }
  if ($Sc.ExpectWater -and $result.turb_count -le 0) { $ok = $false; $notes += "turb_count<=0 on $($Sc.Map)" }
  if ($notes.Count -gt 0) { $result.note = ($notes -join '; ') }
  $result.success = $ok
  if ($ok) { Write-Host "  RESULT: SUCCESS" } else { Write-Host "  RESULT: FAIL ($($result.note))" }

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

# --- Write results JSON ---
$resultsPath = Join-Path $EvidenceDir 'capture_results.json'
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultsPath -Encoding UTF8
Write-Host ""
Write-Host "results JSON -> $resultsPath"

# --- SUMMARY block ---
Write-Host ""
Write-Host ("#" * 70)
Write-Host "# R2 CAPTURE SUMMARY"
Write-Host ("#" * 70)
$failCount = 0
foreach ($r in $results) {
  $state = 'FAIL'
  if ($r.success) { $state = 'SUCCESS' } else { $failCount++ }
  $turb = $r.turb_count
  $line = ("{0,-34} {1,-8} turb={2,-5} fps_avg={3,-7} samples={4,-4} png={5}" -f `
            $r.name, $state, $turb, $r.fps_avg, $r.fps_samples, $r.png)
  Write-Host $line
  if ($r.note) { Write-Host ("    note: {0}" -f $r.note) }
}
Write-Host ("#" * 70)
Write-Host ("# {0}/{1} succeeded" -f ($results.Count - $failCount), $results.Count)
Write-Host ("#" * 70)

if ($failCount -gt 0) { exit 1 }
exit 0
