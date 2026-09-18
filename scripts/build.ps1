# Configure, build and test from any shell.
#
# CMakePresets.json points here, because the Ninja presets need an MSVC
# environment and getting one by hand is three steps people get wrong. It also
# guards against a failure that is worse than an error message: outside an x64
# Native Tools prompt, CMake with the Ninja generator will happily find MinGW
# g++ from a Strawberry Perl install on PATH and build the entire project with
# the wrong toolchain against a mismatched vcpkg triplet, reporting success the
# whole way. The presets pin the compiler to cl so that now fails loudly, and
# this script means nobody has to hit it at all.
#
#   .\scripts\build.ps1                      configure, build and test the dev preset
#   .\scripts\build.ps1 -Preset ci           the same for ci
#   .\scripts\build.ps1 -Gpu 1               run the suite against the second device
#   .\scripts\build.ps1 -NoTest              build only
#   .\scripts\build.ps1 -Clean               delete this preset's build directory first
#   .\scripts\build.ps1 -Target siggen       build one target

[CmdletBinding()]
param(
    [ValidateSet('dev', 'ci', 'vs', 'headless')]
    [string]$Preset = 'dev',

    [int]$Gpu = -1,

    [string]$Target = '',

    [switch]$NoTest,

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Find-VcVars {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found. Visual Studio 2022 or newer with the C++ workload is required."
    }
    $install = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $install) {
        throw "No Visual Studio install with the C++ toolset was found."
    }
    $vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) {
        throw "vcvars64.bat not found under $install."
    }
    return $vcvars
}

# The vs preset uses the Visual Studio generator, which locates its own
# toolchain. Only the Ninja presets need vcvars.
$needsVcVars = $Preset -ne 'vs'

if (-not $env:VCPKG_ROOT) {
    throw "VCPKG_ROOT is not set. The presets build their toolchain path from it."
}
if (-not $env:VULKAN_SDK) {
    throw "VULKAN_SDK is not set. The build needs glslangValidator and spirv-val from the SDK."
}

$buildDir = Join-Path $root "build\$Preset"
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "removing $buildDir" -ForegroundColor DarkGray
    Remove-Item -Recurse -Force $buildDir
}

if ($Gpu -ge 0) {
    $env:REVENANT_GPU_INDEX = "$Gpu"
    Write-Host "REVENANT_GPU_INDEX = $Gpu" -ForegroundColor DarkGray
}

$steps = @("cmake --preset $Preset")
$build = "cmake --build --preset $Preset"
if ($Target) { $build += " --target $Target" }
$steps += $build
if (-not $NoTest -and -not $Target) { $steps += "ctest --preset $Preset --output-on-failure" }

$chain = ($steps -join ' && ')

if ($needsVcVars) {
    $vcvars = Find-VcVars
    # One cmd invocation so the vcvars environment survives into every step.
    # Sourcing it per step would work and would trebled the cost for nothing.
    & cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$root`" && $chain"
} else {
    Push-Location $root
    try {
        foreach ($step in $steps) {
            Write-Host "> $step" -ForegroundColor DarkGray
            Invoke-Expression $step
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
    } finally {
        Pop-Location
    }
}

exit $LASTEXITCODE
