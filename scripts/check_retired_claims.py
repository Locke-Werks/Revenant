#!/usr/bin/env python3
"""Fail when a retired claim has come back somewhere in the tree.

This project states a measured fact in several places on purpose: a comment
that records the measurement beside the code is the habit that makes the tree
worth reading. The cost is that correcting the fact in one place leaves the
other copies asserting the old thing, in files a reader trusts and acts on.
Nine of those were found in a single day, and the fix for one of them
introduced another.

So the retraction is what gets recorded. docs/retired-claims.txt lists every
phrase that has stopped being true, and this walks the tree looking for each
one. The point is not the first correction, which somebody already made. It is
the fifth copy, three months later, pasted by someone who read a file nobody
had swept.

WHY THIS IS A SCRIPT AND NOT A CATCH2 CASE

Its input is the source tree, not the build. `git ls-files` answers exactly
which files are source, which is a question a test binary would have to
re-derive with a list of directory exclusions that goes stale the first time
somebody adds a build directory.

The test binary would also carry every retired phrase in its own read-only
data, so the one artifact anybody might grep for a phrase would be the one
guaranteed to contain all of them. Keeping the phrases out of anything the
compiler touches keeps "never scan the build tree" true by construction rather
than by a rule somebody has to remember.

And it runs on the cheap runner. The GPU legs of this project's CI take tens of
minutes on a self-hosted workstation. A prose check that only answers after
that is a check people learn to route around; this one answers in the guards
job, on a hosted runner, before anything compiles. CTest runs it too, so a
developer sees it locally, but CI's copy is the gate.

HOW A RETRACTION QUOTES ITSELF WITHOUT TRIPPING THIS

The house convention keeps the withdrawn wording visible under "WHAT THIS
PARAGRAPH USED TO SAY", so a retired phrase appears, deliberately, right next
to its own correction. That has to keep working or the mechanism is unusable.

The rule is proximity, not an allowlist: an occurrence is permitted when a
retraction marker appears on its own line or on one of the WINDOW_LINES lines
above it. A file-and-line allowlist was the obvious alternative and it is the
wrong shape twice over. It records WHERE a phrase may appear, which is a second
copy of the fact this whole mechanism exists to stop duplicating, and it rots
on the first line inserted above the entry. Proximity records WHY the
occurrence is legitimate, and the reason travels with the text through every
edit, reflow and file move.

Proximity can be abused: paste a marker next to a stale claim and it goes
quiet. That is a deliberate lie sitting in a diff with the words "used to say"
on it, which a reviewer can see. An allowlist entry is a line number nobody
reads.

WHAT IT CANNOT DO

It catches a false statement coming back. It cannot catch a true statement
going missing: a field whose comment simply never mentioned that the value
moves has no phrase to list. That failure needs a reader, not a grep.
"""

from __future__ import annotations

import argparse
import bisect
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

# How far below a retraction marker a quoted phrase may sit.
#
# Measured against this tree: the longest retraction block today is the first
# of the two in core/detect/detector.h, which runs 28 lines from its marker,
# and every phrase either of them quotes is inside that. Thirty leaves a little
# slack and is still short enough that a stale claim in the next paragraph down
# is caught rather than covered.
WINDOW_LINES = 30

# What opens a retraction window.
#
# These are the words this tree already writes, so the check did not invent a
# vocabulary anybody has to learn. Case-insensitive because the convention
# shouts the marker in some files and writes it in prose in others, and both
# are the same act.
#
# Deliberately NOT here: the bare words "retracted" and "retraction". They
# occur in ordinary prose about a retraction that lives somewhere else, and one
# of those sits 25 lines above a genuinely stale claim in
# core/rpc/revenant.capnp, so admitting them would have silenced the defect
# this check was written to catch.
MARKER = re.compile(
    r"used to say|used to mean|used to read|now false|said the opposite",
    re.IGNORECASE,
)

DEFAULT_LIST = "docs/retired-claims.txt"

# Binary payloads git tracks. The NUL sniff below catches anything not listed;
# this just avoids reading a megabyte of icon to find out.
BINARY_SUFFIXES = {
    ".ico", ".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp",
    ".ttf", ".otf", ".woff", ".woff2",
    ".wav", ".bin", ".spv", ".pdf", ".zip", ".7z",
}

