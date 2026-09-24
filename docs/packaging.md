# Packaging

One installer, `Revenant-Setup.exe`, built by Forge. It carries both programs.

Everything below was measured on 2026-09-21 on the development machine against
Qt 6.8.3 `msvc2022_64`, Visual Studio 17.14 and the `ci` preset. Where a number
appears, the command that produced it is named, because the Qt tree here is an
aqtinstall checkout with no MaintenanceTool and the version and plugin set are
facts about this machine rather than about Qt.

## The pieces

| | |
| --- | --- |
| `installer.toml` | The Forge config at the repository root. Product identity, install directory, preflights, the one option and the shortcuts |
| `scripts/stage-payload.ps1` | Assembles the payload directory out of the two build trees, licence material included |
| `scripts/generate_notices.py` | Writes each program's third-party notices from the tree that built it |
| `scripts/corresponding_source.py` | Records which copyleft sources went into the engine, then fetches, checks and zips them |
| `licenses/` | The LGPL texts and the SPDX licence texts the notices need, with where each came from in `generate_notices.py` |
| `package` job in `.github/workflows/ci.yml` | Unsigned installer and the corresponding-source archive on every build, as artifacts |
| `release` job in the same file | Signed installer and the corresponding-source archive on a `v*` tag, published to the release together |

## Reproducing it locally

Build both trees, stage, forge. Nothing here needs a C++ toolchain past the
two builds: `lwforge` stamps a config and a directory into a copy of a prebuilt
stub.

```powershell
powershell -File .\scripts\build.ps1 -Preset ci -Gpu 0
cd ui; cmake --preset vs; cmake --build --preset vs; cd ..

.\scripts\stage-payload.ps1 -Clean

lwforge.exe build --config installer.toml --payload dist\stage `
  --stub lwstub.exe --out dist\Revenant-Setup.exe --dev
```

`--dev` marks the container an unsigned development build, which the stub
displays. `lwforge` refuses an unsigned stub without it, which is the check
that stops an unsigned uninstaller landing in Program Files where Settings
invokes it elevated.

Get `lwforge.exe` and `lwstub.exe` from the `Locke-Werks/Forge` release rather
than building Forge: the release stub is already signed, which is what makes
the embedded uninstaller signed without a second signing pass here. CI pins
`FORGE_VERSION`. Read the version note in `Forge/docs/using-forge-in-ci.md`
before moving the pin: a pin older than v0.3.0 does not fail the build on
`[[options]]`, it inverts them, and a `when` then fires unconditionally.

The local verification of this config ran against `lwforge` built from the
Forge working tree, not against the pinned release. It produced:

```
Revenant 0.1.0
  payload   135 files, 72861828 bytes raw
  container 25999288 bytes at offset 493568
  output    dist\Revenant-Setup.exe (26492856 bytes)
