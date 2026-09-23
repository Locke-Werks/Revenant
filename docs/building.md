# Building

Windows 11, x64. The versions below are what is installed on the development
machine and what CI runs against.

## Prerequisites

| Tool | Version | Notes |
| --- | --- | --- |
| Visual Studio 2022 | 17.14, toolset v143 | Community is fine. The "Desktop development with C++" workload. |
| CMake | 4.3.4 | Presets version 6, so anything older than 3.28 will not read `CMakePresets.json`. |
| Ninja | any current release | See the trap below. The one on `PATH` is probably the wrong one. |
| LunarG Vulkan SDK | 1.4.350 | `VULKAN_SDK` must be set. `glslangValidator` and `spirv-val` come from here. |
| vcpkg | at `C:\vcpkg` | `VCPKG_ROOT` must be set. Manifest mode, so dependencies come from `vcpkg.json`. |
| Qt | 6.8.3, kit `msvc2022_64` | The `ui/` build only. The engine, the tools and the tests do not need it and must never find it. |

C++23, through `/std:c++latest`. `std::expected`, `std::format` and
`std::print` are in use across the engine and the tools. `std::mdspan` is the
convention for sample buffers once there are strided views worth taking, per
docs/conventions.md, and is not used yet. All four are available on MSVC 19.44
under that flag.

Nothing needs to be installed by hand from vcpkg. The toolchain file in the
preset reads `vcpkg.json` and the baseline pinned in
`vcpkg-configuration.json`, and builds what is missing on first configure. That
first configure is slow. Later ones are not.

## Presets

```powershell
cmake --preset vs          # configure
cmake --build --preset vs  # build
ctest --preset vs          # test
```

| Preset | Generator | Config | For |
| --- | --- | --- | --- |
| `vs` | Visual Studio 17 | Debug | Works from any shell, no `vcvars` needed. Use this if in doubt. |
| `dev` | Ninja | Debug | Day to day. Needs an x64 Native Tools prompt. |
| `ci` | Ninja | RelWithDebInfo | What CI builds. `REVENANT_WERROR=ON`, so warnings are errors. |
| `headless` | Ninja | Debug | Configures with the UI excluded and asserts that nothing in `core` can reach Qt. |

Each preset builds into `build/<preset>/`, so they do not collide and you can
keep more than one alive.

`scripts/build.ps1` runs any of them from any shell. It finds `vcvars` itself,
checks `VCPKG_ROOT` and `VULKAN_SDK` are set before it starts rather than
failing halfway through with an unrelated message, and takes a device index:

```powershell
.\scripts\build.ps1                    # configure, build and test dev
.\scripts\build.ps1 -Preset ci         # the same for ci
.\scripts\build.ps1 -Gpu 0             # run the suite against device 0
.\scripts\build.ps1 -Clean -NoTest     # rebuild from scratch, skip the tests
```

The `vs` preset deliberately does not pin a Visual Studio generator version.
dockedconsole pinned one and broke the day a runner moved to a newer Visual
Studio.

`headless` is a real gate, not a curiosity. Qt must never become load-bearing in
`core`, and that requirement does not survive being checked for the first time
late: by then a header is included somewhere quiet and the dependency is
structural. CI runs this on every push.

### Running the suite while the radio is in use

The cases that open the RTL-SDR are labelled `dongle`, so one flag leaves them
out:

```powershell
ctest --preset ci -LE dongle      # everything that does not open the dongle
ctest --preset ci -L dongle       # only the ones that do
```

That replaces the `-E 'dongle|rtlsdr|RTL|consumer that cannot keep up|describing every device'`
pattern people had been typing, which matched by name. On 2026-09-23 it let
through four cases that open the dongle, "tuning reports where the tuner
landed", "a manual gain snaps to a step the tuner has" and the two source
listing cases in `tests/rpc/test_rpc_session.cpp`, and it dropped four that open
nothing, three URI cases and "a recording states the grid it needs and a dongle
does not".