# Leading punctuation that carries no meaning for a prose claim: comment
# openers in every language in this tree plus markdown's heading marks.
LEADING_NOISE = re.compile(r"^\s*(?:///|//!|//|/\*+|\*+/|\*+|#+|<!--|-->|;;?)\s?")
TRAILING_NOISE = re.compile(r"(?:\*/|-->)\s*$")

# Removed outright, on both sides of the comparison.
#
# Quotes and backticks bracket a claim without being part of it: the same
# sentence is fenced in a markdown document, double-quoted inside a retraction
# and bare in the header it came from. Apostrophes go with them so a curly one
# pasted out of a document matches the straight one beside the code. Asterisks
# are markdown emphasis. Underscores are NOT removed: source_center is an
# identifier and the difference matters.
DROPPED_CHARS = str.maketrans("", "", "`*\"'“”‘’")


def normalise(line: str) -> str:
    """One line, reduced to what the claim actually says.

    Case and line wrapping are the two things that change without the claim
    changing. Comments here wrap at about 76 columns, so nearly every claim
    longer than a few words is split across lines with a `// ` in the middle,
    and a literal match would miss the common case and pass green. Case moves
    when a sentence is reflowed or when this tree shouts a claim for emphasis,
    which is exactly the copy a reader believes.
    """
    line = LEADING_NOISE.sub("", line)
    line = TRAILING_NOISE.sub("", line)
    line = line.translate(DROPPED_CHARS)
    return " ".join(line.lower().split())


@dataclass
class Flattened:
    """A file as one normalised string, with a way back to line numbers."""

    text: str
    offsets: list[int] = field(default_factory=list)
    lines: list[int] = field(default_factory=list)

    def line_of(self, offset: int) -> int:
        index = bisect.bisect_right(self.offsets, offset) - 1
        if index < 0:
            return 1
        return self.lines[index]


def flatten(source: str) -> Flattened:
    chunks: list[str] = []
    offsets: list[int] = []
    lines: list[int] = []
    cursor = 0
    for number, raw in enumerate(source.splitlines(), start=1):
        cleaned = normalise(raw)
        if not cleaned:
            continue
        offsets.append(cursor)
        lines.append(number)
        chunks.append(cleaned)
        cursor += len(cleaned) + 1
    return Flattened(" ".join(chunks), offsets, lines)


def marker_lines(source: str) -> list[int]:
    return [
        number
        for number, raw in enumerate(source.splitlines(), start=1)
        if MARKER.search(raw)
    ]


def covered(line: int, markers: list[int]) -> int | None:
    """The marker that legitimises an occurrence on this line, if any."""
    index = bisect.bisect_right(markers, line) - 1
    if index < 0:
        return None
    nearest = markers[index]
    return nearest if line - nearest <= WINDOW_LINES else None


@dataclass
class Record:
    phrases: list[str]
    retired: str
    commit: str
    instead: str
    line: int


class ListError(Exception):
    pass


FIELD = re.compile(r"^([a-z]+):\s*(.*)$")
DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")

# Short phrases match half the tree. Twenty-four normalised characters is about
# four or five words, which is the floor at which a phrase is a claim rather
# than a turn of speech.
MIN_PHRASE = 24


