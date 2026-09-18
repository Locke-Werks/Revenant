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

C++23, through `/std:c++latest`. `std::expected`, `std::format`, `std::mdspan`
and `std::print` are all in use and all work on MSVC 19.44.

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

The `vs` preset deliberately does not pin a Visual Studio generator version.
dockedconsole pinned one and broke the day a runner moved to a newer Visual
Studio.

`headless` is a real gate, not a curiosity. Qt must never become load-bearing in
`core`, and that requirement does not survive being checked for the first time
late: by then a header is included somewhere quiet and the dependency is
structural. CI runs this on every push.

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
statement. Either open an x64 Native Tools prompt, or use the `vs` preset, which
does not need one.

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

Device selection is explicit and overridable, because the conformance suite runs
the same kernels against every device in the machine and a context that quietly
picks "the best one" cannot be pointed at the integrated GPU to prove a kernel
behaves the same there.

`REVENANT_GPU_INDEX` selects the device by index when no explicit index is
passed in code:

```powershell
$env:REVENANT_GPU_INDEX = 1
ctest --preset ci
```

The index is the position in the order Vulkan enumerates physical devices. On
the development machine and on the CI runner, which are the same box:

| Index | Device | Vulkan |
| --- | --- | --- |
| 0 | NVIDIA GeForce RTX 4090, discrete | 1.4.351 |
| 1 | AMD Radeon integrated graphics, Ryzen 9 7950X | 1.4.315 |

To list what a machine actually has, anything that opens a GPU context takes
`--list-devices`, which prints the same table from
`revenant::gpu::enumerate_devices` in `core/gpu/context.h`, including each
device's maximum workgroup size. The Vulkan SDK's `vulkaninfo --summary` shows
the same devices in the same order and is the independent check when the index
is in doubt.

With the variable unset, selection prefers a discrete GPU, then anything else,
and is stable for a given machine. Nothing in CI relies on that: both
conformance legs pass an explicit index, so a driver update that reorders
enumeration shows up as the wrong device name in the log rather than as one
device silently being tested twice.

Validation layers default to on in a Debug build and off otherwise. They are the
difference between a descriptive error and a driver hang, so leave them on while
writing a kernel.

## Signing

Nothing is signed yet. There is no shippable binary until M3.

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