ctest never runs two `dongle` cases at once, even with `-j`: they share
`RESOURCE_LOCK rtlsdr`. Across processes every opener takes the machine-wide
lock `Global\Revenant.RtlSdr` first, the engine included, so a case started
while an engine or a `revenant-cli` is streaming waits up to a minute for the
radio and then skips saying another Revenant process holds it. After one such
skip the rest wait two seconds each for ten minutes, so a run against a busy
radio is not twenty minutes of waiting. `REVENANT_DONGLE_WAIT_S` sets the wait
in seconds, 0 included. docs/ci.md has why it is built this way.

## Two traps, both of which have cost time

**Outside an x64 Native Tools prompt, CMake with the Ninja generator finds the
wrong compiler.** A Strawberry Perl installation puts `C:\Strawberry\c\bin` on
`PATH`, and that directory contains a MinGW-w64 `g++` 13.2.0. With no `cl` in
the environment, CMake's compiler search finds that one and configures happily,
and you get the whole project built with the wrong toolchain against a vcpkg
triplet that does not match it. The symptom is a wall of link errors that look
like a dependency problem.

The `dev`, `ci` and `headless` presets pin `CMAKE_CXX_COMPILER` to `cl` so this
now fails immediately, saying it cannot find the compiler, which is the true
statement. Three ways past it: open an x64 Native Tools prompt, use
`scripts/build.ps1`, which sources `vcvars` for you, or use the `vs` preset,
which needs neither.

**A failed configure poisons the build directory, so fixing the environment is
not enough on its own.** CMake caches the compiler check, and re-running
`cmake --preset dev` from a correct shell reuses the cache and fails again with
the same message, which reads as though the fix did not work. Delete the
directory and configure again:

```powershell
Remove-Item -Recurse -Force build\dev
.\scripts\build.ps1 -Preset dev
```

`scripts/build.ps1 -Clean` does the same thing in one step.

**The only `ninja` on `PATH` also comes from Strawberry Perl.** Same directory,
version 1.12.0. It works, and depending on a Perl distribution for the build
tool is a trap waiting for the day somebody uninstalls Perl. Install a real
Ninja, from the Visual Studio C++ workload, from `winget install Ninja-build.Ninja`,
or from the release on GitHub, and put it ahead of Strawberry on `PATH`.

Check what you are actually getting:

```powershell
(Get-Command cl, ninja, cmake -ErrorAction SilentlyContinue).Source
```

## Choosing a GPU

Device selection is explicit and overridable, because a machine can carry more
than one device and a context that quietly picks "the best one" cannot be
pointed at a particular one to prove a kernel behaves the same there.

`REVENANT_GPU_INDEX` selects the device by index when no explicit index is
passed in code:

```powershell
$env:REVENANT_GPU_INDEX = 0
ctest --preset ci
```

The index is the position in the order Vulkan enumerates physical devices. On
the development machine and on the CI runner, which are the same box:

| Index | Device | Vulkan | Status |
| --- | --- | --- | --- |
| 0 | NVIDIA GeForce RTX 4090, discrete | 1.4.351 | Supported, tested in CI |
| 1 | AMD Radeon integrated graphics, Ryzen 9 7950X | 1.4.315 | **Not supported, not tested** |

Index 1 is still enumerated because the part is still in the machine. Its
driver corrupts the spectrum kernel when dispatches share a submission, which
`docs/fft.md` measured, and on 2026-09-22 it was dropped from CI and from
support. Pointing the suite at it will show failures that say nothing about
this tree.

This section used to say the conformance suite "runs the same kernels against
every device in the machine", and its example set `REVENANT_GPU_INDEX = 1`,
which pointed at the integrated part. Both were true of CI until 2026-09-22.

To list what a machine actually has, build and run `revenant-devices`:

