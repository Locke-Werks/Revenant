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
| `scripts/stage-payload.ps1` | Assembles the payload directory out of the two build trees |
| `package` job in `.github/workflows/ci.yml` | Unsigned installer on every build, as an artifact |
| `release` job in the same file | Signed installer on a `v*` tag, published to the release |

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
- Six QML modules copied whole, plus the loose files at the root of `QtQuick`
  and `QtQuick/Controls`. A QML module is its `qmldir`, its `.qmltypes`, its
  `.qml` files and its plugin together, and a partial copy fails at import with
  a message naming a type rather than a file.

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
| `QtQuick/Effects`, `QtQuick/Shapes` | small | `Main.qml` imports `QtQuick`, `QtQuick.Controls`, `QtQuick.Layouts` and `Revenant`, and neither of these |
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
  volume, and neither `revenant-engine.exe` nor `revenant-ui.exe` running, the
  last two with `restart_manager` so an upgrade offers to close them rather
  than refusing. There is no Vulkan preflight: the engine imports
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

`upgrade_code` is deliberately absent from `installer.toml`. It is fixed
forever the first time an installer carrying it reaches anybody, and nothing
has been released, so leaving it out costs only exit code 1618 when two
installers race. It has to be chosen before the first release.

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

## What the container does not carry yet

These block the first release. They do not block the per-build artifact, which
is a test that the packaging works and is not distribution.

1. **A third-party notices file.** `docs/clean-room.md` requires one as a
   payload member beside the binaries, not a link, and four of the linked
   licences ask for one in different words. It does not exist. Both inputs are
   on disk and neither needs writing by hand:
   `build/ci/vcpkg_installed/x64-windows-static/share/<port>/copyright` holds
   each engine dependency's licence text verbatim with `vcpkg.spdx.json` beside
   it, and `C:/Qt/6.8.3/msvc2022_64/sbom/*.spdx.json` holds Qt's own package
   list, 130 packages for qtbase alone, which is the Harfbuzz, FreeType and
   PCRE2 attribution `windeployqt` does not generate.
2. **The verbatim LGPL texts.** Qt is LGPL-3.0 and libusb is
   LGPL-2.1-or-later, and both sections ask for the licence text to travel with
   the work. `LICENSE` in this repository is GPL-3.0 and is the only licence
   text in the tree.
3. **The corresponding sources as a release artefact.** `docs/clean-room.md`
   decided on 2026-09-20 that LGPL-2.1 section 6d and GPL-3.0 section 6d are
   both discharged by offering the binary and the sources from the same
   designated place. A release carrying only `Revenant-Setup.exe` is not that.
   What it owes: libusb v1.0.29 upstream, the vcpkg rtlsdr port's three
   patches, and Revenant's own source archive.

Point 2 is also the one thing the Qt dynamic link buys and could lose. Shipping
Qt as DLLs beside the executable is the LGPL-3.0 section 4d(1) route and
discharges the relink obligation without a relink package. Anything that stops
a replaced `Qt6Core.dll` being picked up, a static Qt among them, gives that
away and puts the obligation back.
