# CI

Two workflows. `ci.yml` runs on pushes to `main`, on version tags, and on every pull
request; `nightly.yml` holds the long BER sweeps and has been disabled on GitHub since
2026-10-02, so nothing runs them. "The nightly, disabled" below says why and how.

WHAT THIS PARAGRAPH USED TO SAY: "`nightly.yml` runs the long BER sweeps on a
schedule." True until the owner had the workflow disabled on 2026-10-02.

Note what that leaves out: a push to a topic branch with no pull request open runs
nothing. That is deliberate, because the GPU jobs occupy the workstation, but it means
the first CI a branch sees is when the pull request is opened. Push early if you want
the feedback earlier, or run `scripts/build.ps1 -Preset ci` locally, which is the same
build the runner does.

The maintainer works the same way from the other side. Since 2026-10-02 changes are
committed locally and `main` is pushed only when a release is cut, together with the
version bump, because every push to `main` is a full run of the GPU jobs on the
workstation somebody is using. So a `ci` run on `main` is normally the one in front of
a `v*` tag, and the tag's own run, which adds `release`, follows it.

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
locally and to say which devices they ran on, and a maintainer runs the suite before
merging. A conformance result nobody can trust is worth less than one that is late.

## Jobs

| Job | Runner | What it does |
| --- | --- | --- |
| `guards` | `ubuntu-latest` | Greps for committed signing metadata, build timestamps, hardcoded credentials and vendored copyleft text, checks the version parses, runs `scripts/check_retired_claims.py`, checks every test target suppresses modal dialogs, and runs the self-tests of `generate_notices.py` and `corresponding_source.py`. Cheap, and it answers even when the workstation is off |
| `build-and-test` | self-hosted | Configures and builds the `ci` preset, runs the full suite on the RTX 4090 |
| `headless` | self-hosted | Configures the `headless` preset, which fails if anything under `core/` reaches for Qt |
| `ui` | self-hosted | Configures, builds and tests `ui/`, the Qt client, as its own CMake project |
| `two-process` | self-hosted | Builds `tests/twoprocess`, a small client compiled `/MD` from `core/rpc/client.cpp`, and runs it in its own process against the `/MT` `revenant-engine.exe` that `build-and-test` staged: login, source list, spectrum frames and a receiver's audio across the two runtimes, and, as a second case since 2026-09-27, an engine started with `--no-source` that the client opens a source on, closes and opens again |
| `package` | `windows-latest` | Forges an unsigned installer out of the two halves the jobs above upload, checks both carry their licence material, builds the corresponding-source archive, and keeps both as artifacts. Hosted, because forging needs no toolchain, and unreachable from a fork because the jobs it needs are |
| `release` | `windows-latest` | On a `v*` tag only: checks the tag against the version, signs the payload, forges, signs the installer, and publishes it with the corresponding-source archive and the Qt and FFmpeg source archives. It has published every tag from v0.1.0 on 2026-09-27 to v0.1.6 on 2026-10-03. docs/packaging.md holds the order and why it is that order |
| `sweep` (nightly) | self-hosted | Disabled since 2026-10-02. Sweeps the reference BPSK detector against `tests/baselines/ber-vs-snr.json`, the RDS decoder against `tests/baselines/ber-vs-snr-rds.json`, and eighteen more decoder modes, each against its `tests/baselines/ber-vs-snr-<mode>.json`, failing on a regression in any |

WHAT TWO ROWS OF THIS TABLE USED TO SAY. The `release` row ended "Has never run." It
first ran on the v0.1.0 tag, 2026-09-27, run 36345951513. The `sweep` row read "Runs
two BER sweeps, the reference BPSK detector against `tests/baselines/ber-vs-snr.json`
and the RDS decoder against `tests/baselines/ber-vs-snr-rds.json`, failing on a
regression in either". "Sweep every decoder against its baseline in the nightly" added
a third step on 2026-09-23, and the same day's DMR, AIS and DSC baselines brought it to
the eighteen modes that step lists.

