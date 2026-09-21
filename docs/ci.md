# CI

Two workflows. `ci.yml` runs on pushes to `main`, on version tags, and on every pull
request; `nightly.yml` runs the long BER sweeps on a schedule.

Note what that leaves out: a push to a topic branch with no pull request open runs
nothing. That is deliberate, because the GPU legs occupy the workstation, but it means
the first CI a branch sees is when the pull request is opened. Push early if you want
the feedback earlier, or run `scripts/build.ps1 -Preset ci` locally, which is the same
build the runner does.

## Why the GPU jobs are self-hosted

The most important thing this project tests is that every GPU compute kernel produces
bit-identical output to its CPU reference, on every vendor's driver. GitHub-hosted
runners have no discrete GPU, so that suite cannot run there in any meaningful form. A
software Vulkan implementation would exercise the API and prove nothing about the drivers
the divergences actually come from.

So the GPU jobs run on a self-hosted runner, and that runner is the maintainer's own
workstation.

## The fork gate, which is the thing not to break

A self-hosted runner is not a sandbox. It is a persistent machine with a real
filesystem, a real network position and whatever is in its environment, and a workflow
can run anything at all on it. While this repository was private that was fine, because
only somebody with write access could trigger a run.

The repository is public now, so anyone can fork it, change a build script or the
workflow itself, open a pull request, and have that execute here. It is one of the
best-known ways to lose a machine and GitHub's own guidance is not to pair self-hosted
runners with public repositories without a gate.

Two gates, because one of them is a human being clicking a button while tired.

The one in the file: both self-hosted jobs in `ci.yml` carry

```yaml
if: github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository
```

so a pull request from a fork skips every job that would touch the workstation. Such a
pull request still runs `guards` on a hosted runner, which is ephemeral and disposable,
so a contributor still gets the cheap checks. Conformance runs when a maintainer pushes
the branch to this repository, which is also the point at which somebody has read the
diff.

The one in the web UI: Settings, Actions, General, "Fork pull request workflows from
outside collaborators", set to **require approval for all outside collaborators**. The
default on a public repository is to require approval only for first-time contributors,
which means one merged typo fix buys somebody unattended execution on the workstation
from then on.

The consequence is honest and worth stating: an outside contributor's pull request does
not get GPU verification. `CONTRIBUTING.md` asks contributors to run all three presets
locally and to say which devices they ran on, and a maintainer runs the matrix before
merging. A conformance result nobody can trust is worth less than one that is late.

## Jobs

| Job | Runner | What it does |
| --- | --- | --- |
| `guards` | `ubuntu-latest` | Greps for committed signing metadata, build timestamps, hardcoded credentials and vendored copyleft text, checks the version parses, and checks every test target suppresses modal dialogs. Cheap, and it answers even when the workstation is off |
| `build-and-test` | self-hosted | Configures and builds the `ci` preset, runs the full suite. A matrix over the two GPUs, one leg per device |
| `headless` | self-hosted | Configures the `headless` preset, which fails if anything under `core/` reaches for Qt |
| `ui` | self-hosted | Configures, builds and tests `ui/`, the Qt client, as its own CMake project |
| `sweep` (nightly) | self-hosted | Runs the BER sweep and compares against `tests/baselines/ber-vs-snr.json`, failing on a regression |

The matrix legs select their device with `REVENANT_GPU_INDEX`, which is the same
mechanism a developer uses locally. There is one runner, so the legs execute in sequence
rather than in parallel. That is fine at this scale and is the reason the long sweeps are
nightly rather than per-commit.

### The modal-dialog guard, and the one target exempt from it

`tests/support/no_modal_dialogs.cpp` stops a test process opening a window it then sits
behind. On a desktop that is a popup somebody dismisses; on the runner it is a job that
hangs to its timeout and reports nothing, because the message the process wanted to print
is in a dialog no one will ever see.

Nothing links it automatically. Each target lists the path in its own `add_executable`,
so until today a new target got the file by somebody remembering. The `guards` step reads
the source list of every `*_tests` target under `tests/` **and** under `ui/` and fails on
one that does not list it. It scans `ui/` deliberately: the only target that has ever been
without the file is the one in the other CMake project, which is exactly where a guard
rooted in `tests/` would not look.

`revenant_ui_tests` is exempt and named in the step. It opens no Vulkan context and holds
no `abort()` path of its own, and adding the source belongs to `ui/CMakeLists.txt`. The
point of naming it is that the gap is a decision written in the place that checks, rather
than a claim nobody had checked.

The step fails when it finds zero targets, rather than passing having looked at nothing.

### Both test steps pass `--no-tests=error`

A bare `ctest` prints `No tests were found!!!` and exits **0**. Measured, not assumed: an
empty `CTestTestfile.cmake` gives exit 0 bare and exit 8 with the flag.

So a `tests/CMakeLists.txt` or a `ui/CMakeLists.txt` that stopped registering targets
would turn a gate into a compile check, and the only sign would be a run that got faster.
Both `ctest` invocations in `ci.yml` carry the flag. It matters most in `ui`, which has
exactly one test target, so the empty set there is one deleted `add_test` away rather
than six.

What the flag catches is zero tests, not too few. `catch_discover_tests` registers one
ctest entry per Catch2 case, so the count moves with every commit and a floor on it would
be a number somebody has to keep raising. `scripts/build.ps1` does not pass the flag: a
person running it reads `No tests were found!!!` on their own screen, which is the case
the flag is not for.

