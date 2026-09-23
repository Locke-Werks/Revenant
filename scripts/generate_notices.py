#!/usr/bin/env python3
"""Write the third-party notices a Revenant binary has to carry.

Two programs ship in one installer and they are built by two CMake projects
against two vcpkg triplets, so there are two notices files, one per program,
each generated from the tree that built it:

    generate_notices.py engine --vcpkg build/ci/vcpkg_installed/x64-windows-static
                               --out dist/stage/THIRD-PARTY-NOTICES-engine.txt
    generate_notices.py client --vcpkg ui/build/vs/vcpkg_installed/x64-windows
                               --qt C:/Qt/6.8.3/msvc2022_64
                               --deploy ui/build/vs/RelWithDebInfo
                               --out dist/stage/THIRD-PARTY-NOTICES-client.txt

scripts/stage-payload.ps1 runs it for each half it stages, so the notices in a
payload always describe the binaries beside them.

WHY GENERATED AND NOT WRITTEN

docs/clean-room.md decided this: a hand-written notices file is correct on the
day it is written and wrong the first time a port moves a version. Everything
the file needs is already on disk after a build. Each vcpkg port installs its
licence text verbatim as share/<port>/copyright, and share/<port>/vcpkg.spdx.json
beside it carries the version, the declared SPDX licence and the upstream
source location. Qt ships an SPDX document per module under sbom/, which names
the Harfbuzz, FreeType, PCRE2 and fifty-odd other bundled components that
windeployqt copies and does not attribute.

WHAT IT REFUSES TO DO

It fails, naming the port, when a port vcpkg says is installed has no
copyright file or no SPDX document. A notices file with a hole in it looks
complete, and the hole is found by whoever reads it closely, which is the
wrong person.

It fails when a Qt attribution names an SPDX licence with no text under
licenses/spdx/. A Qt upgrade that brings a new licence stops the stage there,
where the message names the identifier, rather than shipping a notice that
says "MIT" with no MIT text anywhere in the install.

It refuses to write under core/, tools/ or ui/. The guards job in ci.yml fails
the build on any SPDX identifier other than this project's own in those three
directories, and a notices file is a list of exactly those.

THE LICENCE TEXTS IT READS

licenses/LGPL-2.1.txt and licenses/LGPL-3.0.txt are the Free Software
Foundation's texts, fetched from https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
and https://www.gnu.org/licenses/lgpl-3.0.txt on 2026-09-22. LICENSE at the
root is byte for byte https://www.gnu.org/licenses/gpl-3.0.txt, checked the
same day. licenses/spdx/<id>.txt are the SPDX License List texts at
spdx/license-list-data commit 31ba1a50e5397e00a304dbadc76531740e89ee48.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Directories the SPDX guard in ci.yml greps. See the module docstring.
GUARDED = ("core", "tools", "ui")

RELEASES = "https://github.com/Locke-Werks/Revenant/releases"

# Ports vcpkg installs that are not part of either program. Each is still
# named in the notices, so a reader can see it was considered rather than
# missed, but its licence is not reproduced as if it were in the binary.
NOT_IN_PROGRAM = {
    "catch2": "the test framework, linked into the test executables only",
    "pkgconf": "a build tool that runs during the vcpkg install",
}
NOT_IN_PROGRAM_PREFIX = ("vcpkg-",)  # vcpkg's own build helpers

# Upstream NOTICE files a port's copyright file does not carry. Apache-2.0
# section 4(d) requires a NOTICE to travel with the binary, and vcpkg installs
# only the LICENSE. Keyed by port, with the upstream version the file was taken
# from: a port that moves version fails the run rather than shipping last
# version's attribution.
#
# pthreads4w-3.0.0-NOTICE.txt is NOTICE from pthreads4w-code-v3.0.0.zip, the
# archive vcpkg's pthreads port downloads, whose SHA512 matched the one in the
# port's vcpkg.spdx.json on 2026-09-22.
EXTRA_NOTICES = {
    "pthreads": ("3.0.0", "licenses/notices/pthreads4w-3.0.0-NOTICE.txt"),
}

# The Qt modules the client payload carries a library or plugin from. See
# scripts/stage-payload.ps1 for the measured keep-list these follow from.
QT_MODULES = ("qtbase", "qtdeclarative", "qtmultimedia", "qtsvg")

# Qt attribution components whose code is in no library or plugin the payload
# carries. Anything not listed is included, because naming a component that is
# not in the install costs nothing and omitting one that is costs a notice.
QT_EXCLUDED_COMPONENTS = (
    "Test_",  # QtTest, not shipped
    "TestInternalsPrivate_",
    "QSQLiteDriverPlugin_",  # sqldrivers are not in the payload
    "DBus_",  # QtDBus is not loaded on Windows
    "Bootstrap_",  # the host-tool bootstrap library, never in a target binary
    "QuickControls2Material_",  # the Material style is excluded from the payload
    "qtquickcontrols2materialstyleplugin_",
)

# The FFmpeg libraries Qt Multimedia's backend loads, and the function each one
# exports to report its own licence.
FFMPEG = (
    ("avutil-59.dll", "avutil_license"),
    ("avcodec-61.dll", "avcodec_license"),
    ("avformat-61.dll", "avformat_license"),
    ("swresample-5.dll", "swresample_license"),
    ("swscale-8.dll", "swscale_license"),
)

MSVC_RUNTIME = re.compile(r"^(msvcp140.*|vcruntime140.*|concrt140)\.dll$", re.IGNORECASE)

RULE = "=" * 78
THIN = "-" * 78


class NoticeError(Exception):
    pass


# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------


@dataclass
class Port:
    name: str
    version: str
    licence: str
    upstream: list[str]
    copyright_text: str


def read_version() -> str:
    text = (REPO / "cmake" / "RevenantVersion.cmake").read_text(encoding="utf-8")
    parts = []
    for part in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"REVENANT_VERSION_{part}\s+(\d+)", text)
        if not match:
            raise NoticeError(f"cmake/RevenantVersion.cmake has no REVENANT_VERSION_{part}")
        parts.append(match.group(1))
    return ".".join(parts)


def installed_ports(vcpkg: Path) -> list[str]:
    """Every port the triplet's status database says is installed."""
    status = vcpkg.parent / "vcpkg" / "status"
    if not status.is_file():
        raise NoticeError(f"{status} is missing; {vcpkg} is not a vcpkg_installed triplet directory")
    names: list[str] = []
    for record in status.read_text(encoding="utf-8").split("\n\n"):
        fields = dict(
            line.split(": ", 1) for line in record.splitlines() if ": " in line and not line.startswith(" ")
        )
        if fields.get("Status") != "install ok installed":
            continue
        if fields.get("Architecture") != vcpkg.name:
            continue
        # A feature record carries a Feature field; its port is listed on its own.
        if "Feature" in fields:
            continue
        names.append(fields["Package"])
    if not names:
        raise NoticeError(f"no installed ports for {vcpkg.name} in {status}")
    return sorted(set(names), key=str.lower)


