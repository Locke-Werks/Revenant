<#
.SYNOPSIS
    Assemble the Forge installer payload from the two build trees.

.DESCRIPTION
    Revenant is two programs built by two separate CMake projects against two
    C runtimes, and the installer carries both. This script is the one place
    that says which files out of those two trees end up on a customer's disk.

    WHY A KEEP-LIST AND NOT "COPY THE DEPLOY DIRECTORY"

    windeployqt copies what it finds, not what the program loads. Measured on
    2026-09-21 against Qt 6.8.3 msvc2022_64: it produced 1303 files and 114.0
    MB, and revenant-ui.exe loaded 38 modules out of it. The difference is not
    slack to be tidied away for its own sake, it is six Qt Quick Controls
    styles that main.cpp makes unreachable by calling
    QQuickStyle::setStyle("Basic"), eleven QML debugging plugins, and 36 MB of
    alternative RHI backends. See docs/packaging.md for the measurement and
    for what each exclusion gives up.

    So the list below is explicit and the script fails when a member of it is
    missing. A Qt upgrade that renames or drops a file stops the build here,
    where the message names the file, rather than at first launch on somebody
    else's machine, where the message names one DLL and reads as a broken
    build.

    WHAT IT DELIBERATELY DOES NOT DO

    It does not sign anything. Payload members are hashed into the Forge
    container and extracted verbatim, so they have to be signed before they
    are staged, and signing the finished installer does nothing for them. The
    release job in .github/workflows/ci.yml signs this directory between
    staging and forging.

    WHAT IT ADDS BESIDE THE BINARIES

    The licence material each half owes, generated from the tree that built
    that half so it cannot describe a different build. The engine half gets
    LICENSE.txt, THIRD-PARTY-NOTICES-engine.txt from its static vcpkg tree,
    and licenses/LGPL-2.1.txt for libusb. The client half gets
    THIRD-PARTY-NOTICES-client.txt from its dynamic vcpkg tree, Qt's SPDX
    documents and the FFmpeg libraries' own report of their licence, plus
    licenses/LGPL-3.0.txt for Qt and licenses/LGPL-2.1.txt for FFmpeg.
    scripts/generate_notices.py writes the notices and says what it refuses.

    This paragraph used to say the script does not produce a third-party
    notices file because there was not one yet. That was true until
    2026-09-22.

.PARAMETER Only
    Which half to stage. CI builds the two halves in two jobs on two separate
    checkouts, so each one stages what it has the inputs for and the package
    job merges the results. A local run wants "both", which is the default.

.EXAMPLE
    .\scripts\stage-payload.ps1
    .\scripts\stage-payload.ps1 -Only client -Out dist\client
#>
[CmdletBinding()]
param(
    [string]$EngineBuildDir = "build/ci",
    [string]$UiDeployDir = "ui/build/vs/RelWithDebInfo",
    [string]$Out = "dist/stage",
    [ValidateSet("both", "engine", "client")]
    [string]$Only = "both",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
function Resolve-Under($path) {
    if ([System.IO.Path]::IsPathRooted($path)) { return $path }
    return Join-Path $root $path
}

$outDir = Resolve-Under $Out
$engineDir = Resolve-Under $EngineBuildDir
$deployDir = Resolve-Under $UiDeployDir

# The triplets are the ones the two CMakePresets.json files pin, and the Qt
# prefix is read from ui/CMakePresets.json rather than repeated here, so a Qt
# upgrade is one edit and the notices follow it.
$engineVcpkg = Join-Path $engineDir "vcpkg_installed/x64-windows-static"
$clientVcpkg = Join-Path (Split-Path -Parent $deployDir) "vcpkg_installed/x64-windows"
$uiPresets = Get-Content -Raw (Join-Path $root "ui/CMakePresets.json") | ConvertFrom-Json
$qtPrefix = ($uiPresets.configurePresets | Where-Object name -eq "base").cacheVariables.CMAKE_PREFIX_PATH

function Invoke-Notices {
    param([string[]]$Arguments)
    # py first: the launcher is what a python.org install puts on PATH, and a
    # bare python can be the Microsoft Store stub that opens a window instead.
    $python = Get-Command py -ErrorAction SilentlyContinue
    $prefix = @("-3")
    if (-not $python) {
        $python = Get-Command python -ErrorAction SilentlyContinue
        $prefix = @()
    }
    if (-not $python) {
        throw "no Python 3 on PATH, and scripts/generate_notices.py writes the notices " +
              "every payload carries. Install Python 3 rather than staging without them."
    }
    & $python.Source @prefix (Join-Path $root "scripts/generate_notices.py") @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "scripts/generate_notices.py exited $LASTEXITCODE; its message above names what is missing."
    }
}

function Copy-Licence {
    param([string]$Name)
    $source = Join-Path $root "licenses/$Name"
    if (-not (Test-Path -LiteralPath $source)) {
        throw "licenses/$Name is missing from the repository."
    }
    $destination = Join-Path $outDir "licenses"
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    Copy-Item -LiteralPath $source -Destination (Join-Path $destination $Name) -Force
}

