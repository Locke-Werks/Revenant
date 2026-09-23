<#
.SYNOPSIS
    Run revenant-engine and a /MD client as two processes and check they talk.

.DESCRIPTION
    tests/twoprocess/CMakeLists.txt registers this as the two_process_smoke
    test. Every case in tests/rpc runs the engine and its client in one
    address space, so none of them can see an allocation made on one C
    runtime's heap and freed on the other's. Here the engine is the /MT
    binary the root tree builds and the client is core/rpc/client.cpp built
    /MD, each in its own process, which is the arrangement revenant-ui and
    revenant-engine ship in.

    It starts the engine on a synthetic source with an ephemeral port and a
    token file of its own, reads the port off the line the engine prints and
    flushes for exactly this purpose, runs the client against it, and stops
    the engine whatever happened. The engine is given --duration as well, so
    an engine this script loses track of still exits on its own.

.PARAMETER Engine
    revenant-engine.exe from the root tree.

.PARAMETER Client
    rpc_smoke_client.exe from tests/twoprocess.

.PARAMETER Work
    A directory for the token file and the engine's output.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Engine,
    [Parameter(Mandatory)] [string]$Client,
    [Parameter(Mandatory)] [string]$Work,
    [int]$StartupSeconds = 120
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

foreach ($path in $Engine, $Client) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "$path does not exist. tests/twoprocess/CMakeLists.txt says how each is built."
    }
}

New-Item -ItemType Directory -Force -Path $Work | Out-Null
$token = Join-Path $Work "token"
$stdout = Join-Path $Work "engine.out"
$stderr = Join-Path $Work "engine.err"
Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue

# A synthetic scene rather than a radio, so the test needs a GPU and nothing
# else. --gpu -1 honours REVENANT_GPU_INDEX, which is how CI aims every binary.
$engineArgs = @(
    '"synthetic:wideband?rate=2400032&emitters=4&seed=4242"',
    "--gpu", "-1",
    "--block-samples", "16384",
    "--port", "0",
    "--token-file", "`"$token`"",
    "--duration", "180"
)
$process = Start-Process -FilePath $Engine -ArgumentList $engineArgs -NoNewWindow -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr

try {
    $port = $null
    $deadline = (Get-Date).AddSeconds($StartupSeconds)
    while (-not $port) {
        if ($process.HasExited) {
            Get-Content -LiteralPath $stdout, $stderr -ErrorAction SilentlyContinue
            throw "revenant-engine exited $($process.ExitCode) before it printed a port"
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
    "engine pid $($process.Id) listening on port $port"

    & $Client --port $port --token-file $token
    $clientExit = $LASTEXITCODE
    "client exited $clientExit"

    if ($process.HasExited) {
        Get-Content -LiteralPath $stdout, $stderr -ErrorAction SilentlyContinue
        throw "revenant-engine exited $($process.ExitCode) while the client was connected"
    }
    if ($clientExit -ne 0) {
        Get-Content -LiteralPath $stderr -ErrorAction SilentlyContinue
        exit $clientExit
    }
}
finally {
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit(10000) | Out-Null
    }
}