def read_port(vcpkg: Path, name: str) -> Port:
    share = vcpkg / "share" / name
    spdx_path = share / "vcpkg.spdx.json"
    copyright_path = share / "copyright"
    if not spdx_path.is_file():
        raise NoticeError(f"{name}: {spdx_path} is missing")
    if not copyright_path.is_file():
        raise NoticeError(f"{name}: {copyright_path} is missing, so its licence cannot be reproduced")

    spdx = json.loads(spdx_path.read_text(encoding="utf-8"))
    packages = spdx.get("packages", [])
    port = next((p for p in packages if p.get("SPDXID") == "SPDXRef-port"), None)
    if port is None:
        raise NoticeError(f"{name}: {spdx_path} has no SPDXRef-port package")

    version = port.get("versionInfo", "unknown")
    base = version.split("#", 1)[0]
    if "#" in version:
        revision = version.split("#", 1)[1]
        version = f"{base} (vcpkg port revision {revision})"

    text = copyright_path.read_text(encoding="utf-8", errors="replace").strip("\n")
    if name in EXTRA_NOTICES:
        expected, relative = EXTRA_NOTICES[name]
        if base != expected:
            raise NoticeError(
                f"{name} is {base} and {relative} was taken from {expected}. Fetch the NOTICE "
                f"file from the new upstream archive and update EXTRA_NOTICES."
            )
        notice = (REPO / relative).read_text(encoding="utf-8").strip("\n")
        text = f"{text}\n\nNOTICE, from the upstream source archive:\n\n{notice}"

    upstream = [
        p["downloadLocation"]
        for p in packages
        if p.get("SPDXID", "").startswith("SPDXRef-resource-")
        and p.get("downloadLocation") not in (None, "NONE", "NOASSERTION")
    ]
    return Port(
        name=name,
        version=version,
        licence=port.get("licenseConcluded", "NOASSERTION"),
        upstream=upstream,
        copyright_text=text,
    )