def parse_list(path: Path) -> list[Record]:
    records: list[Record] = []
    current: dict[str, list[str]] = {}
    start = 0
    last_key: str | None = None

    def flush() -> None:
        nonlocal current, last_key
        if not current:
            return
        missing = [k for k in ("phrase", "retired", "commit", "instead") if k not in current]
        if missing:
            raise ListError(
                f"{path}:{start}: record is missing {', '.join(missing)}"
            )
        for key in ("retired", "commit", "instead"):
            if len(current[key]) != 1:
                raise ListError(f"{path}:{start}: {key} is given more than once")
        if not DATE.match(current["retired"][0]):
            raise ListError(
                f"{path}:{start}: retired must be YYYY-MM-DD, got "
                f"{current['retired'][0]!r}"
            )
        records.append(
            Record(
                phrases=list(current["phrase"]),
                retired=current["retired"][0],
                commit=current["commit"][0],
                instead=current["instead"][0],
                line=start,
            )
        )
        current = {}
        last_key = None

    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if raw.lstrip().startswith("#"):
            continue
        if not raw.strip():
            flush()
            continue
        if raw[:1] in " \t" and last_key is not None:
            current[last_key][-1] += " " + raw.strip()
            continue
        match = FIELD.match(raw)
        if not match:
            raise ListError(f"{path}:{number}: not a 'key: value' line: {raw!r}")
        key, value = match.group(1), match.group(2).strip()
        if key not in ("phrase", "retired", "commit", "instead"):
            raise ListError(f"{path}:{number}: unknown field {key!r}")
        if not current:
            start = number
        current.setdefault(key, []).append(value)
        last_key = key
    flush()

    if not records:
        raise ListError(f"{path}: no records; the checker is scanning for nothing")

    # Structural checks on the list itself. None of these can go stale, and each
    # one is a way of writing an entry that looks like protection and is not.
    seen: dict[str, int] = {}
    for record in records:
        for phrase in record.phrases:
            key = normalise(phrase)
            if len(key) < MIN_PHRASE:
                raise ListError(
                    f"{path}:{record.line}: phrase normalises to {len(key)} "
                    f"characters, under the {MIN_PHRASE} minimum: {phrase!r}"
                )
            if key in seen:
                raise ListError(
                    f"{path}:{record.line}: phrase already listed at line "
                    f"{seen[key]}: {phrase!r}"
                )
            seen[key] = record.line
    keys = sorted(seen, key=len)
    for index, shorter in enumerate(keys):
        for longer in keys[index + 1:]:
            if shorter in longer:
                raise ListError(
                    f"{path}:{seen[longer]}: this phrase contains the one at "
                    f"line {seen[shorter]}, so every hit would be reported "
                    f"twice; keep the shorter one"
                )
    return records


def tracked_files(root: Path) -> list[Path]:
    try:
        out = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"],
            check=True,
            capture_output=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ListError(
            f"git ls-files failed in {root}: {exc}. This check scans tracked "
            "files so it can never read the build tree; it does not fall back "
            "to walking the filesystem, because a scan that quietly covers "
            "less than it claims is the defect it exists to catch."
        ) from exc
    names = [n for n in out.stdout.decode("utf-8").split("\0") if n]
    if not names:
        raise ListError(f"git ls-files listed nothing in {root}; the checkout is wrong")
    return [root / name for name in names]


def readable(path: Path) -> str | None:
    if path.suffix.lower() in BINARY_SUFFIXES:
        return None
    try:
        data = path.read_bytes()
    except OSError:
        return None
    if b"\0" in data[:8192]:
        return None
    return data.decode("utf-8", errors="replace")


@dataclass
class Hit:
    path: str
    line: int
    record: Record
    phrase: str
    marker: int | None


def scan(root: Path, records: list[Record], skip: set[Path]) -> tuple[list[Hit], list[Hit], int]:
    keyed = [(normalise(p), p, r) for r in records for p in r.phrases]
    violations: list[Hit] = []
    quoted: list[Hit] = []
    scanned = 0

    for path in tracked_files(root):
        if path.resolve() in skip:
            continue
        source = readable(path)
        if source is None:
            continue
        scanned += 1
        flat = flatten(source)
        if not flat.text:
            continue
        markers: list[int] | None = None
        rel = path.relative_to(root).as_posix()
        for key, phrase, record in keyed:
            start = flat.text.find(key)
            while start != -1:
                line = flat.line_of(start)
                if markers is None:
                    markers = marker_lines(source)
                marker = covered(line, markers)
                hit = Hit(rel, line, record, phrase, marker)
                (quoted if marker is not None else violations).append(hit)
                start = flat.text.find(key, start + 1)
    violations.sort(key=lambda h: (h.path, h.line))
    quoted.sort(key=lambda h: (h.path, h.line))
    return violations, quoted, scanned


SELF_TEST_PHRASE = "the frobnicator is read once when the kettle is opened"

