#!/usr/bin/env python3
"""Fail when a test target does not suppress Windows modal dialogs.

tests/support/no_modal_dialogs.cpp stops a test process opening a window it
then sits behind. On a desk that is a popup somebody dismisses. On the CI
runner it is a job that hangs to its timeout and reports nothing, because the
message the process wanted to print is inside a dialog no one will see.

Nothing links the file automatically. Every target lists the path itself, so a
new one gets it by somebody remembering, and the header said as much: "a
blanket claim nobody has checked is how the next target gets added without
it." This is the check, so the claim stops being blanket.

WHY THIS IS A SCRIPT AND NOT THE AWK IT REPLACES

The awk in .github/workflows/ci.yml pulled the target name off the same line
as `add_executable(` and gave up silently when it was not there. CMake does
not require it to be:

    add_executable(
        revenant_widget_tests
        test_widget.cpp
    )

is the same call, and the name the awk extracted from that first line was the
empty string, which matches nothing ending in `_tests`, so the whole target
was skipped. Not reported, not counted, not exempt: absent. A guard that
quietly covers less than it claims is the defect this guard exists to catch,
one level up, and the `count == 0` backstop could never fire on it because
six other targets were being found.

It reads the name across lines now, and it says out loud when a call's target
cannot be determined at all rather than dropping it.

WHY IT SCANS ui/ AS WELL AS tests/

The one target that has never had the file is the one in the other CMake
project, which is exactly where a guard rooted in tests/ would not look.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

SOURCE = "no_modal_dialogs.cpp"

# A target this check knows about and does not require the file of. Naming it
# here is the difference between a known gap and an unnoticed one: an eighth
# target arrives red, not quiet.
#
# revenant_ui_tests opens no Vulkan context and holds no abort() path of its
# own, so it is the cheapest of the seven to leave out, and adding the source
# to ui/ is a change to that project rather than to this one.
EXEMPT = {"revenant_ui_tests"}

# Which targets this is about. A test target is a target whose name ends this
# way, which is the tree's own convention and is what the awk matched.
TARGET = re.compile(r"_tests$")

OPEN = re.compile(r"\badd_executable\s*\(")
COMMENT = re.compile(r"#.*$")


@dataclass
class Call:
    target: str | None
    has_source: bool
    path: str
    line: int


def scan_file(path: Path, rel: str) -> list[Call]:
    """Every add_executable call in one CMakeLists, with its target and flag.

    A small state machine rather than a regex over the whole file, because
    the name may be on the opening line, on the next one, or after a comment
    line, and because a call ends at the first `)` at any depth this tree
    uses inside one.
    """
    calls: list[Call] = []
    open_at = 0
    target: str | None = None
    has_source = False
    inside = False

    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = COMMENT.sub("", raw)

        if not inside:
            match = OPEN.search(line)
            if match is None:
                continue
            inside = True
            open_at = number
            target = None
            has_source = False
            line = line[match.end():]

        if target is None:
            probe = line.strip()
            if probe and not probe.startswith(")"):
                target = re.split(r"[\s)]", probe, maxsplit=1)[0]

        if SOURCE in line:
            has_source = True

        if ")" in line:
            calls.append(Call(target, has_source, rel, open_at))
            inside = False

    if inside:
        calls.append(Call(target, has_source, rel, open_at))
    return calls


def tracked_lists(root: Path, roots: list[str]) -> list[Path]:
    """CMakeLists.txt under the named directories, from git.

    git rather than a walk, for the reason check_retired_claims.py gives: it
    answers exactly which files are source, so a build directory containing a
    generated CMakeLists can never be scanned.
    """
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--"]
        + [f"{d}/**/CMakeLists.txt" for d in roots]
        + [f"{d}/CMakeLists.txt" for d in roots],
        check=True,
        capture_output=True,
    )
    names = sorted({n for n in out.stdout.decode("utf-8").split("\0") if n})
    return [root / name for name in names]


SELF_TEST_CASES = [
    (
        "name on the opening line",
        "add_executable(revenant_widget_tests\n"
        "    ../support/no_modal_dialogs.cpp\n"
        "    test_widget.cpp\n"
        ")\n",
        [("revenant_widget_tests", True)],
    ),
    (
        "name on a continuation line",
        "add_executable(\n"
        "    revenant_widget_tests\n"
        "    ../support/no_modal_dialogs.cpp\n"
        "    test_widget.cpp\n"
        ")\n",
        [("revenant_widget_tests", True)],
    ),
    (
        "name on a continuation line and the file missing",
        "add_executable(\n"
        "    revenant_widget_tests\n"
        "    test_widget.cpp\n"
        ")\n",
        [("revenant_widget_tests", False)],
    ),
    (
        "a comment between the paren and the name",
        "add_executable(\n"
        "    # the widget suite\n"
        "    revenant_widget_tests\n"
        "    test_widget.cpp\n"
        ")\n",
        [("revenant_widget_tests", False)],
    ),
    (
        "all on one line",
        "add_executable(revenant_widget_tests a.cpp ../support/no_modal_dialogs.cpp)\n",
        [("revenant_widget_tests", True)],
    ),
    (
        "indented inside an if",
        "if(TARGET Catch2::Catch2)\n"
        "    add_executable(revenant_widget_tests\n"
        "        test_widget.cpp\n"
        "    )\n"
        "endif()\n",
        [("revenant_widget_tests", False)],
    ),
    (
        "two calls in one file",
        "add_executable(revenant_one_tests ../support/no_modal_dialogs.cpp)\n"
        "add_executable(\n"
        "    revenant_two_tests\n"
        "    b.cpp\n"
        ")\n",
        [("revenant_one_tests", True), ("revenant_two_tests", False)],
    ),
]


def self_test(tmp: Path) -> int:
    """Prove the scanner sees a target it used to drop, every run.

    The second case is the whole reason this file exists. A ratchet nobody
    has watched fail is the same defect as a stale comment.
    """
    bad = 0
    for index, (name, source, expected) in enumerate(SELF_TEST_CASES):
        path = tmp / f"case{index}.txt"
        path.write_text(source, encoding="utf-8")
        got = [(call.target, call.has_source) for call in scan_file(path, name)]
        if got != expected:
            print(f"self-test FAILED: {name}: expected {expected}, got {got}")
            bad += 1
            continue
        print(f"self-test ok: {name}: {got}")
    if bad:
        print(f"\n{bad} self-test case(s) failed: this check cannot be trusted.")
        return 1
    print(f"\nself-test: {len(SELF_TEST_CASES)} cases, all as expected")
    return 0


def main(argv: list[str]) -> int:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(here.parent), help="repository root")
    parser.add_argument(
        "--dir",
        dest="dirs",
        action="append",
        default=None,
        help="a directory to scan; repeatable, defaults to tests and ui",
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            return self_test(Path(tmp))

    root = Path(args.root).resolve()
    dirs = args.dirs or ["tests", "ui"]

    try:
        lists = tracked_lists(root, dirs)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"git ls-files failed in {root}: {exc}", file=sys.stderr)
        return 2

    if not lists:
        print(
            f"no CMakeLists.txt under {', '.join(dirs)}; this guard is looking in the "
            "wrong place",
            file=sys.stderr,
        )
        return 1

    scanned = 0
    missing: list[Call] = []
    unnamed: list[Call] = []

    for path in lists:
        rel = path.relative_to(root).as_posix()
        for call in scan_file(path, rel):
            if call.target is None:
                # An add_executable whose target this cannot read. Loud,
                # because the old guard's failure was exactly a call it could
                # not read and did not mention.
                unnamed.append(call)
                continue
            if not TARGET.search(call.target):
                continue
            scanned += 1
            if call.has_source:
                print(f"ok      {call.target} ({call.path}:{call.line})")
            elif call.target in EXEMPT:
                print(f"exempt  {call.target} ({call.path}:{call.line})")
            else:
                missing.append(call)

    # A guard that scanned nothing and passed is the shape this whole check is
    # about. Zero means the tree moved.
    if scanned == 0:
        print(
            f"no *_tests targets found under {', '.join(dirs)}; this guard is looking "
            "in the wrong place",
            file=sys.stderr,
        )
        return 1
    print(f"scanned {scanned} test target(s) in {len(lists)} CMakeLists.txt")

    status = 0
    for call in unnamed:
        print(f"{call.path}:{call.line}: add_executable with no readable target name")
        status = 1

    for call in missing:
        print(f"{call.path}:{call.line}: {call.target} does not list {SOURCE}")
        status = 1

    if status != 0:
        print()
        print(f"Add tests/support/{SOURCE} to the target's source list, or add the")
        print("target to EXEMPT in this script with the reason it does not need it.")
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