def in_program(name: str) -> bool:
    return name not in NOT_IN_PROGRAM and not name.startswith(NOT_IN_PROGRAM_PREFIX)


def excluded_reason(name: str) -> str:
    if name in NOT_IN_PROGRAM:
        return NOT_IN_PROGRAM[name]
    return "one of vcpkg's own build helpers"


def spdx_ids(expression: str) -> list[str]:
    tokens = re.findall(r"[A-Za-z0-9.+-]+", expression)
    return [t for t in tokens if t not in ("AND", "OR", "WITH")]


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------


def wrap(text: str, width: int = 78) -> str:
    out: list[str] = []
    for paragraph in text.split("\n\n"):
        words = paragraph.split()
        line = ""
        for word in words:
            if line and len(line) + 1 + len(word) > width:
                out.append(line)
                line = word
            else:
                line = f"{line} {word}" if line else word
        out.append(line)
        out.append("")
    return "\n".join(out).rstrip("\n")


def heading(title: str) -> str:
    return f"{RULE}\n{title}\n{RULE}"


def render_port(port: Port) -> str:
    lines = [THIN, port.name, THIN, f"Version:  {port.version}", f"Licence:  {port.licence} (as the vcpkg port declares it)"]
    for location in port.upstream:
        lines.append(f"Upstream: {location}")
    lines.append("")
    lines.append(port.copyright_text)
    return "\n".join(lines)


def copyleft_notices(ports: list[Port], program: str, version: str) -> list[str]:
    release = f"{RELEASES}/tag/v{version}"
    notices: list[str] = []
    for port in ports:
        ids = spdx_ids(port.licence)
        if any(i.startswith("LGPL-2.1") for i in ids):
            notices.append(
                wrap(
                    f"{program} contains {port.name} {port.version}, a library covered by the GNU "
                    f"Lesser General Public License version 2.1 or later. Its text is "
                    f"licenses/LGPL-2.1.txt beside this file. The library is linked statically, so "
                    f"the source of {port.name} and of the rest of {program} is published with the "
                    f"release this program came from, {release}, which is what lets you modify the "
                    f"library and relink the program against it."
                )
            )
        elif any(i.startswith("GPL-") for i in ids):
            notices.append(
                wrap(
                    f"{program} contains {port.name} {port.version}, covered by {port.licence}. "
                    f"Revenant is distributed under GPL-3.0-or-later as a whole. The corresponding "
                    f"source of {port.name}, including the patches the vcpkg port applies to it, is "
                    f"published with the release this program came from, {release}."
                )
            )
    return notices


def render_engine(vcpkg: Path, version: str) -> str:
    names = installed_ports(vcpkg)
    ports = [read_port(vcpkg, n) for n in names if in_program(n)]
    left_out = [n for n in names if not in_program(n)]

    parts = [
        heading(f"Third-party notices for revenant-engine.exe, Revenant {version}"),
        wrap(
            "revenant-engine.exe is Revenant, free software under the GNU General Public License "
            "version 3 or later. The licence text is LICENSE.txt beside this file. The executable "
            "is linked statically, so every component below is inside it rather than beside it."
        ),
        wrap(
            f"Generated by scripts/generate_notices.py from the vcpkg tree that built this "
            f"executable, triplet {vcpkg.name}. Each licence below is the port's own copyright "
            f"file, reproduced verbatim."
        ),
    ]

    notices = copyleft_notices(ports, "revenant-engine.exe", version)
    if notices:
        parts.append(heading("Copyleft components"))
        parts.extend(notices)

    parts.append(heading("Components and their licences"))
    parts.extend(render_port(p) for p in ports)

    parts.append(heading("Not in this program"))
    lines = [
        wrap(
            "vulkan-1.dll, the Vulkan loader, is imported and not shipped: it arrives with the "
            "graphics driver."
        )
    ]
    for name in left_out:
        lines.append(f"{name}: installed for the build, {excluded_reason(name)}.")
    parts.append("\n".join(lines))
    return "\n\n".join(parts) + "\n"