## The `ui` job, and why it is a job rather than a step

`ui/` is a second CMake project. Qt 6.8.3 is built against the dynamic CRT and the
engine is built `/MT`, so one cache cannot hold both: the root `CMakeLists.txt` refuses
`REVENANT_BUILD_UI` outright and the client configures on its own against the
`x64-windows` triplet. That is why it gets a job instead of a step in `build-and-test`.

It needs three things from the runner that no other job does. Qt 6.8.3 at
`C:/Qt/6.8.3/msvc2022_64`, which `ui/CMakePresets.json` pins as `CMAKE_PREFIX_PATH`; a
step checks for it by hand so a Qt upgrade on the runner reports itself in one line
rather than as a `find_package` failure deep in a configure log. `capnproto` and
`catch2` in the `x64-windows` triplet, from `ui/vcpkg.json`, which are a different set of
packages from the static ones the engine legs build. And nothing else: this target links
no Vulkan and opens no device. It carries the `gpu` label only because that is the label
on the one self-hosted Windows runner, the same reason `headless` does.

The job passes `-DREVENANT_WERROR=ON` to match the engine's `ci` preset.
`ui/CMakeLists.txt` defaults that off so a developer's first build of the client is not a
wall of errors out of a Qt header; a merge gate is the other case.

What it covers is thin and stating that is the point. `revenant_ui_tests` is one file over
`AudioRing`. A green run says the client compiles and that the audio timeline arithmetic
holds. It says nothing about the QML, the scene graph, or the audio device, none of which
a headless agent can open.

## Coverage, stated honestly

The runner has an NVIDIA RTX 4090 and the AMD Radeon integrated GPU on a Ryzen 9 7950X,
so two of the three target vendors get real driver coverage on every run.

**Intel is not covered.** There is no Intel GPU in the machine and none available. This
is a real gap in the conformance matrix, not an oversight, and it stays open until
hardware appears. macOS and MoltenVK are out of scope until there is something to render.

## Registering the runner

Needs an elevated shell. Download the current runner from
`https://github.com/actions/runner/releases`, extract it, then:

```powershell
$token = gh api -X POST repos/Locke-Werks/Revenant/actions/runners/registration-token --jq '.token'
.\config.cmd --unattended `
    --url https://github.com/Locke-Werks/Revenant `
    --token $token `
    --name valkyrie-gpu `
    --labels gpu `
    --runasservice
```

`self-hosted`, `Windows` and `X64` are applied automatically; only `gpu` has to be asked
for. The workflows require all four.

### Two things that will break it, and did

**`VCPKG_ROOT` must be a machine variable, not a user one.** The service runs as
`NT AUTHORITY\NETWORK SERVICE`, which does not inherit a user's environment. The
`base` preset builds its toolchain path from `$env{VCPKG_ROOT}`, so with a user-scoped
variable that path resolves to `/scripts/buildsystems/vcpkg.cmake` and the configure
fails somewhere that does not mention vcpkg at all.

```powershell
[Environment]::SetEnvironmentVariable('VCPKG_ROOT','C:\vcpkg','Machine')
```

**The service account needs write access to the vcpkg tree.** vcpkg writes downloads and
buildtrees under `VCPKG_ROOT`, not only into the build directory.

```powershell
icacls C:\vcpkg /grant "NT AUTHORITY\NETWORK SERVICE:(OI)(CI)M" /T /C
```

Restart the service after either change; it reads the machine environment at start.

```powershell
Restart-Service actions.runner.Locke-Werks-Revenant.valkyrie-gpu -Force
```

`VULKAN_SDK` was already machine-scoped and needed nothing. Worth checking rather than
assuming, because the failure looks identical.

### What did not turn out to be a problem

Session 0 isolation. A Windows service runs with no desktop, and the worry was that
Vulkan would refuse to enumerate a physical device from there. It does not: compute-only
Vulkan works from the service account on both the NVIDIA and the AMD device, and the
full suite passes. No change was needed, and the runner does not need to run as an
interactive user.

## Cost

WHAT THIS PARAGRAPH USED TO SAY: "The repository is private, so Actions minutes are
metered, but only `guards` consumes them." The repository is public, which the fork-gate
section above says in the sentence that explains why the gate exists, and a public
repository's hosted-runner minutes are not metered at all. Both halves of that sentence
were wrong and they pointed opposite ways, so nobody reading either one alone would have
caught it.

Nothing here costs GitHub minutes. `guards` runs on `ubuntu-latest`, which is free on a
public repository, and every other job runs on hardware that is already paid for. What
those jobs do cost is the workstation, which is why the nightly is scheduled rather than
frequent and why the long sweeps are not in the pull-request gate: they take real time on
a machine somebody is using.

A scheduled workflow on a self-hosted runner only fires when the machine is on. That is
accepted here, not a fault to chase.

## When the runner is the problem

```powershell
Get-Service actions.runner.*
gh api repos/Locke-Werks/Revenant/actions/runners --jq '.runners[] | "\(.name) \(.status) busy=\(.busy)"'
```

Runner logs are under `_diag/` in the runner directory. A job that never starts is
usually a label mismatch: compare the `runs-on` list in the workflow against the labels
the API reports.