`build-and-test` and the nightly sweep select their device with `REVENANT_GPU_INDEX`,
which is the same mechanism a developer uses locally, and both pin it to 0, the RTX 4090.
The pin matters because the machine also carries the Radeon integrated graphics in its
Ryzen 9 7950X, which is not supported and not tested, and a driver update that reordered
enumeration would otherwise point the suite at it. There is one runner, so the jobs
execute in sequence rather than in parallel. That is fine at this scale and is the reason
the long sweeps were put in a nightly rather than in the per-commit gate. WHAT THAT
SENTENCE USED TO SAY at its end: "the reason the long sweeps are nightly rather than
per-commit." Nothing runs them nightly since 2026-10-02.

Until 2026-09-22 `build-and-test` was a matrix with one leg per device, the second on
the integrated part. That leg is gone; "Coverage, stated honestly" below says why.

### The nightly, disabled

`nightly.yml` still carries its `schedule` trigger, `cron: '0 9 * * *'`, and its
`workflow_dispatch`. What stops it is GitHub's per-workflow switch: the owner had it
disabled on 2026-10-02 with `gh workflow disable`, and `gh workflow list --all` reports
it `disabled_manually`. Nothing in the file says so, which is why this section does.

The cron asks for 09:00 UTC, the small hours in US Central. GitHub started the twelve
scheduled runs from 2026-09-21 to 2026-10-02 between 13:24 and 17:04 UTC instead, which
is the owner's morning at the workstation, and each run held its CPU for 12 to 14
minutes. A different cron hour does not fix that, because the delay is GitHub's. If the
sweeps come back they want a trigger that cannot land while somebody is working: manual
dispatch only, or a check that the box is idle. Ask the owner before re-enabling or
dispatching it. Until then nothing compares a decoder's curve against its baseline
unless somebody runs the commands `docs/sensitivity.md` gives.

### The modal-dialog guard, and the one target exempt from it

`tests/support/no_modal_dialogs.cpp` stops a test process opening a window it then sits
behind. On a desktop that is a popup somebody dismisses; on the runner it is a job that
hangs to its timeout and reports nothing, because the message the process wanted to print
is in a dialog no one will ever see.

Nothing links it automatically. Each target lists the path in its own `add_executable`,
so until today a new target got the file by somebody remembering. `scripts/check_modal_dialogs.py`
reads the source list of every `*_tests` target under `tests/` **and** under `ui/` and
fails on one that does not list it. It scans `ui/` deliberately: the only target that has
ever been without the file is the one in the other CMake project, which is exactly where a
guard rooted in `tests/` would not look. The `guards` job runs it, and so does `ctest` as
`modal_dialogs`, the same arrangement the retired-claims ratchet has.

`revenant_ui_tests` is exempt and named in the script. It opens no Vulkan context and holds
no `abort()` path of its own, and adding the source belongs to `ui/CMakeLists.txt`. The
point of naming it is that the gap is a decision written in the place that checks, rather
than a claim nobody had checked.

The check fails when it finds zero targets, rather than passing having looked at nothing,
and it fails on an `add_executable` whose target name it cannot read rather than skipping it.

**It was an inline `awk` until 2026-09-21, and the `awk` covered less than this paragraph
said.** It pulled the target name off the same line as `add_executable(` and matched
nothing when the name sat on the line below, which CMake allows. Such a target was not
reported, not counted and not exempt: it was absent, and the zero-targets backstop could
never fire on it while the other six were being found. The script's self-test runs that
exact shape every time, which is why the self-test runs before the check in both places.

### Both test steps pass `--no-tests=error`

A bare `ctest` prints `No tests were found!!!` and exits **0**. Measured, not assumed: an
empty `CTestTestfile.cmake` gives exit 0 bare and exit 8 with the flag.

So a `tests/CMakeLists.txt` or a `ui/CMakeLists.txt` that stopped registering targets
would turn a gate into a compile check, and the only sign would be a run that got faster.
Both `ctest` invocations in `ci.yml` carry the flag. It matters most in `ui`, which has
exactly one test target, so the empty set there is one deleted `add_test` away, where
the engine tree has nine `*_tests` targets on 2026-10-03.

WHAT THE LAST SENTENCE USED TO SAY: "one deleted `add_test` away rather than six". True
on 2026-09-20; `revenant_characterise_tests` and `revenant_labelled_tests` made eight by
2026-09-23 and `revenant_transcribe_tests` nine on 2026-10-03.