def load_qt_sbom(qt: Path, module: str) -> dict:
    matches = sorted((qt / "sbom").glob(f"{module}-*.spdx.json"))
    if not matches:
        raise NoticeError(f"no SPDX document for {module} under {qt / 'sbom'}")
    return json.loads(matches[-1].read_text(encoding="utf-8"))


def qt_attributions(sbom: dict) -> list[dict]:
    kept = []
    for package in sbom.get("packages", []):
        ident = package.get("SPDXID", "")
        if "-qt-3rdparty-sources-" not in ident and "-qt-bundled-3rdparty-module-" not in ident:
            continue
        if package.get("name", "").startswith(QT_EXCLUDED_COMPONENTS):
            continue
        kept.append(package)
    return kept


def attribution_name(package: dict) -> str:
    name = package.get("name", "")
    return name.split("_Attribution_", 1)[1] if "_Attribution_" in name else name


def measure_ffmpeg(deploy: Path) -> tuple[str, str, str]:
    """Ask the FFmpeg DLLs for their version, licence and configuration."""
    if os.name != "nt":
        raise NoticeError("the FFmpeg libraries can only be asked for their licence on Windows")
    # add_dll_directory refuses a relative path with a bare "parameter is incorrect".
    deploy = deploy.resolve()
    add = getattr(os, "add_dll_directory", None)
    handle = add(str(deploy)) if add else None
    try:
        licences = set()
        for dll, function in FFMPEG:
            path = deploy / dll
            if not path.is_file():
                raise NoticeError(f"{path} is missing; the client payload carries it")
            library = ctypes.CDLL(str(path))
            call = getattr(library, function)
            call.restype = ctypes.c_char_p
            licences.add(call().decode())
        avutil = ctypes.CDLL(str(deploy / "avutil-59.dll"))
        avutil.av_version_info.restype = ctypes.c_char_p
        avcodec = ctypes.CDLL(str(deploy / "avcodec-61.dll"))
        avcodec.avcodec_configuration.restype = ctypes.c_char_p
        version = avutil.av_version_info().decode()
        configuration = avcodec.avcodec_configuration().decode()
    finally:
        if handle is not None:
            handle.close()
    if len(licences) != 1:
        raise NoticeError(f"the FFmpeg libraries disagree about their licence: {sorted(licences)}")
    return version, licences.pop(), configuration


def licence_text(identifier: str, extracted: dict[str, str]) -> str:
    if identifier in extracted:
        return extracted[identifier]
    path = REPO / "licenses" / "spdx" / f"{identifier}.txt"
    if not path.is_file():
        raise NoticeError(
            f"a Qt attribution names {identifier} and licenses/spdx/{identifier}.txt does not "
            f"exist. Add the SPDX License List text for it."
        )
    return path.read_text(encoding="utf-8").strip("\n")


