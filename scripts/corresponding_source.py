#!/usr/bin/env python3
"""Build the corresponding-source archive a Revenant release publishes.

docs/clean-room.md settled how Revenant meets its copyleft obligations on
2026-09-20 (decision record revenant.licensing.libusb): LGPL-2.1 section 6d and
GPL-3.0 section 6d are both met by offering the binary and its sources from the
same place. The release page is that place, so every release carries this
archive beside the installer:

    Revenant-<version>-corresponding-source.zip
        revenant-<version>-source.zip     Revenant at the commit that was built
        upstream/<port>-<ref>.tar.gz      each copyleft port's upstream source
        vcpkg-port-<port>/...             the recipe vcpkg built it with,
                                          patches included, from vcpkg's git
                                          tree or from vcpkg-overlays/
        SOURCES.txt                       what each piece is and how it was
                                          checked

Two steps, because the inputs live on two machines:

    corresponding_source.py manifest --vcpkg build/ci/vcpkg_installed/x64-windows-static
                                     --out dist/source-manifest/sources.json
    corresponding_source.py bundle --manifest dist/source-manifest/sources.json
                                   --out dist/source

`manifest` runs where the engine was built, because only that tree's
share/<port>/vcpkg.spdx.json knows which port versions went into the binary:
the upstream archive and its SHA512, the git tree vcpkg's port recipe came
from, and the SHA256 of every file in that recipe. `bundle` runs anywhere with
the checkout and a network, downloads each piece, and refuses any byte that
does not match the hash the build recorded. A source archive that is not the
source of the binary is worse than none, because it looks like compliance.

A port built from one of this repository's overlays has no vcpkg git tree:
vcpkg records its origin as NOASSERTION. `manifest` then looks for the recipe
in vcpkg-overlays/<port>/ and accepts it only if every file matches the SHA256
the build recorded, and `bundle` takes it from git at the commit it archives,
checking the hashes again.

WHICH PORTS

Every installed port whose declared licence is GPL or LGPL. Today that is
libusb (LGPL-2.1-or-later), which vcpkg builds unpatched from the registry,
and rtlsdr (GPL-2.0-or-later), built since 2026-09-23 from
vcpkg-overlays/rtlsdr, which applies dependencies.diff, library-linkage.diff,
tools.diff and Revenant's own cancel-waits-for-transfers.diff to
osmocom/rtl-sdr at 797f814. The decision record lists libusb's source, the
patches and Revenant's own archive. This also carries rtlsdr's upstream
source, because a patch is a set of changes to a base and without the base it
is not the source of anything.

THE CLIENT'S LIBRARIES

The client payload conveys Qt 6.8.3 (LGPL-3.0) and FFmpeg 7.1
(LGPL-2.1-or-later) as DLLs, and both licences ask for their source to be
offered with them. They are not in the archive above: The Qt Company built
them, not this repository, so there is no vcpkg record to check against, and
together they come to over 100 MB. The owner decided on 2026-09-27 that the
release page carries them as separate assets, the exact upstream archives,
unmodified: the same route of offering the source from the place the binary
comes from, which for Qt is GPL-3.0 section 6d as LGPL-3.0 incorporates it and
for FFmpeg is LGPL-2.1 section 4:

    corresponding_source.py coverage --stage dist/stage [--qt C:/Qt/6.8.3/msvc2022_64]
    corresponding_source.py client --out dist/client-source [--stage dist/stage]

scripts/client-sources.json pins each archive by SHA256 and lists the payload
files it is the source of. `client` downloads each one, refuses any byte that
does not match, and writes it unchanged beside SOURCES-client.txt. `coverage`
fails a staged payload holding a DLL no pinned archive covers, so a Qt module
added to the keep-list without its source stops the build rather than
shipping without an offer. Given the Qt prefix it also checks each covered
file against the module's own SPDX document, and each pinned commit against
the one that document records, because a file filed under the wrong module
would pass the first check and publish the wrong source.

This section used to say neither source was carried and that the notices
named where each is published upstream, which docs/clean-room.md calls the
weaker promise. True until 2026-09-27.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_notices as notices  # noqa: E402

REPO = notices.REPO

VCPKG_REPO = "microsoft/vcpkg"

# Where this repository keeps the ports vcpkg-configuration.json overlays on
# the registry. A port built from one is published from here.
OVERLAYS = "vcpkg-overlays"
GITHUB_SOURCE = re.compile(r"^git\+https://github\.com/([^/]+)/([^/@]+)@(.+)$")

# Files in a port's SPDX document that the install produced rather than the
# recipe supplied. Everything else at the top of the list is recipe.
PRODUCED = ("BUILD_INFO",)


class SourceError(Exception):
    pass


# ---------------------------------------------------------------------------
# manifest
# ---------------------------------------------------------------------------


def is_copyleft(licence: str) -> bool:
    return any(i.startswith(("GPL-", "LGPL-")) for i in notices.spdx_ids(licence))


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def overlay_for(name: str, recipe_files: list[dict]) -> str:
    """The overlay directory in this repository the port was built from.

    vcpkg records an overlay port's downloadLocation as NOASSERTION, because
    the recipe did not come from a registry. Revenant's overlays live in
    OVERLAYS, so the recipe is found there by name and accepted only when every
    file the build hashed is present with the same SHA256. A checkout whose
    overlay has moved on since the build is refused rather than published as
    the recipe of a binary it did not build.
    """
    directory = REPO / OVERLAYS / name
    if not directory.is_dir():
        raise SourceError(
            f"{name}: vcpkg built it from an overlay port, and {OVERLAYS}/{name} is not in this checkout"
        )
    for recipe_file in recipe_files:
        path = directory / recipe_file["name"]
        if not path.is_file():
            raise SourceError(f"{name}: the build used {recipe_file['name']}, which {OVERLAYS}/{name} does not have")
        if sha256_file(path).lower() != recipe_file["sha256"].lower():
            raise SourceError(
                f"{name}: {OVERLAYS}/{name}/{recipe_file['name']} is not the file the build used; "
                f"its SHA256 differs from the one vcpkg recorded"
            )
    return f"{OVERLAYS}/{name}"


def port_manifest(vcpkg: Path, name: str) -> dict:
    spdx = json.loads((vcpkg / "share" / name / "vcpkg.spdx.json").read_text(encoding="utf-8"))
    packages = spdx.get("packages", [])
    port = next(p for p in packages if p.get("SPDXID") == "SPDXRef-port")

    location = port.get("downloadLocation", "")
    recipe = re.match(r"^git\+https://github\.com/microsoft/vcpkg@([0-9a-f]{40})$", location)
    if not recipe and location != "NOASSERTION":
        raise SourceError(f"{name}: the port's downloadLocation is not a vcpkg git tree: {location}")

    recipe_files = []
    for entry in spdx.get("files", []):
        file_name = entry.get("fileName", "")
        # Top-level entries are the recipe; ./lib, ./include and the rest are
        # what the build installed.
        if not file_name.startswith("./") or "/" in file_name[2:]:
            continue
        if file_name[2:] in PRODUCED:
            continue
        sha256 = next((c["checksumValue"] for c in entry.get("checksums", []) if c.get("algorithm") == "SHA256"), None)
        if sha256 is None:
            raise SourceError(f"{name}: {file_name} has no SHA256 in the port's SPDX document")
        recipe_files.append({"name": file_name[2:], "sha256": sha256})

    upstream = []
    for resource in packages:
        if not resource.get("SPDXID", "").startswith("SPDXRef-resource-"):
            continue
        sha512 = next(
            (c["checksumValue"] for c in resource.get("checksums", []) if c.get("algorithm") == "SHA512"), None
        )
        if sha512 is None:
            raise SourceError(f"{name}: upstream {resource.get('downloadLocation')} has no SHA512 to check against")
        upstream.append({"location": resource["downloadLocation"], "sha512": sha512})
    if not upstream:
        raise SourceError(f"{name}: no upstream source is recorded, so there is nothing to publish")

    entry = {
        "name": name,
        "version": port.get("versionInfo", "unknown"),
        "licence": port.get("licenseConcluded", "NOASSERTION"),
        "recipe_files": recipe_files,
        "upstream": upstream,
    }
    if recipe:
        entry["recipe_tree"] = recipe.group(1)
    else:
        entry["recipe_overlay"] = overlay_for(name, recipe_files)
    return entry


def make_manifest(vcpkg: Path) -> dict:
    ports = []
    for name in notices.installed_ports(vcpkg):
        spdx_path = vcpkg / "share" / name / "vcpkg.spdx.json"
        if not spdx_path.is_file():
            raise SourceError(f"{name}: {spdx_path} is missing")
        spdx = json.loads(spdx_path.read_text(encoding="utf-8"))
        port = next((p for p in spdx.get("packages", []) if p.get("SPDXID") == "SPDXRef-port"), {})
        if not notices.in_program(name) or not is_copyleft(port.get("licenseConcluded", "")):
            continue
        ports.append(port_manifest(vcpkg, name))
    if not ports:
        raise SourceError(f"no copyleft port in {vcpkg}; a manifest with nothing in it is a mistake")
    return {"triplet": vcpkg.name, "ports": ports}


# ---------------------------------------------------------------------------
# bundle
# ---------------------------------------------------------------------------


def fetch(url: str, accept: str | None = None) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "revenant-corresponding-source"})
    if accept:
        request.add_header("Accept", accept)
    # The API allows 60 anonymous requests an hour, which a CI runner sharing
    # an address can exhaust. The workflow's own token lifts that.
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token and url.startswith("https://api.github.com/"):
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def check(data: bytes, algorithm: str, expected: str, what: str, recorded_by: str = "the build") -> None:
    actual = hashlib.new(algorithm, data).hexdigest()
    if actual.lower() != expected.lower():
        raise SourceError(f"{what}: {algorithm} is {actual}, and {recorded_by} recorded {expected}")


def overlay_file(revenant_ref: str, path: str) -> bytes:
    # From git at the commit being archived rather than from the working tree,
    # so the recipe published is the one in Revenant's own source archive.
    shown = subprocess.run(["git", "-C", str(REPO), "show", f"{revenant_ref}:{path}"], capture_output=True)
    if shown.returncode != 0:
        raise SourceError(f"{path} is not in Revenant at {revenant_ref}: {shown.stderr.decode(errors='replace').strip()}")
    return shown.stdout


# revenant_ref rather than ref, because the upstream loop below unpacks each
# archive's own ref, and a shared name sent the overlay lookup to osmocom's
# commit instead of Revenant's.
def bundle_port(port: dict, out: Path, lines: list[str], revenant_ref: str = "HEAD") -> None:
    name = port["name"]
    lines.append(f"{name} {port['version']}, {port['licence']}")

    for resource in port["upstream"]:
        match = GITHUB_SOURCE.match(resource["location"])
        if not match:
            raise SourceError(f"{name}: no rule for fetching {resource['location']}")
        owner, repo, ref = match.groups()
        # vcpkg_from_github downloads exactly this URL, so its SHA512 is the
        # one the port's SPDX document recorded.
        url = f"https://github.com/{owner}/{repo}/archive/{ref}.tar.gz"
        data = fetch(url)
        check(data, "sha512", resource["sha512"], url)
        target = out / "upstream" / f"{name}-{repo}-{ref}.tar.gz"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        lines.append(f"  upstream/{target.name}")
        lines.append(f"    from {url}, SHA512 matched the build")

    recipe_dir = out / f"vcpkg-port-{name}"
    recipe_dir.mkdir(parents=True, exist_ok=True)

    if "recipe_overlay" in port:
        overlay = port["recipe_overlay"]
        for recipe_file in port["recipe_files"]:
            file_name = recipe_file["name"]
            data = overlay_file(revenant_ref, f"{overlay}/{file_name}")
            check(data, "sha256", recipe_file["sha256"], f"{name}/{file_name}")
            (recipe_dir / file_name).write_bytes(data)
        lines.append(f"  vcpkg-port-{name}/: {', '.join(f['name'] for f in port['recipe_files'])}")
        lines.append(f"    Revenant's overlay port, {overlay} at the commit above, each SHA256 matched the build")
        lines.append("")
        return

    tree = json.loads(fetch(f"https://api.github.com/repos/{VCPKG_REPO}/git/trees/{port['recipe_tree']}"))
    blobs = {entry["path"]: entry["sha"] for entry in tree.get("tree", []) if entry.get("type") == "blob"}
    for recipe_file in port["recipe_files"]:
        file_name = recipe_file["name"]
        if file_name not in blobs:
            raise SourceError(f"{name}: {file_name} is not in vcpkg tree {port['recipe_tree']}")
        blob = json.loads(fetch(f"https://api.github.com/repos/{VCPKG_REPO}/git/blobs/{blobs[file_name]}"))
        data = base64.b64decode(blob["content"])
        check(data, "sha256", recipe_file["sha256"], f"{name}/{file_name}")
        (recipe_dir / file_name).write_bytes(data)
    lines.append(f"  vcpkg-port-{name}/: {', '.join(f['name'] for f in port['recipe_files'])}")
    lines.append(f"    from git tree {port['recipe_tree']} in {VCPKG_REPO}, each SHA256 matched the build")
    lines.append("")


def revenant_archive(out: Path, version: str, ref: str) -> tuple[Path, str]:
    target = out / f"revenant-{version}-source.zip"
    subprocess.run(
        ["git", "-C", str(REPO), "archive", "--format=zip", f"--prefix=revenant-{version}/", "-o", str(target), ref],
        check=True,
    )
    commit = subprocess.run(
        ["git", "-C", str(REPO), "rev-parse", ref], check=True, capture_output=True, text=True
    ).stdout.strip()
    return target, commit


def make_bundle(manifest: dict, out: Path, ref: str) -> Path:
    version = notices.read_version()
    if out.exists():
        shutil.rmtree(out)
    staging = out / f"Revenant-{version}-corresponding-source"
    staging.mkdir(parents=True)

    archive, commit = revenant_archive(staging, version, ref)
    lines = [
        f"Corresponding source for Revenant {version}",
        "",
        "Published beside the installer so the binaries and their sources come",
        "from the same place, which is how this release meets GPL-3.0 section 6d",
        "and LGPL-2.1 section 6d. docs/clean-room.md has the reasoning.",
        "",
        f"{archive.name}",
        f"  Revenant at commit {commit}, from git archive. Its vcpkg.json and",
        "  vcpkg-configuration.json pin every dependency version.",
        "",
        f"Copyleft components of revenant-engine.exe, triplet {manifest['triplet']}:",
        "",
    ]
    for port in manifest["ports"]:
        bundle_port(port, staging, lines, ref)
    lines += [
        "Not in this archive: the sources of Qt and FFmpeg, whose DLLs the installer",
        "carries beside revenant-ui.exe. They are published beside this archive on",
        "the same release page, as the upstream archives, unmodified, and",
        "SOURCES-client.txt there says what each one is.",
    ]
    (staging / "SOURCES.txt").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

    zipped = out / f"{staging.name}.zip"
    with zipfile.ZipFile(zipped, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
        for path in sorted(staging.rglob("*")):
            if path.is_file():
                bundle.write(path, path.relative_to(out).as_posix())
    print(f"wrote {zipped} ({zipped.stat().st_size} bytes)")
    print("\n".join(lines))
    return zipped


# ---------------------------------------------------------------------------
# client
# ---------------------------------------------------------------------------

# An archive's name becomes a file name here and a release asset name on
# GitHub, so it is a bare name and nothing a path could hide in.
ASSET_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
HEX = re.compile(r"^[0-9a-f]+$")


def load_pins(path: Path | None = None) -> dict:
    pins = notices.load_client_sources(path or notices.CLIENT_SOURCES)
    seen_names: set[str] = set()
    seen_files: dict[str, str] = {}
    for archive in pins["archives"]:
        name = archive.get("name", "")
        if not ASSET_NAME.match(name) or name in seen_names:
            raise SourceError(f"client sources: {name!r} is not a usable asset name, or it is pinned twice")
        seen_names.add(name)
        sha256 = archive.get("sha256", "")
        if len(sha256) != 64 or not HEX.match(sha256):
            raise SourceError(f"client sources: {name} has no SHA256 pinned")
        if not str(archive.get("url", "")).startswith("https://"):
            raise SourceError(f"client sources: {name} is not fetched over https")
        if not archive.get("covers"):
            raise SourceError(f"client sources: {name} covers no payload file, so it has no reason to be published")
        for covered in archive["covers"]:
            if covered.lower() in seen_files:
                raise SourceError(f"client sources: {covered} is covered by both {seen_files[covered.lower()]} and {name}")
            seen_files[covered.lower()] = name
    return pins


def staged_dlls(stage: Path) -> list[str]:
    return sorted(
        p.relative_to(stage).as_posix() for p in stage.rglob("*") if p.is_file() and p.suffix.lower() == ".dll"
    )


def sbom_path(covered: str) -> str:
    """Where Qt's install puts a payload file, as its SPDX document names it.

    windeployqt flattens Qt's layout: a library from bin/ lands at the top of
    the payload and a plugin from plugins/<kind>/ lands at <kind>/, while QML
    modules keep their qml/ prefix.
    """
    if "/" not in covered:
        return f"bin/{covered}"
    if covered.startswith("qml/"):
        return covered
    return f"plugins/{covered}"


def check_against_qt(pins: dict, qt: Path) -> list[str]:
    if qt.parent.name != pins["qt_version"]:
        raise SourceError(f"{qt} is not Qt {pins['qt_version']}, the version whose sources are pinned")
    lines = []
    for archive in pins["archives"]:
        module = archive.get("qt_module")
        if module is None:
            continue
        sbom = notices.load_qt_sbom(qt, module)
        installed = {f.get("fileName", "").removeprefix("./").lower() for f in sbom.get("files", [])}
        wrong = [c for c in archive["covers"] if sbom_path(c).lower() not in installed]
        if wrong:
            raise SourceError(
                f"{archive['name']} is pinned as the source of {', '.join(wrong)}, and {module}'s own "
                f"SPDX document does not install them. Pin the archive of the module that does."
            )
        top = next((p for p in sbom.get("packages", []) if p.get("SPDXID") == f"SPDXRef-Package-{module}"), {})
        recorded = top.get("downloadLocation", "").rpartition("@")[2]
        # qtmultimedia's document records the repository and no commit, so
        # there is nothing to compare; the others name the commit they built.
        if HEX.match(recorded) and recorded != archive.get("commit"):
            raise SourceError(
                f"{module} was built from {recorded}, and {archive['name']} is pinned as {archive.get('commit')}"
            )
        lines.append(f"{module}: {len(archive['covers'])} files installed by it; commit {recorded if HEX.match(recorded) else 'not recorded'}")
    return lines


def coverage(stage: Path, pins: dict, qt: Path | None = None) -> list[str]:
    """Match every DLL in a staged payload to the archive that is its source.

    The Visual C++ runtime is the one exception: it is redistributed under
    Microsoft's terms, which ask for no source. Revenant's own programs are
    executables and their source is the corresponding-source archive.
    """
    covered = {c.lower(): a for a in pins["archives"] for c in a["covers"]}
    dlls = staged_dlls(stage)
    if not dlls:
        raise SourceError(f"{stage} holds no DLL, so it is not a staged client payload")

    uncovered, counted = [], {a["name"]: 0 for a in pins["archives"]}
    for path in dlls:
        if "/" not in path and notices.MSVC_RUNTIME.match(path):
            continue
        archive = covered.get(path.lower())
        if archive is None:
            uncovered.append(path)
        else:
            counted[archive["name"]] += 1
    if uncovered:
        raise SourceError(
            f"the payload carries {', '.join(uncovered)} and no archive in {notices.CLIENT_SOURCES.name} "
            f"is its source. Add it to the covers of the archive it comes from, pinning that archive if "
            f"it is new, before it ships."
        )
    # The other direction as well. A pin for a file the payload no longer
    # carries publishes a source nobody was given the binary of, and says in
    # SOURCES-client.txt that it corresponds to something that is not there.
    staged = {p.lower() for p in dlls}
    stale = [c for a in pins["archives"] for c in a["covers"] if c.lower() not in staged]
    if stale:
        raise SourceError(
            f"{notices.CLIENT_SOURCES.name} pins sources for {', '.join(stale)}, which {stage} does not carry"
        )

    lines = [f"{name}: {count} DLLs" for name, count in counted.items()]
    if qt is not None:
        lines += check_against_qt(pins, qt)
    return lines


def make_client(pins: dict, out: Path, get=None, echo: bool = True) -> int:
    get = get or fetch
    version = notices.read_version()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    lines = [
        f"Sources of the Qt and FFmpeg libraries in Revenant {version}",
        "",
        "revenant-ui.exe loads Qt and FFmpeg as DLLs installed beside it. The",
        "archives listed here are their source, published unmodified on the same",
        "release page as the installer, so the object code and its source are",
        "offered from the same place: GPL-3.0 section 6d, which LGPL-3.0",
        "incorporates, for Qt, and LGPL-2.1 section 4 for FFmpeg. docs/clean-room.md",
        "and docs/packaging.md in Revenant's own source have the reasoning, and the",
        "evidence that these are the archives the DLLs were built from.",
        "",
        "Each archive was downloaded from the address given and written here byte",
        "for byte, and its SHA256 matched the one scripts/client-sources.json pins",
        "at the commit this release was built from.",
        "",
    ]
    total = 0
    for archive in pins["archives"]:
        data = get(archive["url"])
        check(data, "sha256", archive["sha256"], archive["url"], recorded_by=notices.CLIENT_SOURCES.name)
        if "sha1_qt_pinned" in archive:
            check(data, "sha1", archive["sha1_qt_pinned"], archive["url"], recorded_by=archive["qt_pinned_by"])
        (out / archive["name"]).write_bytes(data)
        total += len(data)

        lines.append(f"{archive['name']} ({len(data):,} bytes)")
        lines.append(f"  {archive['component']}, {archive['licence']}")
        lines.append(f"  from {archive['url']}")
        lines.append(f"  SHA256 {archive['sha256']}, matched")
        if "commit" in archive:
            lines += indented(
                f"The Qt Company's source archive of {archive['qt_module']} at commit "
                f"{archive['commit']}, its v{pins['qt_version']} tag."
            )
        if "sha1_qt_pinned" in archive:
            lines += indented(
                f"SHA1 {archive['sha1_qt_pinned']}, matched: the hash The Qt Company's build of these "
                f"libraries checks this archive against before configuring and building it with no "
                f"patch applied, in {archive['qt_pinned_by']}"
            )
        lines += indented(
            f"The source of {', '.join(archive['covers'])}"
            + (f", and of {archive['also']}" if "also" in archive else "")
            + "."
        )
        lines.append("")

    (out / "SOURCES-client.txt").write_text("\n".join(lines), encoding="utf-8", newline="\n")
    if echo:
        print("\n".join(lines))
    print(f"wrote {len(pins['archives'])} archives and SOURCES-client.txt to {out}: {total:,} bytes of source")
    return total


def indented(text: str) -> list[str]:
    # Neither a URL nor a payload path may be split across lines: a reader
    # copies them, and a hyphen or a break inside one is a different address.
    return textwrap.wrap(
        text, 78, initial_indent="  ", subsequent_indent="  ", break_long_words=False, break_on_hyphens=False
    )


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------


def self_test() -> int:
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
        for name, licence in (("libfoo", "LGPL-2.1-or-later"), ("libbar", "MIT"), ("catch2", "GPL-3.0-only")):
            share = triplet / "share" / name
            share.mkdir(parents=True)
            (share / "copyright").write_text("text", encoding="utf-8")
            spdx = {
                "packages": [
                    {
                        "SPDXID": "SPDXRef-port",
                        "versionInfo": "1.0",
                        "licenseConcluded": licence,
                        "downloadLocation": "git+https://github.com/microsoft/vcpkg@" + "a" * 40,
                    },
                    {
                        "SPDXID": "SPDXRef-resource-0",
                        "downloadLocation": f"git+https://github.com/example/{name}@v1.0",
                        "checksums": [{"algorithm": "SHA512", "checksumValue": "00"}],
                    },
                ],
                "files": [
                    {"fileName": "./portfile.cmake", "checksums": [{"algorithm": "SHA256", "checksumValue": "11"}]},
                    {"fileName": "./fix.diff", "checksums": [{"algorithm": "SHA256", "checksumValue": "22"}]},
                    {"fileName": "./BUILD_INFO", "checksums": [{"algorithm": "SHA256", "checksumValue": "33"}]},
                    {"fileName": "./lib/foo.lib", "checksums": [{"algorithm": "SHA256", "checksumValue": "44"}]},
                ],
            }
            (share / "vcpkg.spdx.json").write_text(json.dumps(spdx), encoding="utf-8")
            status.append(f"Package: {name}\nVersion: 1.0\nArchitecture: x64-test\nStatus: install ok installed\n")
        (root / "vcpkg").mkdir(parents=True)
        (root / "vcpkg" / "status").write_text("\n".join(status), encoding="utf-8")

        manifest = make_manifest(triplet)
        names = [p["name"] for p in manifest["ports"]]
        expect("a copyleft port is in the manifest", names == ["libfoo"])
        expect("a permissive port and a test-only port are not", "libbar" not in names and "catch2" not in names)
        recipe = [f["name"] for f in manifest["ports"][0]["recipe_files"]]
        expect("the recipe is the port's own files and nothing the build produced", recipe == ["portfile.cmake", "fix.diff"])

        # An overlay port: vcpkg records NOASSERTION, and the recipe has to be
        # found in the checkout with the hashes the build recorded.
        global REPO
        saved_repo = REPO
        REPO = Path(scratch) / "repo"
        try:
            overlay = REPO / OVERLAYS / "libfoo"
            overlay.mkdir(parents=True)
            (overlay / "portfile.cmake").write_bytes(b"portfile")
            (overlay / "fix.diff").write_bytes(b"fix")
            spdx_path = triplet / "share" / "libfoo" / "vcpkg.spdx.json"
            spdx = json.loads(spdx_path.read_text(encoding="utf-8"))
            spdx["packages"][0]["downloadLocation"] = "NOASSERTION"
            spdx["files"][0]["checksums"][0]["checksumValue"] = hashlib.sha256(b"portfile").hexdigest()
            spdx["files"][1]["checksums"][0]["checksumValue"] = hashlib.sha256(b"fix").hexdigest()
            spdx_path.write_text(json.dumps(spdx), encoding="utf-8")
            port = make_manifest(triplet)["ports"][0]
            expect("an overlay port's recipe is found in the checkout", port.get("recipe_overlay") == f"{OVERLAYS}/libfoo")
            expect("an overlay port names no vcpkg tree", "recipe_tree" not in port)

            (overlay / "fix.diff").write_bytes(b"a fix edited after the build")
            try:
                make_manifest(triplet)
                expect("an overlay edited since the build is refused", False)
            except SourceError:
                expect("an overlay edited since the build is refused", True)
        finally:
            REPO = saved_repo

    try:
        check(b"abc", "sha256", "0" * 64, "fixture")
        expect("a hash mismatch is refused", False)
    except SourceError:
        expect("a hash mismatch is refused", True)

    # The client sources. The real pins are loaded and validated, then the
    # download, the coverage check and the SPDX cross-check run against
    # fixtures, with a stand-in for fetch so nothing touches the network.
    real = load_pins()
    expect("the pinned client sources load and validate", len(real["archives"]) >= 2)
    expect("every pinned Qt archive names its commit", all("commit" in a for a in real["archives"] if "qt_module" in a))

    with tempfile.TemporaryDirectory() as scratch:
        scratch = Path(scratch)
        qt_bytes, ff_bytes = b"qtbase source archive", b"ffmpeg source archive"
        pins = {
            "qt_version": "6.8.3",
            "archives": [
                {
                    "name": "qtbase-src.tar.xz", "component": "Qt, qtbase", "licence": "LGPL-3.0-only",
                    "qt_module": "qtbase", "commit": "c" * 40, "url": "https://example.invalid/qtbase",
                    "sha256": hashlib.sha256(qt_bytes).hexdigest(), "size": len(qt_bytes),
                    "covers": ["Qt6Core.dll", "platforms/qwindows.dll"],
                },
                {
                    "name": "ffmpeg-src.tar.gz", "component": "FFmpeg", "licence": "LGPL-2.1-or-later",
                    "ffmpeg_version": "7.1", "url": "https://example.invalid/ffmpeg",
                    "sha256": hashlib.sha256(ff_bytes).hexdigest(), "size": len(ff_bytes),
                    "sha1_qt_pinned": hashlib.sha1(ff_bytes).hexdigest(), "qt_pinned_by": "a fixture",
                    "covers": ["avcodec-61.dll"],
                },
            ],
        }
        pin_file = scratch / "pins.json"
        pin_file.write_text(json.dumps(pins), encoding="utf-8")
        expect("a well-formed pin file validates", load_pins(pin_file)["qt_version"] == "6.8.3")

        served = {"https://example.invalid/qtbase": qt_bytes, "https://example.invalid/ffmpeg": ff_bytes}
        out = scratch / "client-source"
        total = make_client(pins, out, get=served.__getitem__, echo=False)
        expect("each archive is written byte for byte",
               (out / "qtbase-src.tar.xz").read_bytes() == qt_bytes and (out / "ffmpeg-src.tar.gz").read_bytes() == ff_bytes)
        expect("the total is the archives' size", total == len(qt_bytes) + len(ff_bytes))
        sources = (out / "SOURCES-client.txt").read_text(encoding="utf-8")
        expect("SOURCES-client.txt names each archive and what it is the source of",
               "qtbase-src.tar.xz" in sources and "platforms/qwindows.dll" in sources and "avcodec-61.dll" in sources)

        tampered = dict(served, **{"https://example.invalid/ffmpeg": b"not the pinned archive"})
        try:
            make_client(pins, out, get=tampered.__getitem__, echo=False)
            expect("an archive that is not the pinned one is refused", False)
        except SourceError:
            expect("an archive that is not the pinned one is refused", True)

        stage = scratch / "stage"
        for path in ("Qt6Core.dll", "platforms/qwindows.dll", "avcodec-61.dll", "msvcp140.dll", "revenant-ui.exe"):
            (stage / path).parent.mkdir(parents=True, exist_ok=True)
            (stage / path).write_bytes(b"binary")
        report = coverage(stage, pins)
        expect("a payload whose every DLL is pinned passes, the VC runtime aside",
               "qtbase-src.tar.xz: 2 DLLs" in report and "ffmpeg-src.tar.gz: 1 DLLs" in report)

        (stage / "Qt6Foo.dll").write_bytes(b"binary")
        try:
            coverage(stage, pins)
            expect("a Qt library with no pinned source fails the payload", False)
        except SourceError as error:
            expect("a Qt library with no pinned source fails the payload", "Qt6Foo.dll" in str(error))
        (stage / "Qt6Foo.dll").unlink()

        (stage / "avcodec-61.dll").unlink()
        try:
            coverage(stage, pins)
            expect("a pin for a file the payload no longer carries fails", False)
        except SourceError as error:
            expect("a pin for a file the payload no longer carries fails", "avcodec-61.dll" in str(error))
        (stage / "avcodec-61.dll").write_bytes(b"binary")

        qt = scratch / "Qt" / "6.8.3" / "msvc2022_64"
        (qt / "sbom").mkdir(parents=True)

        def qt_sbom(files: list[str], commit: str) -> None:
            document = {
                "packages": [{"SPDXID": "SPDXRef-Package-qtbase",
                              "downloadLocation": f"git://code.qt.io/qt/qtbase.git@{commit}"}],
                "files": [{"fileName": f"./{f}"} for f in files],
            }
            (qt / "sbom" / "qtbase-6.8.3.spdx.json").write_text(json.dumps(document), encoding="utf-8")

        qt_sbom(["bin/Qt6Core.dll", "plugins/platforms/qwindows.dll"], "c" * 40)
        expect("files Qt's own SPDX document installs from the pinned commit pass",
               any(line.startswith("qtbase: 2 files") for line in coverage(stage, pins, qt)))

        qt_sbom(["bin/Qt6Core.dll"], "c" * 40)
        try:
            coverage(stage, pins, qt)
            expect("a file pinned under a module that does not install it is refused", False)
        except SourceError as error:
            expect("a file pinned under a module that does not install it is refused", "platforms/qwindows.dll" in str(error))

        qt_sbom(["bin/Qt6Core.dll", "plugins/platforms/qwindows.dll"], "d" * 40)
        try:
            coverage(stage, pins, qt)
            expect("a module built from another commit than the one pinned is refused", False)
        except SourceError:
            expect("a module built from another commit than the one pinned is refused", True)

    print(f"\nself-test: {'all as expected' if failures == 0 else f'{failures} failed'}")
    return 0 if failures == 0 else 1


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="command")

    manifest = sub.add_parser("manifest", help="record what the engine was built from")
    manifest.add_argument("--vcpkg", type=Path, required=True, help="vcpkg_installed/<triplet>")
    manifest.add_argument("--out", type=Path, required=True)

    bundle = sub.add_parser("bundle", help="download, check and zip the sources")
    bundle.add_argument("--manifest", type=Path, required=True)
    bundle.add_argument("--out", type=Path, required=True)
    bundle.add_argument("--ref", default="HEAD", help="the Revenant commit to archive")

    client = sub.add_parser("client", help="download and check the Qt and FFmpeg source archives")
    client.add_argument("--out", type=Path, required=True)
    client.add_argument("--stage", type=Path, help="run the coverage check against this payload first")

    cover = sub.add_parser("coverage", help="fail a staged payload holding a DLL with no pinned source")
    cover.add_argument("--stage", type=Path, required=True)
    cover.add_argument("--qt", type=Path, help="the Qt prefix, to check the pins against Qt's SPDX documents")

    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    try:
        if args.command == "manifest":
            data = make_manifest(args.vcpkg)
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8", newline="\n")
            print(f"wrote {args.out}: {', '.join(p['name'] for p in data['ports'])}")
        elif args.command == "bundle":
            make_bundle(json.loads(args.manifest.read_text(encoding="utf-8")), args.out, args.ref)
        elif args.command == "client":
            pins = load_pins()
            # Checked before anything is downloaded, so a payload with an
            # unpinned DLL costs seconds rather than 100 MB of transfer.
            if args.stage is not None:
                print("\n".join(coverage(args.stage, pins)))
            make_client(pins, args.out)
        elif args.command == "coverage":
            print("\n".join(coverage(args.stage, load_pins(), args.qt)))
            print(f"every DLL in {args.stage} has its source pinned in {notices.CLIENT_SOURCES.name}")
        else:
            parser.print_help()
            return 2
    except (SourceError, notices.NoticeError) as error:
        print(f"corresponding_source: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