What the flag catches is zero tests, not too few. `catch_discover_tests` registers one
ctest entry per Catch2 case, so the count moves with every commit and a floor on it would
be a number somebody has to keep raising. `scripts/build.ps1` does not pass the flag: a
person running it reads `No tests were found!!!` on their own screen, which is the case
the flag is not for.

### The dongle, which the runner shares with its owner

The runner is also the owner's development machine, and it has one RTL-SDR. On the
night of 2026-09-22 `build-and-test` failed over and over with `usb_open error -3`
while a local process held the dongle: a test, a `revenant-cli rtlsdr://`, or a
client whose device picker called `listSources`, which opens each radio to describe
it. librtlsdr refusing the second opener was the only arbitration there was, and a
refusal after the fact says nothing about who holds the device or whether waiting
would help.

Every path that opens a dongle now takes one machine-wide lock first, a named mutex
called `Global\Revenant.RtlSdr`, and holds it for as long as the device is open:
`open_rtlsdr_source` until the source is destroyed, which for an engine is until the
stream closes, `describe_rtlsdr_source` for the moment it takes to read the tuner,
and `tools/devicespike` until it exits. `core/source/device_lock.h` has the
mechanism and `core/source/rtlsdr_lock.h` the name and the wait. What each path does
when another process holds it:

| Path | Waits | Then |
| --- | --- | --- |
| A describe, which `listSources` and `--list` reach | not at all | the row reads "in use by another Revenant process" and the listing moves on |
| An open, `openSource`, `revenant-cli`, `revenant-engine` | 5 s | fails naming the lock and saying another process holds the dongle |
| A `dongle` test case | 60 s, or 2 s for ten minutes after one timed out | skips with that reason |