```

`--check-only` on the result listed the one option and passed all four
preflights.

## What is in the container

135 files, 69.5 MB on disk, 26.5 MB as the finished installer. `lwforge` reports
136 payload members: the extra one is the stub itself, added at
`.lw\uninstall.exe`, which is the embedded uninstaller.

| Part | Files | Size |
| --- | --- | --- |
| `revenant-engine.exe` and `LICENSE.txt` | 2 | 6.2 MB |
| `revenant-ui.exe` and the Qt runtime it needs | 133 | 63.2 MB |
| Notices and licence texts, below | 4 | 0.2 MB |

The last row was added on 2026-09-22 and the counts above it were measured the
day before. The table is kept as the 2026-09-21 measurement; the payload has
grown twice since.

Measured on 2026-09-23 in the release dry run below, from a clean worktree at
`b43d763`: staging each half separately and merging them, the way the
`package` job does, gave **199 files, 80,325,761 bytes**, identical file for
file to one `stage-payload.ps1` run over both. `lwforge` v0.4.0 reported 200
payload members, the stub included, and a 28,634,280 byte installer. The
`package` job on the same commit, run 35944382945, staged 199 files and
80,327,809 bytes. The 60 files over the earlier 139 are `QtQuick.Dialogs` and
`Qt.labs.folderlistmodel` with their four libraries, described under "What
the client actually needs".

WHAT THIS PARAGRAPH USED TO SAY, in its second sentence: "so the payload is
now 139 files. Staging both halves that day wrote 139 and `lwforge inspect`
listed all four licence members in the container." True on 2026-09-22, and
stale from the commit that kept the file dialog's modules.

### The licence material

| Member | From | For |
| --- | --- | --- |
| `LICENSE.txt` | `LICENSE`, byte for byte the FSF's GPL-3.0 text | Revenant, both programs |
| `THIRD-PARTY-NOTICES-engine.txt` | `generate_notices.py engine`, over `build/ci/vcpkg_installed/x64-windows-static` | Every port linked into the engine, with its licence verbatim, the LGPL-2.1 section 6 notice for libusb, the GPL note for rtlsdr, and pthreads4w's upstream `NOTICE` |
| `THIRD-PARTY-NOTICES-client.txt` | `generate_notices.py client`, over the client's vcpkg tree, Qt's `sbom/*.spdx.json` and the FFmpeg DLLs themselves | capnproto, Qt 6.8.3 under LGPL-3.0 with the 55 distinct bundled components its SPDX documents attribute in the four modules shipped, FFmpeg as the DLLs report it, the Visual C++ runtime, and an appendix with every licence text those name |
| `licenses/LGPL-2.1.txt` | gnu.org, 2026-09-22 | libusb, FFmpeg |
| `licenses/LGPL-3.0.txt` | gnu.org, 2026-09-22 | Qt |

Two notices files rather than one because the halves are staged by different
CI jobs on different checkouts, and each can only describe the tree it has.

The FFmpeg paragraph is measured, not copied from Qt's documentation. The
generator loads the five DLLs and asks each for its licence, and asks
`avcodec` for its configure line: "LGPL version 2.1 or later" from all five,
version 7.1, configured with `--enable-shared --disable-static` and no
`--enable-gpl`.

The generator fails rather than writing a file with a hole in it: a port with
no `copyright` or no `vcpkg.spdx.json`, a Qt attribution whose SPDX identifier
has no text under `licenses/spdx/`, and pthreads moving off the version its
`NOTICE` was taken from all stop the stage, naming the thing that is missing.

### One directory, not two

The engine is `/MT` and the client is `/MD`, and that split is why they are two
processes: a `std::string` handed across it in-process is allocated on one heap
and freed on another. It is not a reason for two payloads or two directories.

Measured with `dumpbin /dependents` on `revenant-engine.exe`: `WS2_32.dll`,
`bcrypt.dll`, `SHELL32.dll`, `ADVAPI32.dll`, `ole32.dll`, `vulkan-1.dll`,
`KERNEL32.dll`. No `MSVCP140`, no `VCRUNTIME140`. The engine imports no C
runtime DLL at all, so it cannot resolve the wrong one out of a directory that
also holds the client's. Everything installs into `{ProgramFiles}\Revenant`
side by side, which is also what the client's own DLL resolution wants: Windows
searches the directory of the executable being launched, so the Qt libraries
have to sit beside `revenant-ui.exe`.

`vulkan-1.dll` is the loader and arrives with the graphics driver. It is not in
the payload and must not be.

### What the client actually needs, as distinct from what windeployqt copies

`windeployqt` copies what it finds. Measured: its output for this client, minus
PDBs and the test binary, is **1303 files and 114.0 MB**. Launching
`revenant-ui.exe` from that directory with `PATH` reduced to
`C:\Windows\system32;C:\Windows;C:\Windows\System32\Wbem` and the working
directory set elsewhere, the process loaded **38 modules**, all of them from
that directory and none from anywhere but that directory and `C:\Windows`. The
window came up titled `Revenant  ·  waiting for 127.0.0.1:17690`, which is the
client running with no engine, so the run reached the QML, the scene graph and
Qt Multimedia's backend.

The staged payload holds those 38 plus the files that are read rather than
loaded. Repeating the same launch against the staged payload produced the same
38 modules and the same window.

Kept, and why:

- The sixteen `Qt6*.dll` that loaded. `Qt6Network` and `Qt6OpenGL` are not
  linked by `ui/CMakeLists.txt`: Qt Multimedia pulls the first and Qt Quick the
  second. `Qt6Svg` is pulled by the `qsvg` image format plugin.
- `platforms/qwindows.dll`, and the four `imageformats` plugins, which Qt loads
  while building its list of supported formats.
- `multimedia/ffmpegmediaplugin.dll` and five FFmpeg libraries, 18.3 MB
  together. See below.
- Eight Visual C++ redistributable DLLs, of which five loaded. The other three
  are members of the same redistributable, are loaded on demand, and come to
  under 2 MB. Shipping a redistributable minus the parts one execution did not
  touch is how a rare path becomes a loader dialog on somebody else's machine.
- Eight QML modules copied whole, plus the loose files at the root of `QtQuick`
  and `QtQuick/Controls`. A QML module is its `qmldir`, its `.qmltypes`, its
  `.qml` files and its plugin together, and a partial copy fails at import with
  a message naming a type rather than a file.
- `QtQuick.Dialogs` and its libraries, `Qt6QuickDialogs2`,
  `Qt6QuickDialogs2QuickImpl` and `Qt6QuickDialogs2Utils`, since 2026-09-23,
  for the file dialog in the picker's recording section. On Windows the dialog
  is the native one, served by the platform plugin; the module carries a
  drawn fallback under `quickimpl`, which imports `Qt.labs.folderlistmodel`,
  so that module and `Qt6LabsFolderListModel` are kept too rather than the
  fallback failing at import. These four were NOT in the measured run's module
  list: the dialog loads them when it opens, and the offscreen run that
  measured the rest never opens one. They are kept on the import, which
  `scripts/stage-payload.ps1` says beside them.

WHAT THE FIRST ITEM OF THAT PAIR USED TO SAY: "Six QML modules copied whole".
The two added for the file dialog made it eight.

`qtquick2plugin.dll` and `QtQml`'s `qmlplugin.dll` are in the payload and did
not load. Their `qmldir` marks them optional and the linked Qt libraries
register the types, so they are dead weight in this build and 100 KB. They stay
because a module missing a file its `qmldir` names is a failure mode that does
not announce itself at build time.

### The FFmpeg finding, which corrects a comment in the tree

`ui/CMakeLists.txt` used to say that raw PCM through `QAudioSink` is served by
the windows platform backend, that nothing on this path depends on FFmpeg, and
that an install that carries only the windows one still plays audio. It was
written to decide this payload.

Measured, and it is the other way round. `multimedia/ffmpegmediaplugin.dll` is
in the loaded module list together with `avcodec-61`, `avformat-61`,
`avutil-59`, `swresample-5` and `swscale-8`. `windowsmediaplugin.dll` is not
loaded at all. FFmpeg is the default Qt Multimedia backend in Qt 6.8 and the
client sets no override, so the backend that initialises `QMediaDevices` and
serves `QAudioSink` is the FFmpeg one and the five libraries come with it.

That is 18.3 MB of a 63.2 MB client payload. Dropping it means putting
`QT_MEDIA_BACKEND=windows` into the process before Qt Multimedia initialises,
which is a change to `main.cpp` and a claim about the Windows backend that
would then need its own measurement on the audio path. It is not a packaging
decision and it is not made here.

### What was left out, and what each exclusion gives up

| Left out | Size | What it costs |
| --- | --- | --- |
| Six Qt Quick Controls styles and NativeStyle | 6.7 MB | Nothing reachable. `main.cpp` calls `QQuickStyle::setStyle("Basic")`, so a style set through `QT_QUICK_CONTROLS_STYLE` is overridden before the QML loads |
| `opengl32sw.dll` | 19.7 MB | The software OpenGL fallback. Qt Quick's RHI defaults to Direct3D 11 on Windows, which falls back to WARP in software without this file. `QSG_RHI_BACKEND=opengl` on a machine with no OpenGL driver will not start |
| `dxcompiler.dll` and `dxil.dll` | 15.1 MB | `QSG_RHI_BACKEND=d3d12` will not start |
| `qmltooling` | 1.0 MB | The QML debugger and profiler, reachable only with `-qmljsdebugger` on the command line |
| `QtQuick/Effects`, `QtQuick/Shapes` | small | `Main.qml` imports `QtQuick`, `QtQuick.Controls`, `QtQuick.Layouts` and `Revenant`, and neither of these; `RecordingSection.qml` adds `QtQuick.Dialogs`, which is kept, and neither of these either |
| `windowsmediaplugin.dll`, `networkinformation`, `tls`, `iconengines`, `generic/qtuiotouchplugin` | small | Unloaded in the measured run. The TLS backends matter only to `QSslSocket`, which nothing here opens: the RPC wire is plaintext Cap'n Proto over a socket |

Three of those are one-line reversals in `scripts/stage-payload.ps1`. The script
fails the build when a file on its keep-list is missing, so a Qt upgrade that
renames something stops there, where the message names the file, rather than at
first launch on somebody else's machine, where the message names one DLL and
reads as a broken build.

## What the installer does to a machine

Machine scope, `{ProgramFiles}\Revenant`, elevation required. `installer.toml`
carries the reasoning for each of these inline; the summary:

- **Preflights.** Windows 11 by build number, 256 MB free on the install
  volume, and neither `revenant-engine.exe` nor `revenant-ui.exe` running. A
  running copy refuses the install with exit 1603 and the person has to close
  it: `restart_manager = true` is set on both and Forge v0.4.0 does nothing
  with it, see the comment in `installer.toml`. The name matches any process
  on the machine, from any path, so an engine running out of a build tree
  blocks an install as well. WHAT THIS ITEM USED TO SAY: "the last two with
  `restart_manager` so an upgrade offers to close them rather than refusing."
  The stub has never offered. There is no Vulkan preflight: the engine imports
  `vulkan-1.dll` and would genuinely fail without it, but the client is a
  remote user interface with every reason to be installed on a machine that
  will never open a GPU.
- **Actions.** An Add/Remove Programs entry and a Start Menu shortcut to the
  client. A desktop shortcut behind the one option, `desktop_shortcut`,
  defaulted off and answerable unattended with `/O:desktop_shortcut=off`.
- **No service, no file associations, no hooks.** The reasons are in
  `installer.toml`. The short form: the engine holds a GPU and a radio and
  needs a person to start it, `.wav` belongs to whatever already plays audio,
  and nothing in the install needs code to run.
- **Nothing per-user.** The RPC token the client hands `Authenticator.login`
  lives at `%LOCALAPPDATA%\Revenant\rpc-token` and is created by the engine on
  first run under the invoking user's own token. An elevated installer writing
  there would put it in the administrator's profile owned by
  `BUILTIN\Administrators`, which is the failure `as = "user"` exists for and
  which is better avoided than handled.

`upgrade_code` is `d4be342a-9c6c-4c6b-9fd7-848637a79967`, generated on
2026-09-22 before any release and never to change. It keys the stub's
single-instance mutex, so two Revenant installers of any version refuse to run
at once rather than racing over one directory, and a different value in a later
installer would make it a stranger to every earlier one.

This paragraph used to say `upgrade_code` is deliberately absent from
`installer.toml` and has to be chosen before the first release; it has been.

## Signing

Azure Artifact Signing, account and certificate profile `specterpoint`, which
signs as Specter Point Intelligence, LLC. That is the parent organisation and a
Locke Werks product signed by it is correct.

The order is the whole point and the `release` job holds it:

1. Sign the staged payload. `lwforge` hashes payload members into the container
   and the stub extracts them byte for byte, so an unsigned binary going in is
   an unsigned binary on the customer's disk. Signing the finished installer
   does nothing for them.
2. Forge against the signed release stub. That stub becomes
   `.lw\uninstall.exe`, hashed and extracted like any other member and never
   re-signed on the way out.
3. Sign the installer last, after resource stamping and after the container is
   appended, because the signature is what makes the bytes immutable. Appended
   data before the certificate table is inside the Authenticode digest, so this
   order puts the payload under the signature rather than outside it.
4. Verify with `Get-AuthenticodeSignature` and then `lwforge inspect`. The
   second is the check people leave out: it proves the container is still
   readable behind the certificate table rather than orphaned.

Authentication is `AZURE_TENANT_ID`, `AZURE_CLIENT_ID` and
`AZURE_CLIENT_SECRET`: a user principal with a client secret. There is no
federated identity, no OIDC subject and no `azure/login` step, and there is not
going to be one. The first two are repository variables and the third is a
secret on the `release` environment.

The endpoint, the account and the profile come from `signing/signing.env` at
run time rather than being written into the workflow, because that file exists
precisely so the profile name is in one place. It drifted into three different
answers across four other repositories once it was copied.

`signing/metadata.json` and its `ExcludeCredentials` list are for **local**
signing through a script. The action builds its own metadata on the runner. Do
not commit the generated file; `guards` fails the build if it appears.

Nothing has been signed yet. The `release` job has never run.

## The corresponding sources

`docs/clean-room.md` decided on 2026-09-20 that LGPL-2.1 section 6d and
GPL-3.0 section 6d are both discharged by offering the binary and the sources
from the same designated place. The release page is that place, so the
`release` job publishes `Revenant-<version>-corresponding-source.zip` in the
same `gh release create` as the installer:

- `revenant-<version>-source.zip`, `git archive` of the tagged commit.
- `upstream/libusb-libusb-v1.0.29.tar.gz` and
  `upstream/rtlsdr-rtl-sdr-797f8143266d983c56d8f35d2d442527529dd8a5.tar.gz`,
  the archives vcpkg built from. The second is `osmocom/rtl-sdr` at the commit
  that is also its v2.0.3 tag, pinned by commit in the overlay's portfile.
- `vcpkg-port-libusb/`, libusb's recipe from the vcpkg git tree it was built
  from, with no patches.
- `vcpkg-port-rtlsdr/`, rtlsdr's recipe from `vcpkg-overlays/rtlsdr/` in this
  repository at the tagged commit: the portfile, `dependencies.diff`,
  `library-linkage.diff`, `tools.diff` and `cancel-waits-for-transfers.diff`,
  the last of them Revenant's own change to librtlsdr.
- `SOURCES.txt`, saying what each piece is and how it was checked.

rtlsdr's upstream archive is beyond the decision record's list of libusb's
source, the patches and Revenant's own archive. It is there because the
patches are changes to that base and are not the source of anything without it.

librtlsdr has been built from Revenant's overlay port since 2026-09-23, and
`docs/rtlsdr-provenance.md`, "The librtlsdr the engine links", says why. vcpkg
records an overlay port's origin as NOASSERTION rather than a git tree, so
`corresponding_source.py manifest` finds the recipe under `vcpkg-overlays/`
and refuses it unless every file matches the SHA256 the build recorded, and
`bundle` takes it from git at the commit being archived. Before that date the
rtlsdr recipe was the registry's, v2.0.2 with three patches, fetched from the
vcpkg git tree like libusb's.

The engine job writes `sources.json` from its own vcpkg tree: each copyleft
port's upstream SHA512 and the SHA256 of every file in its recipe, all read
from `vcpkg.spdx.json`. The `package` and `release` jobs download the pieces
and refuse any byte that does not match. The `package` job builds the archive
on every run and keeps it as the `corresponding-source` artifact, so the first
tag is not its first outing. Built locally on 2026-09-22 against the `dev`
tree: 3.0 MB, and every upstream and recipe hash matched. Built again on
2026-09-23 against the `ci` tree with the librtlsdr overlay: 4,372,784
bytes, and every upstream and recipe hash matched, the seven files of the
overlay among them. Built a third time in the release dry run at `b43d763`:
4,673,080 bytes, every hash matched; the `package` job wrote 4,673,075 on
the same commit. The growth since the second build is Revenant's own
source archive.

## What the first release still owes

The full list, with what the dry run of 2026-09-23 found, is the checklist
under "Release readiness" below. These two were known before it.

**The sources of Qt and FFmpeg.** The client payload conveys Qt 6.8.3 and
FFmpeg 7.1 as DLLs, which is conveying their object code, and LGPL-3.0 and
LGPL-2.1 each ask for the corresponding source to be offered with it. The
notices say where each is published upstream, which is the weaker promise
`docs/clean-room.md` describes, a promise about somebody else's hosting. The
corresponding-source archive does not carry either: Qt's source is hundreds of
megabytes per module and both are built by The Qt Company rather than here.
Whether to carry them, or offer them from a mirror this project controls, is a
decision nobody has made, and it comes due at the first tag.

**The first run of the `release` job.** It has never run, and the corresponding
source step in it is the same command the `package` job runs on every build.

Keeping Qt as DLLs is the one thing here that could be lost by accident.
Shipping Qt beside the executable is the LGPL-3.0 section 4d(1) route and
discharges the relink obligation without a relink package. Anything that stops
a replaced `Qt6Core.dll` being picked up, a static Qt among them, gives that
away and puts the obligation back.

WHAT THIS SECTION USED TO SAY. It was headed "What the container does not
carry yet" and listed three things blocking the first release: a third-party
notices file, which "does not exist"; the verbatim LGPL texts, with `LICENSE`
"the only licence text in the tree"; and the corresponding sources as a release
artefact. All three were built on 2026-09-22 and are described above. It did
not list the Qt and FFmpeg sources, which were owed the whole time.

## Release readiness

A local dry run of the first public release on 2026-09-23, from a clean
worktree at `b43d763`, with `lwforge.exe` and `lwstub.exe` downloaded from the
Forge v0.4.0 release and both reporting `Valid`. Nothing was signed, tagged,
pushed or published.

### How it was run

1. `scripts/build.ps1 -Preset ci -Gpu 0 -NoTest` and the client's `vs` preset,
   both in the worktree.
2. `corresponding_source.py manifest` over `build/ci/vcpkg_installed`, then
   `bundle`, as the `package` job runs them.
3. `stage-payload.ps1 -Only engine` and `-Only client` into two directories,
   merged, then forged with `--dev`, `lwforge inspect`, and `--check-only
   /O:desktop_shortcut=off`.
4. An install. The shipped config is machine scope and would write an HKLM
   Add/Remove Programs entry and an all-users Start Menu shortcut, so the
   install used a variant forged in scratch from the same staged directory:
   `scope = "user"`, `elevation = "on-demand"`, a different product name,
   `aumid` and `upgrade_code`, and no `revenant-engine.exe` preflight, because
   another checkout's engine was running on the machine. It ran unelevated
   through `runas /trustlevel:0x20000` with `/S /D=<scratch directory>`, and
   the uninstall ran the same way from the `QuietUninstallString`.
5. Both programs started out of the installed directory with `PATH` reduced
   to the three Windows directories.

### Done and verified

| Check | Result |
| --- | --- |
| Corresponding source | 4,673,080 bytes; every upstream SHA512 and recipe SHA256 matched the build. `--self-test` passes for both `corresponding_source.py` and `generate_notices.py` |
| Payload | 199 files, 80,325,761 bytes; both notices files, `LICENSE.txt`, `licenses/LGPL-2.1.txt` and `licenses/LGPL-3.0.txt` present |
| Container | `lwforge inspect` ok, 200 members, `[UNSIGNED DEV BUILD]`; `--check-only` lists the one option, unticked |
| Installed bytes | All 199 files SHA-256 identical to the staged copies; the only additions are `.lw\install.manifest` and `.lw\uninstall.exe` |
| Registration | One HKCU Add/Remove Programs key with version 0.1.0, publisher, URL and quiet uninstall string; one Start Menu shortcut to `revenant-ui.exe`; no desktop shortcut, which is `desktop_shortcut` defaulting off |
| Engine from the install | Synthetic source, `--port 0`, its own `--token-file`, `--duration 60`: listened on `127.0.0.1:18263` and nothing else. One module from the install directory; every other module from `C:\Windows` except three implicit Vulkan layers this machine carries (OBS, TikTok LIVE Studio, Aurora), which are the machine and not the payload. Stopped and confirmed gone |
| Client from the install | `--smoke-seconds 12` offscreen against that engine: 36 modules from the install directory, a connection to the engine in `Established`, exit 0, "loaded and ran without a QML warning". `qoffscreen.dll` came from outside, through `QT_QPA_PLATFORM_PLUGIN_PATH`, because the payload carries `qwindows.dll` alone |
| Qt and FFmpeg | `Qt6*.dll` 6.8.3.0, `avcodec-61` 61.19.100 and the other four beside the executable, which keeps the LGPL-3.0 section 4d(1) route. `THIRD-PARTY-NOTICES-client.txt` names both |
| Icon file | `assets/revenant.ico`: 16, 24, 32, 48, 64, 128 and 256 px, all 32 bpp, PNG-compressed entries; Pillow reads all seven as RGBA |
| Icon in the engine | One `RT_GROUP_ICON`, id 1, its seven images byte-identical to `assets/revenant.ico`; VERSIONINFO 0.1.0.0, Locke Werks |
| Icon in the installer | The same seven images, stamped by `lwforge` from `product.icon` |
| Icon in the README | The header block points at `assets/revenant.ico`. No release badge, which is right until there is a release |
| Uninstall | Install directory, Add/Remove Programs key and Start Menu shortcut gone. `PendingFileRenameOperations` 60 entries before and after. One file left, `%TEMP%\lwu847B.tmp.exe`, 489,872 bytes: the stub's copy that finishes the removal, which cannot schedule its own deletion without administrator rights. Forge documents it. An elevated machine-scope uninstall queues it for deletion at the next restart instead. Deleted by hand |
| `FORGE_VERSION` | v0.4.1 in both jobs, moved from v0.4.0 after the second run below. `installer.toml` uses `[[options]]` and `when`, which need v0.3.0, and no hooks, so no key it carries is one either pin would drop |
| Hooks as the user | None declared, so there is nothing to mark `as = "user"`. Nothing in the install writes into a user profile: the RPC token is made by the engine on first run |
| Release job | Now checks `installer.toml`'s version against `cmake/RevenantVersion.cmake`, which only the `package` job did and the `release` job does not wait for, and runs `--check-only` on the signed installer it publishes |

### Blocks a first public release

1. **The Qt and FFmpeg source offer.** The owner's decision.
   `docs/clean-room.md`, open item 2, and "What the first release still owes"
   above.
2. **The M2 physical-monitor frame run.** The owner's run, on a real display.
   Nothing offscreen stands in for it.
3. **`revenant-ui.exe` has no icon and no version resource.** Measured: no
   `RT_GROUP_ICON` and an empty VERSIONINFO. It is the program the Start Menu
   shortcut opens, so the shortcut, the taskbar and Alt-Tab show the generic
   executable icon. `ui/CMakeLists.txt` never calls `revenant_embed_version`,
   and cannot as `cmake/EmbedVersion.cmake` stands: that function finds
   `assets/revenant.ico` and `res/revenant.rc.in` through `CMAKE_SOURCE_DIR`,
   which in the client's own CMake project is `ui/`, and the client includes
   neither `RevenantVersion` nor `EmbedVersion` nor enables RC. `main.cpp`
   sets no window icon either. The fix is in `cmake/` and `ui/`, which belong
   to other lanes.
4. **`AZURE_CLIENT_SECRET` is not set anywhere the `release` job can read
   it.** Checked 2026-09-23 through the API: `Locke-Werks/Revenant` has no
   `release` environment (404), no repository secrets, and the organisation
   lists no secrets. `AZURE_TENANT_ID` and `AZURE_CLIENT_ID` are organisation
   variables visible to all repositories, so those two resolve. The job's
   first signing step would fail. Creating the environment and the secret is
   the owner's.
5. **The `release` job would re-sign 53 binaries that are not ours.** Its
   payload signing step takes every `.exe` and `.dll` under `dist/stage`.
   Measured on the staged payload: 45 are already signed by The Qt Company Oy
   (Qt and the five FFmpeg libraries), 8 by Microsoft (the Visual C++
   runtime), and only `revenant-engine.exe` and `revenant-ui.exe` are
   unsigned. `signtool sign` without `/as` replaces a signature rather than
   adding one, so as written the shipped VC runtime and Qt DLLs would carry
   Specter Point's signature instead of their publishers'. Whether that is
   wanted is a signing decision and was not changed here; narrowing the step
   to the two executables is the change if it is not.

### Owed, not blocking

- **The Add/Remove Programs icon.** `DisplayIcon` is `.lw\uninstall.exe,0`,
  and the stub it points at has no icon resource (checked on the v0.4.0
  `lwstub.exe`), so Settings shows a generic icon. The uninstaller cannot be
  stamped without breaking its signature, so the fix is in Forge, for example
  a way to point `DisplayIcon` at `revenant-ui.exe` once that carries the
  icon.
- **`restart_manager` offers nothing.** Corrected above and in
  `installer.toml`. Forge's `docs/config-schema.md` still describes the key as
  giving the user an option; that file is Forge's to correct.
- **The README has no install section and no release badge.** Both are right
  while there is no release, and both are owed at the tag: the badge per the
  header rule, and a short section naming `Revenant-Setup.exe`, the
  corresponding-source archive beside it, `/S` and `/O:desktop_shortcut=`.
- **The `release` job has never run.** The first tag is its first run. Every
  step before signing has a counterpart in the `package` job that runs on
  each push; the signing steps and `gh release create` do not.

### The Forge pin, moved to v0.4.1

Forge v0.4.1, released 2026-09-16, reports files an uninstall could not remove
where v0.4.0 dropped them silently, and adds no config keys. A second dry run
the same day, from another worktree at `b43d763`, checked it against this
config before the pin moved:

- The same staged payload forged with the `v0.4.0` and `v0.4.1` release
  assets, all four binaries `Valid`: 28,634,304 and 28,635,560 bytes, and
  `lwforge inspect` listing the same 43 config keys and 200 members from each.
- The per-user variant forged with `v0.4.1` and installed with
  `/S /D=<scratch directory> /O:desktop_shortcut=off`: exit 0 in 2.5 s, all
  199 payload files SHA-256 identical to the stage.
- The engine and the client from that install, as in the first run: the
  engine served 25 s of the synthetic source on a free loopback port with its
  own token file and exited 0, and the client's offscreen smoke against it
  exited 0 while the engine counted 306 spectrum frames sent and 0 dropped.
  The client's settings key held 7 subkeys and 0 values before and after.
- `/uninstall /S` from the `v0.4.1` stub: exit 0, with no file reported as
  left behind. The install directory, the HKCU key and the shortcut were gone.
  The one remaining file was the stub's `%TEMP%` copy, 490,896 bytes, queued in
  `PendingFileRenameOperations` for the next restart because that run was
  elevated.

Both jobs in `.github/workflows/ci.yml` pin `v0.4.1` from the commit that
moved it, and have to move together.

WHAT THIS SECTION'S BULLET USED TO SAY, under "Owed, not blocking": "The pin
stays at v0.4.0 until somebody chooses the move." And the `FORGE_VERSION` row
of "Done and verified" read "v0.4.0 in both jobs". Both were true on the first
run and stopped being true when the pin moved.

