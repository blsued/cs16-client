<#
.SYNOPSIS
    CSOZ material/perf capture harness: (optionally deploy) -> launch headless
    dedicated server + offscreen client -> client self-drives via cfg and quits
    -> collect engine.log + screenshots into OutDir -> run parse_fps.py gate.

.DESCRIPTION
    The client cfg (capture_session.cfg) must end in `quit`. This script polls
    for the client process to exit, then kills any lingering xash3d under the
    run dir. It NEVER moves or force-deletes official run-dir content; it only
    COPIES the produced log/screenshots into OutDir.

.PARAMETER RunDir
    Deployed run directory (contains \server and \client). Default the r1 dir.

.PARAMETER Port
    UDP port the dedicated server listens on / client connects to.

.PARAMETER ClientCfg
    Path to the client capture cfg (exec'd by the client). Copied into
    <RunDir>\client\ so `+exec <name>` finds it.

.PARAMETER ServerCfg
    Path to the dedicated server cfg (exec'd by the server). Copied into
    <RunDir>\server\.

.PARAMETER OutDir
    Output dir for collected engine.log, *.png, fps.json. Created if missing.

.PARAMETER Deploy
    If set, runs package.ps1 first to (re)deploy RunDir before launching.

.PARAMETER BotQuota
    Bot quota passed to package.ps1 when -Deploy.

.PARAMETER AssetSource
    Asset source passed to package.ps1 when -Deploy.

.EXAMPLE
    .\run_capture.ps1 -RunDir D:\csoz-run-r1-materialperf -Port 27023 `
        -ClientCfg .\capture_session.cfg -ServerCfg .\server.cfg `
        -OutDir D:\csoz-wt\materialperf\_scratch\judge-test\cap1
#>
[CmdletBinding()]
param(
    [string]$RunDir = "D:\csoz-run-r1-materialperf",
    [int]$Port = 27023,
    [Parameter(Mandatory = $true)][string]$ClientCfg,
    [Parameter(Mandatory = $true)][string]$ServerCfg,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [switch]$Deploy,
    [int]$BotQuota = 31,
    [string]$AssetSource = "E:\CSSME_Evolution_Build3601(HLND)",
    [int]$TimeoutSec = 300
)

$ErrorActionPreference = "Stop"

function Step($msg) { Write-Host "[run_capture] $msg" -ForegroundColor Cyan }
function Warn($msg) { Write-Host "[run_capture] WARN: $msg" -ForegroundColor Yellow }
function Die($msg)  { Write-Host "[run_capture] ERROR: $msg" -ForegroundColor Red; exit 2 }

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ParseFps  = Join-Path $ScriptDir "parse_fps.py"

# --- 0. deploy (optional, one-way) ----------------------------------------
if ($Deploy) {
    Step "Deploying run dir via package.ps1 -> $RunDir (BotQuota=$BotQuota)"
    $pkg = "D:\csoz\scripts\package.ps1"
    if (-not (Test-Path $pkg)) { Die "package.ps1 not found at $pkg" }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $pkg `
        -AssetSource $AssetSource -Target $RunDir -BotQuota $BotQuota
    if ($LASTEXITCODE -ne 0) { Die "package.ps1 failed (exit $LASTEXITCODE)" }
}

# --- 1. preflight ---------------------------------------------------------
if (-not (Test-Path $RunDir)) {
    Die "RunDir '$RunDir' does not exist. Re-run with -Deploy to build it first."
}
$ServerDir = Join-Path $RunDir "server"
$ClientDir = Join-Path $RunDir "client"
$ServerExe = Join-Path $ServerDir "xash3d.exe"
$ClientExe = Join-Path $ClientDir "xash3d.exe"
foreach ($p in @($ServerDir, $ClientDir, $ServerExe, $ClientExe)) {
    if (-not (Test-Path $p)) { Die "expected path missing: $p" }
}
if (-not (Test-Path $ClientCfg)) { Die "ClientCfg not found: $ClientCfg" }
if (-not (Test-Path $ServerCfg)) { Die "ServerCfg not found: $ServerCfg" }
if (-not (Test-Path $ParseFps))  { Die "parse_fps.py not found: $ParseFps" }

if (-not (Test-Path $OutDir)) {
    Step "Creating OutDir $OutDir"
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

# --- 2. stage cfgs into the run dir ---------------------------------------
$ClientCfgName = Split-Path -Leaf $ClientCfg
$ServerCfgName = Split-Path -Leaf $ServerCfg
Step "Staging client cfg '$ClientCfgName' -> $ClientDir"
Copy-Item -Path $ClientCfg -Destination (Join-Path $ClientDir $ClientCfgName) -Force
Step "Staging server cfg '$ServerCfgName' -> $ServerDir"
Copy-Item -Path $ServerCfg -Destination (Join-Path $ServerDir $ServerCfgName) -Force

# --- 3. clear any stale client engine.log so we capture a fresh window ----
$EngineLog = Join-Path $ClientDir "engine.log"
if (Test-Path $EngineLog) {
    Step "Removing stale engine.log (fresh capture)"
    Remove-Item $EngineLog -Force
}

# --- 4. launch dedicated server (hidden) ----------------------------------
Step "Launching dedicated server on port $Port (map de_dust2)"
$serverArgs = @(
    "-dedicated", "-log", "-port", "$Port",
    "+map", "de_dust2", "+exec", $ServerCfgName
)
$server = Start-Process -FilePath $ServerExe -ArgumentList $serverArgs `
    -WorkingDirectory $ServerDir -WindowStyle Hidden -PassThru
Step "Server PID $($server.Id); waiting 8s for it to come up"
Start-Sleep -Seconds 8

if ($server.HasExited) {
    Die "server exited prematurely (exit $($server.ExitCode)) - check server cfg/port"
}

# --- 5. launch client (offscreen) -----------------------------------------
# Push the SDL window far offscreen so it does not steal focus / show.
$env:SDL_VIDEO_WINDOW_POS = "-32000,-32000"
Step "Launching client (offscreen) -> exec $ClientCfgName"
$clientArgs = @(
    "-log", "-console", "-dev", "2", "-windowed",
    "-width", "1920", "-height", "1080", "-nosound",
    "+exec", $ClientCfgName
)
$client = Start-Process -FilePath $ClientExe -ArgumentList $clientArgs `
    -WorkingDirectory $ClientDir -PassThru
Step "Client PID $($client.Id); polling for exit (cfg ends in 'quit'), timeout ${TimeoutSec}s"

# --- 6. poll for client exit ----------------------------------------------
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while (-not $client.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
}
if (-not $client.HasExited) {
    Warn "client did not exit within ${TimeoutSec}s; forcing stop"
    try { $client | Stop-Process -Force } catch {}
} else {
    Step "Client exited (code $($client.ExitCode))"
}

# --- 7. stop server + any lingering xash3d under RunDir -------------------
Step "Stopping any lingering xash3d processes under $RunDir"
$runPrefix = $RunDir.TrimEnd('\') + '\'
Get-Process xash3d -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path -like "$runPrefix*" } |
    ForEach-Object {
        try { $_ | Stop-Process -Force; Step "  stopped PID $($_.Id)" } catch {}
    }

# --- 8. collect artifacts (COPY only) -------------------------------------
if (Test-Path $EngineLog) {
    Step "Copying engine.log -> $OutDir"
    Copy-Item $EngineLog -Destination (Join-Path $OutDir "engine.log") -Force
} else {
    Warn "no engine.log produced at $EngineLog (did -log / developer 1 take effect?)"
}

$ShotDir = Join-Path $ClientDir "cstrike\scrshots"
if (Test-Path $ShotDir) {
    $shots = Get-ChildItem -Path $ShotDir -Filter "*.png" -ErrorAction SilentlyContinue
    if ($shots) {
        Step "Copying $($shots.Count) screenshot(s) -> $OutDir"
        foreach ($s in $shots) {
            Copy-Item $s.FullName -Destination (Join-Path $OutDir $s.Name) -Force
        }
    } else {
        Warn "no screenshots found in $ShotDir"
    }
} else {
    Warn "scrshots dir not found: $ShotDir"
}

# --- 9. run the parser gate -----------------------------------------------
$OutLog = Join-Path $OutDir "engine.log"
if (-not (Test-Path $OutLog)) {
    Die "no engine.log collected - cannot run parser gate"
}
$OutJson = Join-Path $OutDir "fps.json"
Step "Running parse_fps.py gate"
& python $ParseFps $OutLog --json $OutJson
$gate = $LASTEXITCODE
Step "parse_fps.py exit code = $gate ($(if ($gate -eq 0) {'PASS'} else {'FAIL'}))"
exit $gate