**Global, not Local.** The runner's service is in session 0 and the owner's engine
is in a desktop session, and `Local\` is one namespace per session, so a `Local\`
lock would serialise neither against the other. The mutex's security descriptor
lets every account wait on and release it and nothing more, because the service
account and the owner's account both have to open an object whichever of them
created it.

**A process that dies holding it does not keep the dongle.** Windows releases an
abandoned mutex to the next waiter with `WAIT_ABANDONED`, which counts as acquired,
and when nobody else had the mutex open it goes with the dead process and the next
opener creates it afresh.
The mutex is owned by a thread kept for that purpose rather than by whichever thread
opened the source, because a mutex belongs to a thread, and a source is usually
destroyed on a different one from the one that opened it.

**Holders in one process share it.** It arbitrates between processes. A second open
in the same process still reaches `rtlsdr_open` and is refused there, as
`tests/engine/test_rtlsdr_source.cpp` pins.

**In ctest**, every case that opens the dongle is tagged `[dongle]`, carries the
label `dongle` beside its suite's, and shares `RESOURCE_LOCK rtlsdr`, so no two run
at once even under `-j`. `-LE dongle` runs everything else. A case that skips
because the radio was busy exits with Catch2's skip code, which
`catch_discover_tests` registers as `SKIP_RETURN_CODE 4`, so ctest lists it as
`Skipped` under "The following tests did not run" rather than among the failures.
A busy radio is a fact about the machine, not about the code. The cost is the one
CONTRIBUTING.md names for every skip, that a skip is not a pass: a green run with
dongle skips in it did not test the backend.

`tests/engine/test_device_lock.cpp` covers the lock without a radio: acquire,
contention with a second process, the timeout, release on destruction, a holder
that is killed, and a test double standing in for the device open.

### One case may fail without failing the run, until issue #2

"each emitter of the labelled scene ends with its own label", in
`tests/detect/test_labelled_scene.cpp`, carries Catch2's `[!mayfail]` tag since
2026-10-02. It still runs and still prints every wrong label, but a failure exits 0, so
ctest lists the case as passed and `build-and-test` stays green. It went on after the
case failed three runs in a row on RTTY, seed 3, 16 channels, which held up the release
job behind it ("Let the labelled scene case fail without failing the run until #2").
The RTTY cause was fixed the same day by giving an unverified identification dwell two
more tries; the tag stays for a second one, the M17 row in `core/identify` sometimes
verifying on a D-STAR signal. The comment above the case has both, and the tag comes off
when GitHub issue #2 closes.

A green run therefore says nothing about that case. Read its output in the ctest log
rather than its status.

### Speech to text, and the model the runner does not have

Since 2026-10-03 `build-and-test` builds and runs `revenant_transcribe_tests`, label
`transcribe`, beside the rest. Most of it needs nothing outside the tree: the model
store's size and hash logic and its download's failure paths against loopback, the
segmenter, the resampler, and the transcription queue with its rejection rules against a
fake recogniser. `tests/rpc/test_rpc_transcribe.cpp` runs the server end to end on that
fake, and `tests/engine/test_engine_source_clock.cpp` measures where a receiver's audio
lands on the source clock; both are in the GPU suites they belong to.

The Whisper cases in `tests/transcribe/test_whisper.cpp` need the 1.6 GB model and skip,
saying so, when it is absent. The runner is this machine, which has the model, but the
model lives in `%LOCALAPPDATA%\Revenant\models` of the account that fetched it, and the
runner's service runs as `NT AUTHORITY\NETWORK SERVICE`, whose `%LOCALAPPDATA%` is
`C:\Windows\ServiceProfiles\NetworkService\AppData\Local`. On 2026-10-03 there was no
model there, and `ci.yml` does not set `REVENANT_REQUIRE_WHISPER`, so in CI those cases
skip and Whisper itself is not exercised. Fetching the model into the service's profile
and setting the variable on `build-and-test` closes that; until then a green run says the
pipeline around the recogniser works and nothing about the recogniser. The two
`[.download]` cases and the `[.speed]` measurement are hidden and never run in CI.

The `ui` job picks up `ui/tests/test_transcription.cpp` and
`ui/tests/test_caption_layout.cpp` in `revenant_ui_tests`: the per-receiver choice, the
switch's label and chip, the log line and hover card, and the caption placement, hold,
fade and stacking.

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

### What the `ui` job actually covers

Thin, and stating it exactly is the point of this section. A green run says three things
and no more.

The client compiles, with `REVENANT_WERROR=ON`, which also fires the `static_assert`
declarations in `models/engine_link.h` that nothing else in CI reaches, the confidence
bar's fixed point. `qt_add_qml_module`
runs `qmlcachegen` over every file under `qml/`, so a syntax error in any of them is a red build; nothing
checks what the QML does.

WHAT THE FIRST SENTENCE USED TO SAY: "fires the eight `static_assert` declarations in
`models/engine_link.h`". Seven since 2026-09-23, when "Offer P25, D-STAR and TETRA in the
receiver window's mode selector" moved the demodulator name table and its assertion to
`models/mode_choice.h`, which `revenant_ui_tests` compiles anyway.

`revenant_ui_tests` passes. It holds the pieces of the client that were lifted out of Qt
types so they can be asserted without a window, and the source list in
`ui/CMakeLists.txt` is the authority on which files those are. On 2026-09-22 it was 141
Catch2 cases in 14 files, in three groups:

- `tests/test_audio_ring.cpp` over `audio/audio_ring.cpp`. Gap fill, resync past a
  ring-length gap, overrun eviction, a starved read, a read at the wrong format, the gate,
  stereo counted in frames, and the timeline accounting for every frame the engine
  indexed.
- `tests/test_history_resize.cpp` over `render/history_resize.h`. The waterfall's ring
  arithmetic across a grow, a shrink and a no-op resize, including after the cursor has
  wrapped.
- Twelve files over the model logic that sits in Qt-free headers under `models/`:
  bookmarks, the RDS text mapping, frequency entry, scroll tuning, receiver matching and
  markers, what happens when a receiver goes away, source choice and pacing, gain
  control, the front-end note and the composite probe.

The count is dated rather than kept current because it moves with nearly every commit.
`ctest --preset vs -N` from `ui/` prints today's.

WHAT THIS SECTION USED TO SAY, twice. First "`revenant_ui_tests` is one file over
`AudioRing`." Two files, and the second is not `AudioRing`: the job and
`test_history_resize.cpp` were written thirteen minutes apart on separate lanes and met
in a merge. Then "`revenant_ui_tests` passes: 26 Catch2 cases in two files", which was
true when written and stayed in place while twelve more files arrived beside them.

### What is still unguarded in `ui/`

Everything below has no test of any kind. Compilation is the only thing standing over it.

The scale arithmetic left this list on 2026-09-22. `render/spectrum_scale.cpp` became the
header `render/spectrum_scale.h`, and `tests/test_spectrum_scale.cpp` covers
`reduce_peak`, `peak_reduction_headroom_db`, `map_ends`, `resolve_ends`, `pin_level`,
`colour_at` and `colour_argb_at`, the max-of-K floor correction among them.

WHAT THIS LIST USED TO SAY first: "**`render/spectrum_scale.cpp` is the one that should
not be on this list.** ... It needs a test file and one line in the target, nothing
more." It got both in "Pin either end of the span colour map, and test the scale
arithmetic", two days after this section was written, and the paragraph stayed.

**`models/engine_launcher.cpp`**, since 2026-09-27. Starting `revenant-engine.exe` in a
Job object that ends it with the window, and reading its stderr. It talks to Windows,
which is why it is its own file; the decision whether to start one is
`models/engine_start.h`, and `tests/test_engine_start.cpp` covers that.

**`models/engine_link.cpp`, `models/receiver_link.cpp`, `models/audio_link.cpp`,
`models/transcribe_link.cpp`.** The RPC-facing state: connection lifecycle, reconnect,
the detection, RDS and speech to text surfaces, and what happens when the engine goes
away mid-stream. These hold Qt types and want an event
loop and a server on the other end, so covering them is a harness, not a test file.

**`render/spectrum_item.cpp`, `render/waterfall_item.cpp`, `render/passband_item.cpp`.**
Scene graph nodes. A headless agent cannot open a window, and the geometry they build is
only meaningful once something rasterises it.

**`audio/audio_player.cpp`.** WASAPI in shared mode. Needs a sound card.

**`main.cpp` and the files under `qml/`.** Wiring and layout.

None of this is covered by the engine tree's tests, however many there are on the day:
`ui/` links no part of the engine and talks to it over a socket. This sentence used to
say "the engine tree's 361 tests", a count that was 590 by 2026-09-22.

## Coverage, stated honestly

The suite runs on an NVIDIA RTX 4090, so one of the three target vendors gets real driver
coverage on every run.

**AMD is not covered.** The runner also carries the Radeon integrated GPU on its Ryzen 9
7950X, and that device is not supported and not tested. Its driver corrupts the spectrum
kernel at a rate that climbs by two orders of magnitude once several dispatches share a
command buffer, which is what the engine records, and in run 35813177662 on 2026-09-22
its leg failed in `tests/rpc/test_rpc_detect.cpp` on a detector that never went quiet.
The kernel's
arithmetic is exact on the 4090 over the same work. It was dropped from CI that day rather
than chased; `docs/fft.md` keeps the measurements. There is no discrete AMD card in the
machine, so AMD's gap stays open until one appears.

**Intel is not covered.** There is no Intel GPU in the machine and none available. This
is a real gap in the conformance matrix, not an oversight, and it stays open until
hardware appears. macOS and MoltenVK are out of scope until there is something to render.

WHAT THIS SECTION USED TO SAY: "The runner has an NVIDIA RTX 4090 and the AMD Radeon
integrated GPU on a Ryzen 9 7950X, so two of the three target vendors get real driver
coverage on every run." True until 2026-09-22, when the integrated part left the matrix.

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
Vulkan works from the service account on the NVIDIA device, and the full suite passes. It
enumerated the integrated AMD device too while that was in the matrix. No change was
needed, and the runner does not need to run as an interactive user.

## Cost

WHAT THIS PARAGRAPH USED TO SAY: "The repository is private, so Actions minutes are
metered, but only `guards` consumes them." The repository is public, which the fork-gate
section above says in the sentence that explains why the gate exists, and a public
repository's hosted-runner minutes are not metered at all. Both halves of that sentence
were wrong and they pointed opposite ways, so nobody reading either one alone would have
caught it.

Nothing here costs GitHub minutes. `guards` runs on `ubuntu-latest`, which is free on a
public repository, and every other job runs on hardware that is already paid for. What
those jobs do cost is the workstation, which is why the long sweeps are not in the
pull-request gate, why the nightly that held them is disabled, and why `main` is pushed
only at a release: they take real time on a machine somebody is using.

WHAT THE FIRST OF THOSE REASONS USED TO SAY: "which is why the nightly is scheduled
rather than frequent". Scheduled turned out to mean mid-morning, and the nightly was
disabled on 2026-10-02; "The nightly, disabled" above has the times.

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