def render_client(vcpkg: Path, qt: Path, deploy: Path, version: str, ffmpeg=measure_ffmpeg) -> str:
    names = installed_ports(vcpkg)
    ports = [read_port(vcpkg, n) for n in names if in_program(n)]
    left_out = [n for n in names if not in_program(n)]

    parts = [
        heading(f"Third-party notices for revenant-ui.exe, Revenant {version}"),
        wrap(
            "revenant-ui.exe is Revenant's client, free software under the GNU General Public "
            "License version 3 or later. The licence text is LICENSE.txt beside this file. The "
            "Qt, FFmpeg and Visual C++ runtime libraries beside it are separate files it loads "
            "at run time; the components built into the executable itself come first."
        ),
        wrap(
            f"Generated by scripts/generate_notices.py from the vcpkg tree that built this "
            f"executable, triplet {vcpkg.name}, and from the SPDX documents Qt installs under "
            f"{qt.name}/sbom."
        ),
        heading("Components built into revenant-ui.exe"),
    ]
    parts.extend(render_port(p) for p in ports)
    if left_out:
        parts.append(
            "\n".join(f"{n}: installed for the build, {excluded_reason(n)}." for n in left_out)
        )

    # Qt itself.
    sboms = {m: load_qt_sbom(qt, m) for m in QT_MODULES}
    parts.append(heading("Qt"))
    module_lines = []
    for module, sbom in sboms.items():
        top = next((p for p in sbom["packages"] if p.get("SPDXID") == f"SPDXRef-Package-{module}"), None)
        if top is None:
            raise NoticeError(f"the {module} SPDX document has no SPDXRef-Package-{module}")
        module_lines.append(f"{module}: {top.get('downloadLocation', 'NOASSERTION')}")
    qt_version = qt.parent.name
    parts.append(
        wrap(
            f"The Qt libraries and plugins beside revenant-ui.exe are Qt {qt_version}, from the "
            f"modules listed below, used under the GNU Lesser General Public License version 3. "
            f"Its text is licenses/LGPL-3.0.txt beside this file, and the GNU General Public "
            f"License version 3 it incorporates is LICENSE.txt. Qt is dynamically linked: the "
            f"libraries are ordinary DLLs in this directory, and a modified build of the same Qt "
            f"version put in their place is what revenant-ui.exe will load. Copyright (C) The Qt "
            f"Company Ltd. and other contributors."
        )
        + "\n\nSource, as each module's SPDX document records it:\n"
        + "\n".join(module_lines)
    )

    parts.append(heading("Third-party code inside the Qt libraries"))
    parts.append(
        wrap(
            "Qt bundles these, and records each in its SPDX documents. The licence identifiers "
            "are SPDX expressions and every identifier's text is in the appendix at the end of "
            "this file."
        )
    )
    extracted: dict[str, str] = {}
    needed: set[str] = set()
    seen: set[tuple] = set()
    for module, sbom in sboms.items():
        for info in sbom.get("hasExtractedLicensingInfos", []):
            extracted[info["licenseId"]] = info.get("extractedText", "").strip("\n")
        entries = []
        for package in qt_attributions(sbom):
            key = (
                attribution_name(package),
                package.get("versionInfo"),
                package.get("licenseConcluded"),
                package.get("copyrightText"),
            )
            if key in seen:
                continue
            seen.add(key)
            needed.update(spdx_ids(package.get("licenseConcluded", "")))
            entries.append(
                "\n".join(
                    [
                        f"{key[0]} {key[1] or ''}".rstrip(),
                        f"  Licence:   {key[2]}",
                        "  " + (key[3] or "NOASSERTION").replace("\n", "\n  "),
                    ]
                )
            )
        if entries:
            parts.append(f"{THIN}\nFrom {module}\n{THIN}\n\n" + "\n\n".join(entries))

    # FFmpeg, measured rather than assumed.
    ff_version, ff_licence, ff_configuration = ffmpeg(deploy)
    parts.append(heading("FFmpeg"))
    parts.append(
        wrap(
            f"{', '.join(dll for dll, _ in FFMPEG)} are FFmpeg {ff_version}, loaded by Qt "
            f"Multimedia's FFmpeg backend. The libraries report their licence as \"{ff_licence}\"; "
            f"its text is licenses/LGPL-2.1.txt beside this file. Copyright (C) 2000-2024 FFmpeg "
            f"Project and its contributors. FFmpeg's source is published at "
            f"https://ffmpeg.org/download.html, and this build reports its configuration as:"
        )
        + f"\n\n  {ff_configuration}"
    )

    runtime = sorted(p.name for p in deploy.iterdir() if MSVC_RUNTIME.match(p.name))
    if runtime:
        parts.append(heading("Microsoft Visual C++ runtime"))
        parts.append(
            wrap(
                f"{', '.join(runtime)} are the Microsoft Visual C++ runtime, redistributed as "
                f"Distributable Code under the Visual Studio licence terms."
            )
        )

    parts.append(heading("Appendix: licence texts named above"))
    for identifier in sorted(needed, key=str.lower):
        if identifier == "NOASSERTION":
            continue
        parts.append(f"{THIN}\n{identifier}\n{THIN}\n\n{licence_text(identifier, extracted)}")
    return "\n\n".join(parts) + "\n"


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------