# ---------------------------------------------------------------------------
# What the client half carries
# ---------------------------------------------------------------------------
#
# Every name here was loaded by revenant-ui.exe in a run from a directory with
# no Qt on PATH, except where the comment says otherwise. docs/packaging.md
# holds the module list that run produced.

# The Qt libraries the client links or loads. Qt6Network and Qt6OpenGL are not
# linked by ui/CMakeLists.txt and load anyway: Qt6Multimedia pulls the first
# and Qt6Quick the second. Qt6Svg is here because the qsvg image format plugin
# imports it.
$qtLibraries = @(
    "Qt6Core.dll"
    "Qt6Gui.dll"
    "Qt6Multimedia.dll"
    "Qt6Network.dll"
    "Qt6OpenGL.dll"
    "Qt6Qml.dll"
    "Qt6QmlMeta.dll"
    "Qt6QmlModels.dll"
    "Qt6QmlWorkerScript.dll"
    "Qt6Quick.dll"
    "Qt6QuickControls2.dll"
    "Qt6QuickControls2Basic.dll"
    "Qt6QuickControls2Impl.dll"
    "Qt6QuickLayouts.dll"
    "Qt6QuickTemplates2.dll"
    "Qt6Svg.dll"

    # QtQuick.Dialogs, for the recording section's file dialog. Kept on the
    # import rather than on a measured load: the dialog loads these only when
    # it opens, which the offscreen run that measured the rest never does.
    # Qt6LabsFolderListModel is what the non-native fallback dialog imports.
    "Qt6QuickDialogs2.dll"
    "Qt6QuickDialogs2QuickImpl.dll"
    "Qt6QuickDialogs2Utils.dll"
    "Qt6LabsFolderListModel.dll"
)

# Qt Multimedia's FFmpeg backend and the libraries it loads.
#
# ui/CMakeLists.txt used to say that raw PCM through QAudioSink is served by
# the windows platform backend and that nothing on this path depends on FFmpeg.
# Measured 2026-09-21: multimedia\ffmpegmediaplugin.dll and all five FFmpeg
# libraries are in the client's loaded module list and windowsmediaplugin.dll
# is not. FFmpeg is the default backend in Qt 6.8 and the client sets no
# override, so the backend that initialises QMediaDevices is the FFmpeg one.
$ffmpegLibraries = @(
    "avcodec-61.dll"
    "avformat-61.dll"
    "avutil-59.dll"
    "swresample-5.dll"
    "swscale-8.dll"
)

# The whole Visual C++ redistributable set that InstallRequiredSystemLibraries
# resolved, not the subset that loaded.
#
# Five of the eight loaded in the measured run. The other three are members of
# the same redistributable, are loaded on demand by a code path that run did
# not reach, and come to under 2 MB together. Shipping a redistributable minus
# the parts one execution happened not to touch is how a rare path turns into
# a loader dialog on somebody else's machine.
$crtLibraries = @(
    "concrt140.dll"
    "msvcp140.dll"
    "msvcp140_1.dll"
    "msvcp140_2.dll"
    "msvcp140_atomic_wait.dll"
    "msvcp140_codecvt_ids.dll"
    "vcruntime140.dll"
    "vcruntime140_1.dll"
)

# Qt plugins, by their path under the deploy directory. Qt finds these
# relative to the executable, so the directory names are part of the contract
# and not a layout choice.
$qtPlugins = @(
    "platforms/qwindows.dll"
    "imageformats/qgif.dll"
    "imageformats/qico.dll"
    "imageformats/qjpeg.dll"
    "imageformats/qsvg.dll"
    "multimedia/ffmpegmediaplugin.dll"
)

# QML modules, copied whole because a module is its qmldir, its .qmltypes, its
# .qml files and its plugin together, and a partial copy fails at import time
# with a message about a type rather than about a file.
#
# Main.qml imports QtQuick, QtQuick.Controls and QtQuick.Layouts. Controls
# resolves its style at run time, and main.cpp pins that to Basic, which is
# why exactly one of the seven style directories is here.
#
# RecordingSection.qml imports QtQuick.Dialogs for its FileDialog. On Windows
# that is the native dialog, served by the platform plugin; QtQuick/Dialogs
# carries its quickimpl fallback with it, which imports Qt.labs.folderlistmodel,
# so both are here whole rather than the fallback failing at import on a
# machine where the native one is refused.
$qmlModules = @(
    "qml/QtQml"
    "qml/QtQuick/Controls/Basic"
    "qml/QtQuick/Controls/impl"
    "qml/QtQuick/Dialogs"
    "qml/QtQuick/Layouts"
    "qml/QtQuick/Templates"
    "qml/QtQuick/Window"
    "qml/Qt/labs/folderlistmodel"
)

# Files that sit at the root of a QML module directory whose subdirectories
# are not all wanted. QtQuick and QtQuick/Controls are both in this shape.
$qmlLooseFiles = @(
    "qml/QtQuick/qmldir"
    "qml/QtQuick/plugins.qmltypes"
    "qml/QtQuick/qtquick2plugin.dll"
    "qml/QtQuick/Controls/qmldir"
    "qml/QtQuick/Controls/plugins.qmltypes"
    "qml/QtQuick/Controls/qtquickcontrols2plugin.dll"
)