SELF_TEST_CASES = [
    (
        "bare occurrence",
        "// Something else entirely.\n"
        "// The frobnicator is read once when the kettle is opened.\n",
        True,
    ),
    (
        "wrapped across two comment lines",
        "// The frobnicator is read once\n"
        "// when the kettle is opened, which is why it cannot follow.\n",
        True,
    ),
    (
        "shouted, fenced and reflowed",
        "Note: `THE FROBNICATOR IS READ   ONCE WHEN THE KETTLE IS OPENED`.\n",
        True,
    ),
    (
        "quoted inside a retraction",
        "// WHAT THIS PARAGRAPH USED TO SAY\n"
        "//\n"
        "// The frobnicator is read once when the kettle is opened. It is not:\n"
        "// set_frobnicator moves it.\n",
        False,
    ),
    (
        "too far below the marker to be its quote",
        "// WHAT THIS PARAGRAPH USED TO SAY: something unrelated.\n"
        + "//\n" * WINDOW_LINES
        + "// The frobnicator is read once when the kettle is opened.\n",
        True,
    ),
    (
        "marker on the same line",
        "// This used to say the frobnicator is read once when the kettle is opened.\n",
        False,
    ),
]


def self_test() -> int:
    """Prove the matcher can fail, every run, not once by hand.

    A ratchet nobody has watched fail is the same defect as a stale comment:
    something everyone believes and nothing has checked.
    """
    key = normalise(SELF_TEST_PHRASE)
    bad = 0
    for name, source, expect_violation in SELF_TEST_CASES:
        flat = flatten(source)
        start = flat.text.find(key)
        if start == -1:
            found_violation = False
            matched = False
        else:
            matched = True
            line = flat.line_of(start)
            found_violation = covered(line, marker_lines(source)) is None
        if start == -1 and expect_violation:
            print(f"self-test FAILED: {name}: the phrase was not matched at all")
            bad += 1
            continue
        if found_violation != expect_violation:
            want = "reported" if expect_violation else "allowed"
            got = "reported" if found_violation else "allowed"
            print(f"self-test FAILED: {name}: expected {want}, got {got}")
            bad += 1
            continue
        state = "reported" if found_violation else f"allowed (matched={matched})"
        print(f"self-test ok: {name}: {state}")
    if bad:
        print(f"\n{bad} self-test case(s) failed: this check cannot be trusted.")
        return 1
    print(f"\nself-test: {len(SELF_TEST_CASES)} cases, all as expected")
    return 0


def main(argv: list[str]) -> int:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(here.parent), help="repository root")
    parser.add_argument("--list", dest="listfile", default=None, help="the claim list")
    parser.add_argument(
        "--report",
        action="store_true",
        help="also print the occurrences a retraction marker legitimises",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="check that the matcher reports what it should and allows what it should",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    root = Path(args.root).resolve()
    listfile = Path(args.listfile).resolve() if args.listfile else root / DEFAULT_LIST
    if not listfile.is_file():
        print(f"the claim list is missing: {listfile}", file=sys.stderr)
        return 2

    try:
        records = parse_list(listfile)
        # The list holds every retired phrase by definition, so it is the one
        # file that must not be scanned.
        violations, quoted, scanned = scan(root, records, {listfile})
    except ListError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    phrases = sum(len(r.phrases) for r in records)
    print(
        f"{len(records)} retired claim(s), {phrases} phrase(s), "
        f"{scanned} tracked text file(s) scanned"
    )

    if args.report:
        for hit in quoted:
            print(f"  quoted  {hit.path}:{hit.line} (retraction at line {hit.marker})")

    if not violations:
        print(f"{len(quoted)} occurrence(s), all inside a retraction. No claim has come back.")
        return 0

    print()
    for hit in violations:
        print(f"{hit.path}:{hit.line}: a retired claim is stated here")
        print(f'    phrase:  "{hit.phrase}"')
        print(f"    retired: {hit.record.retired} in {hit.record.commit}")
        print(f"    true:    {hit.record.instead}")
        print()
    print(
        f"{len(violations)} occurrence(s) of a retired claim, with no retraction "
        f"marker within {WINDOW_LINES} lines above."
    )
    print()
    print("Either the statement is wrong and needs correcting, or you are")
    print("quoting it deliberately, in which case put it under the retraction")
    print(f"that withdraws it. {DEFAULT_LIST} has the format and the rule.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
