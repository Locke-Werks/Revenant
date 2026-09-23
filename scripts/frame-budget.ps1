<#
.SYNOPSIS
    Run revenant-ui at the client's full target load and write its frame stats.

.DESCRIPTION
    M2 closes when the render pipeline holds its frame budget at full target
    load. docs/ui-spectrum.md, "Frame budget", defines that load for the client
    and this script is the one command that produces it:

      - revenant-engine on a synthetic 20 MS/s scene paced to realtime, 64
        channels, the default 2048-point spectrum (65536 bins a frame) and
        65536-sample blocks, so about 305 spectrum frames a second are
        offered and the client asks for every one of them;
      - eight receivers in the rack, the client's limit, spread across the
        span in five modes, the first focused with its passband display and
        passband waterfall live in the receiver window;
      - the engine's detector running, as it always does, with the client
        polling it at 4 Hz and drawing what it tracks on both span displays.

    The scene has two emitters and not the sixty-four the registry's example
    names, because the synthetic source renders on one CPU thread (see
    core/source/synthetic_source.h) and cannot keep up with more at 20 MS/s.
    Measured unthrottled on 2026-09-23: 1.42x realtime with none, 1.08x with
    two, 1.05x with three, 0.96x with four, 0.36x with eight and 0.02x with
    sixty-four. A scene below realtime offers the display fewer frames than
    the screen refreshes, and the interval measured is then the source's. The
    client's per-frame work does not depend on the emitter count; only the
    number of detection boxes does.

    The engine binds an ephemeral loopback port and mints a token file of its
    own under -Work, so it never meets an engine already running on this
    machine or its token. It is given --duration as well, so an engine this
    script loses track of still exits on its own. The client runs as a smoke
    run, so it opens no sound card and writes no settings, and it exits by
    itself when -Seconds of measurement are done.

    Without -Visible the client runs on the offscreen platform, which renders
    with Qt's software rasteriser and has no vsync: its numbers say what the
    GUI thread and the scene graph cost, not whether a frame met a refresh.
    -Visible opens real windows on the default platform, and that is the run
    the M2 criterion is measured by, with one condition: the windows have to
    be in front. A window this script starts opens behind whatever is in
    front, and a covered window is neither composed nor paced by the display,
    so it measures the occlusion instead (ui/main.cpp, --on-top, has the
    numbers). -OnTop keeps both windows above every other window for the run.
    It covers the screen for the length of the run, so it is for a machine
    nobody is using or a time agreed with whoever is.

    A CI run in progress is refused, because the engine takes the GPU CI's
    runner uses on this machine.

.PARAMETER Seconds
    Seconds of measurement. The client runs longer by the warm-up and the time
    it takes to connect.

.PARAMETER Visible
    Real windows at the screen's refresh rather than the offscreen platform.

.PARAMETER OnTop
    With -Visible, keep both windows above every other window. See above for
    why a measurement needs it and whose screen it takes.

.PARAMETER NoReceiverWindow
    Leave the receiver window closed. Not the target load: it is the control
    that says whether a second vsync-paced window costs the main one frames.

.PARAMETER Work
    Where the token, the engine's output, the JSON and the receiver window's
    picture go.
#>
[CmdletBinding()]
param(
    [int]$Seconds = 60,
    [switch]$Visible,
    [switch]$OnTop,
    [switch]$NoReceiverWindow,
    [string]$Work = (Join-Path $env:TEMP 'revenant-frame-budget'),
    [string]$Engine = (Join-Path $PSScriptRoot '..\build\ci\tools\engined\revenant-engine.exe'),
    [string]$Client = (Join-Path $PSScriptRoot '..\ui\build\vs\RelWithDebInfo\revenant-ui.exe'),
    [int]$StartupSeconds = 120
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

foreach ($path in $Engine, $Client) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "$path does not exist. Build the engine with .\scripts\build.ps1 -Preset ci -NoTest and the client from ui\ with the vs preset."
    }
}

$running = gh run list --repo Locke-Werks/Revenant --status in_progress --limit 1 --json databaseId | ConvertFrom-Json
if ($running) {
    throw "a CI run is in progress (run $($running[0].databaseId)); try again when it has finished"
}

New-Item -ItemType Directory -Force -Path $Work | Out-Null
$token = Join-Path $Work 'token'
$stdout = Join-Path $Work 'engine.out'
$stderr = Join-Path $Work 'engine.err'
$mode = if ($Visible) { 'visible' } else { 'offscreen' }
if ($NoReceiverWindow) {
    $mode += '-main-only'
}
$stats = Join-Path $Work "frame-stats-$mode.json"
$picture = Join-Path $Work "receivers-$mode.png"
Remove-Item -LiteralPath $stdout, $stderr, $stats -Force -ErrorAction SilentlyContinue