function Copy-Member {
    param([string]$From, [string]$RelativeTo, [string]$Into)

    $source = Join-Path $RelativeTo $From
    if (-not (Test-Path -LiteralPath $source)) {
        throw "the payload wants $From and $RelativeTo does not have it. " +
              "If Qt moved or renamed it, fix the list in scripts/stage-payload.ps1 " +
              "rather than dropping the file: it was measured as loaded."
    }
    $destination = Join-Path $Into $From
    $parent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
}

function Copy-Tree {
    param([string]$From, [string]$RelativeTo, [string]$Into)

    $source = Join-Path $RelativeTo $From
    if (-not (Test-Path -LiteralPath $source -PathType Container)) {
        throw "the payload wants the QML module $From and $RelativeTo does not have it."
    }
    # Copy-Item is given the directory and the PARENT of where it goes, not
    # "<source>\*" and the destination. Against -LiteralPath a trailing
    # asterisk is a literal file name, so the wildcard form silently copies
    # nothing and the module is missing from the payload with no error. That
    # is what happened the first time this ran, and the count check below is
    # what caught it.
    $destination = Join-Path $Into $From
    $parent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    if (Test-Path -LiteralPath $destination) {
        Remove-Item -LiteralPath $destination -Recurse -Force
    }
    Copy-Item -LiteralPath $source -Destination $parent -Recurse -Force

    $copied = @(Get-ChildItem -LiteralPath $destination -Recurse -File)
    if ($copied.Count -eq 0) {
        throw "the QML module $From staged as an empty directory."
    }
}

if ($Clean -and (Test-Path -LiteralPath $outDir)) {
    Remove-Item -LiteralPath $outDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

if ($Only -in @("both", "engine")) {
    $engineExe = Join-Path $engineDir "tools/engined/revenant-engine.exe"
    if (-not (Test-Path -LiteralPath $engineExe)) {
        throw "revenant-engine.exe is not at $engineExe. Build the ci preset first: " +
              "powershell -File .\scripts\build.ps1 -Preset ci -Gpu 0"
    }
    Copy-Item -LiteralPath $engineExe -Destination (Join-Path $outDir "revenant-engine.exe") -Force

    # The project's own licence, beside the binaries rather than linked. A
    # recipient who has the installer and no network still has the terms.
    $license = Join-Path $root "LICENSE"
    if (-not (Test-Path -LiteralPath $license)) {
        throw "LICENSE is missing from the repository root."
    }
    Copy-Item -LiteralPath $license -Destination (Join-Path $outDir "LICENSE.txt") -Force

    Invoke-Notices @("engine", "--vcpkg", $engineVcpkg,
                     "--out", (Join-Path $outDir "THIRD-PARTY-NOTICES-engine.txt"))
    Copy-Licence "LGPL-2.1.txt"
}

if ($Only -in @("both", "client")) {
    if (-not (Test-Path -LiteralPath (Join-Path $deployDir "revenant-ui.exe"))) {
        throw "revenant-ui.exe is not at $deployDir. Build the client first: " +
              "cd ui; cmake --preset vs; cmake --build --preset vs"
    }

    Copy-Member -From "revenant-ui.exe" -RelativeTo $deployDir -Into $outDir
    foreach ($name in $qtLibraries + $ffmpegLibraries + $crtLibraries + $qtPlugins + $qmlLooseFiles) {
        Copy-Member -From $name -RelativeTo $deployDir -Into $outDir
    }
    foreach ($module in $qmlModules) {
        Copy-Tree -From $module -RelativeTo $deployDir -Into $outDir
    }

    Invoke-Notices @("client", "--vcpkg", $clientVcpkg, "--qt", $qtPrefix,
                     "--deploy", $deployDir,
                     "--out", (Join-Path $outDir "THIRD-PARTY-NOTICES-client.txt"))
    Copy-Licence "LGPL-3.0.txt"
    Copy-Licence "LGPL-2.1.txt"
}

# A PDB in a payload is a file the customer cannot use and the container has
# to carry. They are excluded by the keep-list above rather than deleted here;
# this catches a future addition that brings one along.
#
# -Include is not used here. Against -LiteralPath naming a directory it is
# silently ignored, every file matches, and the check reports the whole
# payload as a leak, which is how this line was wrong the first time.
$unwanted = @(".pdb", ".ilk", ".exp", ".lib")
$leaked = @(Get-ChildItem -LiteralPath $outDir -Recurse -File |
    Where-Object { $unwanted -contains $_.Extension.ToLowerInvariant() })
if ($leaked.Count -gt 0) {
    throw "the payload holds build artefacts that do not belong on a customer's disk: " +
          ($leaked.Name -join ", ")
}

$files = @(Get-ChildItem -LiteralPath $outDir -Recurse -File)
$bytes = ($files | Measure-Object -Property Length -Sum).Sum
"staged $($files.Count) files, $('{0:N1}' -f ($bytes / 1MB)) MB, into $outDir"