```
build\dev\tools\devices\revenant-devices.exe
[0] NVIDIA GeForce RTX 4090 (NVIDIA, discrete, Vulkan 1.4.351)
[1] AMD Radeon(TM) Graphics (AMD, integrated, Vulkan 1.4.315)
```

`--verbose` adds the vendor and device ids, the driver version and each
device's workgroup limits. It enumerates without opening a device, so it still
answers on a machine where creating a context fails, which is when the question
usually comes up. The Vulkan SDK's `vulkaninfo --summary` lists the same
devices in the same order and is the independent check when an index is in
doubt.

With the variable unset, selection prefers a discrete GPU, then anything else,
and is stable for a given machine. Nothing in CI relies on that:
`build-and-test` and the nightly sweep pin index 0, so a driver update that
reorders enumeration shows up as the wrong device name in the log rather than
as the unsupported part being tested in silence.

### A value the test suite cannot read is an error, not a fallback

`index_from_environment` in `core/gpu/context.cpp` returns an optional, so a
value it cannot parse is indistinguishable there from the variable being
unset, and selection falls back to "pick the best device". On this machine that
is index 0. A run meant for another device would then run the discrete one and
report a green run for a device that was never touched.

The test suite does not accept that. `tests/reference/gpu_fixture.cpp` reads the
variable again with `std::from_chars` and every GPU case fails, with the reason,
on a value it cannot read. It then checks that the device the context opened is
the index that was asked for, so an index that parsed but was not honoured fails
too.

The fixture is the stricter of the two readers, measured by running both forms
over the same strings. `std::from_chars` takes neither leading whitespace nor a
leading `+`, so `" 1"` and `"+1"` select device 1 for the engine and are a
configuration error for the suite. Both agree on `1`, `0` and `01`, and both
reject `one`, `-1` and anything with trailing text.

### `REVENANT_REQUIRE_GPU`

GPU cases skip when no device can be opened, so that a developer without Vulkan
still gets the rest of the suite. On a runner that is the wrong behaviour: a
driver that has quietly died would report a green build having tested nothing on
the device.

Set `REVENANT_REQUIRE_GPU=1` and a missing device is a failure naming the
driver's own message instead of a skip. CI's `build-and-test` sets it. Set it
locally whenever a green run is supposed to mean the kernels were actually
executed.

It does not govern the configuration errors above. Those fail whatever it is set
to, because a skip there is the outcome the variable exists to prevent.

Validation layers default to on in a Debug build and off otherwise. They are the
difference between a descriptive error and a driver hang, so leave them on while
writing a kernel.

## The UI is a second build

`ui/` is its own CMake project, configured separately against a different vcpkg
triplet and a different C runtime. Everything above builds `/MT` against
`x64-windows-static`; the Qt client builds `/MD` against `x64-windows`.
docs/rpc.md has the reasoning, the `dumpbin` measurements behind it, and the
rule about what `core/rpc/client.cpp` is allowed to include.

`REVENANT_BUILD_UI` in the root `CMakeLists.txt` is not the switch for this. It
fails the configure and says to configure `ui/` separately, and it will not
become the switch, because the main tree's cache holds the wrong triplet and
the wrong runtime library. Until 2026-09-20 the message it printed said the
QML client arrives at M2, which stopped being the reason once the client was
built; the refusal was right either way.

From an x64 Native Tools prompt, same as `dev` and `ci`, for the same
Strawberry Perl reason:

```powershell
cmake -S ui -B build\ui -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL `
  -DCMAKE_PREFIX_PATH="C:\Qt\6.8.3\msvc2022_64" `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build\ui
```

`scripts/build.ps1` drives the presets in `CMakePresets.json` and knows nothing
about this tree. Do not point it here.

Two ways to get it wrong:

- Configuring `ui/` into `build\dev`, or any other main build directory. The
  cache there already holds `x64-windows-static` and `MultiThreaded`, CMake
  reuses both, and the configure succeeds. What comes out is Qt linked against
  the static CRT, which fails at link if you are lucky and at runtime if you
  are not.
