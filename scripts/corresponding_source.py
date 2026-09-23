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

WHAT IT DOES NOT COVER

The client payload conveys Qt 6.8.3 (LGPL-3.0) and FFmpeg 7.1
(LGPL-2.1-or-later) as DLLs. Both licences ask for their source to be offered
with the binaries as well, and neither is in this archive: Qt's is hundreds of
megabytes per module and both are built by The Qt Company rather than here.
THIRD-PARTY-NOTICES-client.txt names where each is published upstream, which
is the weaker promise docs/clean-room.md describes. docs/packaging.md keeps it
on the list of what the first release still owes.
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


def check(data: bytes, algorithm: str, expected: str, what: str) -> None:
    actual = hashlib.new(algorithm, data).hexdigest()
    if actual.lower() != expected.lower():
        raise SourceError(f"{what}: {algorithm} is {actual}, and the build recorded {expected}")


def overlay_file(ref: str, path: str) -> bytes:
    # From git at the commit being archived rather than from the working tree,
    # so the recipe published is the one in Revenant's own source archive.
    return subprocess.run(["git", "-C", str(REPO), "show", f"{ref}:{path}"], check=True, capture_output=True).stdout


def bundle_port(port: dict, out: Path, lines: list[str], ref: str = "HEAD") -> None:
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
            data = overlay_file(ref, f"{overlay}/{file_name}")
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
        "Not in this archive: the sources of Qt 6.8.3 and FFmpeg 7.1, whose DLLs",
        "the installer carries beside revenant-ui.exe. THIRD-PARTY-NOTICES-client.txt",
        "names where each is published.",
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
        else:
            parser.print_help()
            return 2
    except (SourceError, notices.NoticeError) as error:
        print(f"corresponding_source: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