def check_destination(out: Path) -> None:
    resolved = out.resolve()
    for guarded in GUARDED:
        root = (REPO / guarded).resolve()
        if resolved == root or root in resolved.parents:
            raise NoticeError(
                f"refusing to write {out}: {guarded}/ is grepped by the SPDX guard in ci.yml, and "
                f"a notices file lists other projects' licence identifiers"
            )


def write(out: Path, text: str) -> None:
    check_destination(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {out} ({len(text.encode('utf-8'))} bytes)")


def self_test() -> int:
    """Build a two-port fake triplet and check what the engine notices say."""
    failures = 0

    def expect(label: str, condition: bool) -> None:
        nonlocal failures
        print(f"self-test {'ok' if condition else 'FAILED'}: {label}")
        if not condition:
            failures += 1

    with tempfile.TemporaryDirectory() as scratch:
        root = Path(scratch) / "vcpkg_installed"
        triplet = root / "x64-test"
        status = []
        for name, licence, text in (
            ("libfoo", "LGPL-2.1-or-later", "libfoo licence text"),
            ("catch2", "BSL-1.0", "catch2 licence text"),
            ("vcpkg-cmake", "MIT", "helper"),
        ):
            share = triplet / "share" / name
            share.mkdir(parents=True)
            (share / "copyright").write_text(text, encoding="utf-8")
            spdx = {
                "packages": [
                    {"SPDXID": "SPDXRef-port", "name": name, "versionInfo": "1.2.3#4", "licenseConcluded": licence},
                    {"SPDXID": "SPDXRef-resource-0", "downloadLocation": f"git+https://example.invalid/{name}@v1.2.3"},
                ]
            }
            (share / "vcpkg.spdx.json").write_text(json.dumps(spdx), encoding="utf-8")
            status.append(f"Package: {name}\nVersion: 1.2.3\nArchitecture: x64-test\nStatus: install ok installed\n")
        (root / "vcpkg").mkdir(parents=True)
        (root / "vcpkg" / "status").write_text("\n".join(status), encoding="utf-8")

        text = render_engine(triplet, "9.9.9")
        expect("a linked port's licence is reproduced", "libfoo licence text" in text)
        expect("the LGPL section 6 notice names the port", "contains libfoo 1.2.3 (vcpkg port revision 4)" in text)
        expect("the release is named for the version", "tag/v9.9.9" in text)
        expect("a test-only port is named and not reproduced", "catch2: installed for the build" in text
               and "catch2 licence text" not in text)
        expect("a vcpkg helper is named and not reproduced", "vcpkg-cmake: installed for the build" in text)

        (triplet / "share" / "libfoo" / "copyright").unlink()
        try:
            render_engine(triplet, "9.9.9")
            expect("a missing copyright file is refused", False)
        except NoticeError as error:
            expect("a missing copyright file is refused", "libfoo" in str(error))

    try:
        check_destination(REPO / "ui" / "NOTICES.txt")
        expect("a destination under ui/ is refused", False)
    except NoticeError:
        expect("a destination under ui/ is refused", True)

    print(f"\nself-test: {'all as expected' if failures == 0 else f'{failures} failed'}")
    return 0 if failures == 0 else 1


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="program")

    engine = sub.add_parser("engine", help="notices for revenant-engine.exe")
    engine.add_argument("--vcpkg", type=Path, required=True, help="vcpkg_installed/<triplet>")
    engine.add_argument("--out", type=Path, required=True)

    client = sub.add_parser("client", help="notices for revenant-ui.exe and the Qt runtime beside it")
    client.add_argument("--vcpkg", type=Path, required=True, help="vcpkg_installed/<triplet>")
    client.add_argument("--qt", type=Path, required=True, help="the Qt prefix, e.g. C:/Qt/6.8.3/msvc2022_64")
    client.add_argument("--deploy", type=Path, required=True, help="the windeployqt output directory")
    client.add_argument("--out", type=Path, required=True)

    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.program is None:
        parser.print_help()
        return 2

    try:
        version = read_version()
        if args.program == "engine":
            write(args.out, render_engine(args.vcpkg, version))
        else:
            write(args.out, render_client(args.vcpkg, args.qt, args.deploy, version))
    except NoticeError as error:
        print(f"generate_notices: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
