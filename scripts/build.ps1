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

# Scoped to this script, not left behind in the caller's session. A helper that
# silently pins the device for every later command in the same shell is worse
# than one that does not offer the option at all: the next ctest run aims
# somewhere nobody asked for, and the result reads as a difference between
# devices rather than a mistake.
$previousGpu = $env:REVENANT_GPU_INDEX
$previousPath = $env:PATH
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

try {
    if ($needsVcVars) {
        $vcvars = Find-VcVars

        # Put Visual Studio's own Ninja first.
        #
        # vcvars appends its Ninja directory rather than prepending it, so on a
        # machine with Strawberry Perl on PATH the build still picks up
        # C:\Strawberry\c\bin\ninja.exe even after sourcing vcvars. That is the
        # second trap docs/building.md documents, and sourcing vcvars alone
        # does not escape it: verified by running `where ninja` inside a vcvars
        # shell and seeing Strawberry's copy listed first.
        #
        # It works today. The problem is that the build quietly depends on a
        # Perl distribution nobody installed for this, and stops the day
        # somebody removes it.
        # vcvars64.bat sits at <install>\VC\Auxiliary\Build\, so the install
        # root is four levels up from the file itself.
        $vsRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $vcvars)))
        $vsNinja = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
        if (Test-Path (Join-Path $vsNinja 'ninja.exe')) {
            # Prepended here rather than inside the cmd chain. A `set
            # "PATH=...;%PATH%"` in the chain looks equivalent and is not: cmd
            # expands %PATH% when it parses the whole command line, which is
            # before vcvars has run, so it captures the pre-vcvars PATH and
            # then restores it over the top of vcvars' additions. The compiler
            # disappears and the error says cl is not on PATH, which is true
            # and deeply confusing.
            #
            # Setting it in the parent process instead means the child inherits
            # it, vcvars appends its own entries after, and ours stays first.
            $env:PATH = "$vsNinja;$env:PATH"
        } else {
            Write-Warning "Visual Studio's ninja was not found; falling back to whatever is on PATH."
        }

        # One cmd invocation so the vcvars environment survives into every step.
        # Sourcing it per step would work and would treble the cost for nothing.
        & cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$root`" && $chain"
    } else {
        Push-Location $root
        try {
            foreach ($step in $steps) {
                Write-Host "> $step" -ForegroundColor DarkGray
                Invoke-Expression $step
                if ($LASTEXITCODE -ne 0) { break }
            }
        } finally {
            Pop-Location
        }
    }
} finally {
    if ($null -eq $previousGpu) {
        Remove-Item Env:REVENANT_GPU_INDEX -ErrorAction SilentlyContinue
    } else {
        $env:REVENANT_GPU_INDEX = $previousGpu
    }
    $env:PATH = $previousPath
}

exit $LASTEXITCODE
