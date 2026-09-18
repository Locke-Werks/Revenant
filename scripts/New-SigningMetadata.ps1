<#
.SYNOPSIS
    Generates signing/metadata.json from metadata.json.in and signing.env.

.DESCRIPTION
    metadata.json is generated rather than committed so the endpoint, account
    and certificate profile are written down exactly once. Hand-maintained
    copies drifted into three different profile names across four other
    Locke Werks repositories, and CI fails the build if the generated file is
    ever committed here.

    Values come from signing.env and can be overridden by environment variables
    of the same name, which is how CI supplies a different profile without
    editing a tracked file.

    Nothing in Revenant is signed yet. There is no shippable binary until M3.
    This exists so the shape is already right when there is.

.PARAMETER Force
    Regenerate over an existing signing/metadata.json.

.EXAMPLE
    ./scripts/New-SigningMetadata.ps1 -Force
#>

[CmdletBinding()]
param(
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Template = Join-Path $RepoRoot 'signing\metadata.json.in'
$EnvFile  = Join-Path $RepoRoot 'signing\signing.env'
$Output   = Join-Path $RepoRoot 'signing\metadata.json'

foreach ($required in @($Template, $EnvFile)) {
    if (-not (Test-Path $required)) {
        throw "missing $required"
    }
}

if ((Test-Path $Output) -and -not $Force) {
    Write-Host "$Output already exists. Pass -Force to regenerate." -ForegroundColor Yellow
    exit 0
}

$values = @{}
foreach ($line in Get-Content $EnvFile) {
    $trimmed = $line.Trim()
    if ($trimmed -eq '' -or $trimmed.StartsWith('#')) { continue }
    $split = $trimmed.Split('=', 2)
    if ($split.Count -ne 2) { continue }
    $values[$split[0].Trim()] = $split[1].Trim()
}

# Environment wins, so CI can point at a different profile without a file edit.
foreach ($key in @($values.Keys)) {
    $fromEnv = [Environment]::GetEnvironmentVariable($key)
    if ($fromEnv) { $values[$key] = $fromEnv }
}

$content = Get-Content $Template -Raw
foreach ($key in $values.Keys) {
    $content = $content.Replace("@$key@", $values[$key])
}

if ($content -match '@[A-Z_]+@') {
    throw "unsubstituted placeholder remains: $($Matches[0])"
}

# Written through .NET rather than Set-Content because Set-Content -Encoding
# UTF8 emits a byte order mark under Windows PowerShell 5.1 and none under
# PowerShell 7. A BOM ahead of the opening brace is a parse error to a strict
# JSON reader, and the failure surfaces during signing as an unhelpful
# complaint about metadata rather than about encoding.
[System.IO.File]::WriteAllText($Output, $content, [System.Text.UTF8Encoding]::new($false))

Write-Host "Wrote $Output" -ForegroundColor Green
Write-Host "  endpoint $($values['REVENANT_SIGN_ENDPOINT'])"
Write-Host "  account  $($values['REVENANT_SIGN_ACCOUNT'])"
Write-Host "  profile  $($values['REVENANT_SIGN_PROFILE'])"
