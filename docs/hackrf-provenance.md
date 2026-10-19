# libhackrf provenance

What the HackRF backend links, where it comes from, and under which licence.
Roadmap M5 item 1.

## The library

`greatscottgadgets/hackrf` at commit `1cfe7dfe98d333450217d50e3f3a1ad0702e000f`,
the `v2026.01.3` release and the latest on 2026-10-18. Pinned by commit rather
than by tag, as the rtlsdr overlay is, because a tag can be moved.

vcpkg fetches the GitHub archive of that commit and checks it against

    SHA512 b981eae0790f81c43bdedb178a1e2ed40e04152febfc6f1c7a0c5ea539fd3fcba059dfcc5ebec847899318d2d61f247f2c9687ac28dd63e5b0e5384dc4f6ea30

which was computed on 2026-10-18 from a download of
`https://github.com/greatscottgadgets/hackrf/archive/1cfe7dfe98d333450217d50e3f3a1ad0702e000f.tar.gz`.

## Why an overlay

The vcpkg registry has no hackrf or libhackrf port: not at the baseline in
`vcpkg-configuration.json`, and not on `microsoft/vcpkg` master on 2026-10-18.
`vcpkg-overlays/hackrf/` is therefore the recipe, listed in
`vcpkg-configuration.json` beside the rtlsdr overlay.

It does not use upstream's build. `host/libhackrf/CMakeLists.txt` builds the
shared and static libraries together and on Windows installs the import
library, the static library and the DLL all into `bin/`, which vcpkg's
post-build checks refuse. The overlay carries its own `CMakeLists.txt` that
compiles `host/libhackrf/src/hackrf.c` and nothing else, links libusb by path
and PThreads4W by target the way the rtlsdr overlay does, and installs a
`HackRF::hackrf` target.

One change is made to the upstream source, by `vcpkg_replace_string` in the
portfile rather than a patch file, and only for a static build: `hackrf.h`
defines `ADD_EXPORTS` unconditionally on Windows, so every declaration is
`dllexport` and a static link would put libhackrf's whole API in the export
table of `revenant-engine.exe`. The portfile empties `ADDAPI` instead.

## Licence

`hackrf.c` and `hackrf.h` each carry a BSD-3-Clause notice (copyright Great
Scott Gadgets, Jared Boone, Benjamin Vernoux). Those are the only two upstream
files compiled. The repository's root `COPYING` is GPL-2.0 and covers the
firmware, the tools and the CMake files, none of which the port builds; that
root file is why `docs/clean-room.md`'s survey table recorded libhackrf as
GPL-2.0.

The port declares `BSD-3-Clause`, and its installed `share/hackrf/copyright`
is the notice from the head of `hackrf.h` rather than the root `COPYING`.
`licenses/spdx/BSD-3-Clause.txt` already holds the licence text.

Because the licence is permissive, `scripts/corresponding_source.py` does not
put libhackrf in the corresponding-source archive: that archive carries the
GPL and LGPL ports, and BSD-3-Clause asks only that the notice travel with the
binary. libusb and pthreads, which libhackrf pulls in, were already
dependencies through the rtlsdr port.

## Receive only

The backend calls no transmit function, sets no transmit gain and starts no
sweep. `core/source/hackrf_source.h` says why that is a rule.

## What has not been checked against hardware

No HackRF was attached when the backend was written. The gain split,
the parameter limits, the URI layer, enumeration with no device and the cs8
conversion are unit tested in `tests/engine/test_hackrf_source.cpp`. Opening,
streaming, retuning, gain changes on a live device, and the overrun counting
are written against `hackrf.h` and have not run.