# Seconds before the measured window opens: connecting, then the probe's
# three-second warm-up after the first frame. Connecting takes well under a
# second on loopback, so the measured window comes out a little over -Seconds.
$lead = 4
$clientSeconds = $Seconds + $lead

$engineArgs = @(
    '"synthetic:wideband?rate=20000000&center=100000000&emitters=2&seed=20260918"',
    '--pace', '1',
    '--port', '0',
    '--token-file', "`"$token`"",
    '--duration', ($clientSeconds + 60),
    '--quiet'
)
$engineProcess = Start-Process -FilePath $Engine -ArgumentList $engineArgs -NoNewWindow -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr

try {
    $port = $null
    $deadline = (Get-Date).AddSeconds($StartupSeconds)
    while (-not $port) {
        if ($engineProcess.HasExited) {
            Get-Content -LiteralPath $stdout, $stderr -ErrorAction SilentlyContinue
            throw "revenant-engine exited $($engineProcess.ExitCode) before it printed a port"
        }
        if ((Get-Date) -gt $deadline) {
            Get-Content -LiteralPath $stdout, $stderr -ErrorAction SilentlyContinue
            throw "revenant-engine printed no port in $StartupSeconds s"
        }
        $line = Select-String -LiteralPath $stdout -Pattern 'listening on 127\.0\.0\.1:(\d+)' `
            -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($line) {
            $port = $line.Matches[0].Groups[1].Value
        } else {
            Start-Sleep -Milliseconds 100
        }
    }
    "engine pid $($engineProcess.Id) on port $port"

    # Five modes over the span, none on another's band. The first is focused.
    $receivers = @(
        '100.3M:wfm', '92.4M:nfm', '94.1M:am', '96.7M:usb',
        '103.2M:lsb', '105.5M:nfm', '107.1M:am', '108.6M:cw'
    )
    $clientArgs = @('127.0.0.1', $port, '--smoke-seconds', $clientSeconds, '--frame-stats', $stats)
    if (-not $NoReceiverWindow) {
        # A smoke run shows the receiver window only to photograph it.
        $clientArgs += @('--grab-receivers', $picture)
    }
    if ($Visible) {
        $clientArgs += '--maximise'
    }
    if ($OnTop) {
        $clientArgs += '--on-top'
    }
    foreach ($receiver in $receivers) {
        $clientArgs += @('--receiver', $receiver)
    }

    # Start-Process -Wait rather than the call operator: revenant-ui is a
    # windowed program, and PowerShell does not wait for one of those, which
    # left the engine stopped under a client still measuring.
    $clientOut = Join-Path $Work "client-$mode.err"
    $env:REVENANT_RPC_TOKEN_FILE = $token
    $env:QT_QPA_PLATFORM = if ($Visible) { 'windows' } else { 'offscreen' }
    try {
        $quoted = $clientArgs | ForEach-Object { if ("$_" -match '\s') { "`"$_`"" } else { "$_" } }
        $clientProcess = Start-Process -FilePath $Client -ArgumentList $quoted -NoNewWindow `
            -PassThru -Wait -RedirectStandardError $clientOut
        $clientExit = $clientProcess.ExitCode
    }
    finally {
        Remove-Item Env:REVENANT_RPC_TOKEN_FILE, Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
    }
    Get-Content -LiteralPath $clientOut -ErrorAction SilentlyContinue
    "client exited $clientExit; stats in $stats"

    # A window DWM is not composing is not paced by the display at all, and
    # Windows 11 then serves the client's timers at the 15.6 ms tick, so it
    # runs at about 64 frames a second whatever the client does. Seen on
    # 2026-09-23 with a one-rectangle Qt Quick window behind two maximised
    # windows, before --on-top: 64.0 frames a second. A run like that
    # measures the occlusion, or a display that is asleep.
    if ($Visible -and $clientExit -eq 0 -and (Test-Path -LiteralPath $stats)) {
        $report = Get-Content -LiteralPath $stats -Raw | ConvertFrom-Json
        $main = $report.windows | Where-Object { $_.name -eq 'main' } | Select-Object -First 1
        if ($main -and $main.interval_ms.p50 -gt 1.5 * $report.budget_ms) {
            Write-Warning ("the main window's median frame interval was $($main.interval_ms.p50) ms " +
                "against a $($report.budget_ms) ms refresh: the display is not pacing frames " +
                "(covered by another window, or asleep), so this run does not measure the budget; see -OnTop")
        }
    }
    if ($clientExit -ne 0) {
        exit $clientExit
    }
}
finally {
    if (-not $engineProcess.HasExited) {
        Stop-Process -Id $engineProcess.Id -Force
        $engineProcess.WaitForExit(10000) | Out-Null
    }
}