- Expecting to link `revenant_rpc_client` from the main build. That library is
  `/MT` and unusable here. `ui/` compiles `core/rpc/client.cpp` and the
  generated schema sources itself. Build the main tree first regardless, since
  that is what proves the schema still generates.

`ui/CMakePresets.json` holds `vs`, `ninja` and `ninja-debug`, all against
`x64-windows` and the dynamic CRT, so `cmake --preset vs` from inside `ui/` is
the short form of the command above. This paragraph used to say `ui/` held
three placeholders, that there was no `ui/CMakeLists.txt`, and that the
commands above were the shape of a build rather than one that runs. All three
were true until the client was built and none is now.

### Running the client's tests

From inside `ui/`:

```powershell
cmake --preset vs
cmake --build --preset vs
ctest --preset vs
```

Each configure preset in `ui/CMakePresets.json` has a test preset of the same
name, and each one fails on an empty test set rather than reporting success
with nothing run. The paragraph here used to say `ui/CMakePresets.json` has no
test preset and give the build directory and configuration by hand; that was
true until the presets were added on 2026-09-22.

That is `revenant_ui_tests`: the pieces of the client lifted out of Qt types so
they can be asserted without a window. On 2026-09-22 it was 141 cases in 14
files, and docs/ci.md says what they cover. It links no Qt: the rule in `ui/CMakeLists.txt` is that
anything pulled out for a test goes in a header or a `.cpp` with no Qt type in
it, so a test target that started linking Qt would be the sign that rule had
been broken. The target is skipped, with a message, when Catch2 is missing from
the `x64-windows` triplet; `revenant-ui` itself still builds.

CI runs this as the `ui` job. Until 2026-09-20 nothing did, and the engine
tree's suite says nothing about the client, which links no part of the engine
and reaches it over a socket. docs/ci.md lists what that job covers and, file
by file, what it does not.

### The two-process smoke test

`tests/twoprocess` is a third CMake project, for the reason `ui/` is one: it
builds `core/rpc/client.cpp` `/MD` into a small client, and one cache cannot
hold both runtimes. Its one test starts `revenant-engine` from the root tree
on a synthetic source, reads the port it prints, and runs the client against
it in a separate process. From inside `tests/twoprocess`, with the root tree
built:

```powershell
cmake --preset vs -DREVENANT_ENGINE=..\..\build\ci\tools\engined\revenant-engine.exe
cmake --build --preset vs
$env:REVENANT_GPU_INDEX = 0
ctest --preset vs
```

`REVENANT_ENGINE` defaults to `build/ci`'s engine, and the configure warns if
nothing is there. The test needs a GPU, because the engine does. On 2026-09-22
against a `dev` engine it logged in, listed 2 sources, took 22 spectrum frames
of 8192 bins and 8 audio chunks from an NFM receiver, and passed in 1.9 s. CI
runs it as the `two-process` job against the engine `build-and-test` staged.

## Packaging and signing

docs/packaging.md is the whole of it: what the installer carries, how to build
one locally, and the order the release job signs in. This section is the two
sentences somebody looking for a certificate needs.

Nothing is signed yet. The `release` job in `.github/workflows/ci.yml` can sign
and has never run; `package` produces an unsigned development installer on
every build.

The scaffold is in place: `signing/signing.env` holds the endpoint, account and
certificate profile, and `scripts/New-SigningMetadata.ps1` generates
`signing/metadata.json` from it.

```powershell
./scripts/New-SigningMetadata.ps1 -Force
```

The generated file is gitignored and CI fails if it is ever committed, which is
how the profile name stays in one place. Signing authenticates with
`AZURE_TENANT_ID`, `AZURE_CLIENT_ID` and `AZURE_CLIENT_SECRET` from the
environment and nothing else; the `ExcludeCredentials` list in
`signing/metadata.json.in` is what forces that. Do not go looking for a
federated identity, there is not one.
