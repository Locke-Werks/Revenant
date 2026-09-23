# Contributing to Revenant

Bug reports and ideas are welcome with no ceremony at all. Open an issue.

Code needs a little more, because this project makes two claims that are only
worth what the discipline behind them is worth: that every GPU kernel is
bit-exact against a scalar twin, and that every component's provenance can be
traced to the document it came from. Both are easy to break by accident and
neither failure is visible in output that looks fine.

## Before your first pull request

Sign the [CLA](CLA.md). One comment on the pull request, the form is at the
bottom of that file. It exists so the project can be relicensed later if there
is ever a reason; it does not take your copyright and it does not change the
licence your contribution ships under, which is GPL-3.0-or-later like
everything else here.

## Provenance, which is the rule most likely to catch you out

Revenant links librtlsdr, which is GPL-2.0-or-later, and is GPL-3.0-or-later
as a result. That does not mean copyleft code can now be pasted in freely.
The rules are in [docs/clean-room.md](docs/clean-room.md) and reduce to three:

**Say where it came from.** Code taken or adapted from anywhere, under any
licence, is disclosed in the pull request. Not doing so is the only thing here
that will get a contribution rejected on sight rather than reviewed.

**Link libraries, do not vendor them.** A dependency goes in `vcpkg.json` with
a version. Copying source files into this tree makes a fork nobody maintains
and a provenance nobody can reconstruct, and CI fails on a copyleft licence
grant appearing under `core/`, `tools/` or `ui/`.

**DSP and decoders stay clean-room.** Where a specification is published, work
from the specification and cite it: document, revision, section or table, in a
comment next to the constant. This is no longer a legal requirement and it is
still the rule, because a filter you can trace to Oppenheim and Schafer is one
the next person can check, and one copied from a project that got it right is
one nobody can. Device backends are the recorded exception, for the reason set
out in [docs/rtlsdr-provenance.md](docs/rtlsdr-provenance.md).

If you have read a copyleft implementation of something you are about to write
clean-room, say so. That is not a disqualification, it is a fact that changes
how the work should be done.

## The code

[docs/conventions.md](docs/conventions.md) is the whole of the house style and
is worth reading once end to end. The parts that come up in review most often:

- Errors are `Expected<T>` and `Status` from `core/error.h`. Every setter
  returns what was achieved rather than void, because a device that lands on a
  nearby frequency and reports success is how a tuning offset becomes somebody
  else's bug.
- Frequency is integer hertz and time is a sample index. Not doubles, not
  wall-clock timestamps.
- The sample path is pure: no clocks read, no allocation, no logging.
- A GPU kernel arrives with its scalar twin and a bit-exact diff test. See
  `tests/reference/test_vrx.cpp` for the shape, including the behavioural
  cases, which are what catch a kernel that is bit-exact against a twin
  implementing the wrong convention.
- Comments explain why. The code already says what.

## Building and testing

[docs/building.md](docs/building.md) has the prerequisites. Then:

```
.\scripts\build.ps1 -Preset dev          # configure, build, test
.\scripts\build.ps1 -Preset ci           # what CI builds, warnings are errors
.\scripts\build.ps1 -Preset headless     # the Qt-free check
```

All three must build and pass before a pull request. The `ci` preset carries
`/WX`, so a warning that the `dev` preset tolerated is a failure there, and
finding that out from CI rather than locally wastes a round trip.

If you have more than one supported Vulkan device, run the suite against each,
by index:

```
.\scripts\build.ps1 -Preset ci -Gpu 0
```

and again with `-Gpu` set to each other index. `revenant-devices` lists them;
docs/building.md has how. Leave out the Radeon integrated graphics in a Ryzen 9
7950X: it is not supported, its driver corrupts the spectrum kernel, and a
failure there says nothing about your change. This section used to say to run
`-Gpu 1` as well, which on the machine it was written on was that device.

Vendors disagree about floating point in ways that only appear on hardware, so
a bit-exactness change verified on one GPU is verified on one GPU. Say in the
pull request which devices you ran on. If you have only one, that is fine and
worth stating rather than leaving to be assumed.

Hardware tests skip when the hardware is absent, so a machine with no radio
and no sound card still runs a green suite. A skip is not a pass: if you
changed something a skipped test covers, find a way to run it.

## Commits and pull requests

Imperative mood, concise subject line, body when the change needs explaining.
Explain why the change is right, not what the diff already shows. No trailers,
no generated boilerplate, no emoji.

A pull request that describes what it verified, and how, gets reviewed faster
than one that describes what it implements.
