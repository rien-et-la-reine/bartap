#!/usr/bin/env python3
"""Checks the fixture dump (phase 10, R51) against the expected lines, and the properties that hold
between fixtures.

Each fixture's lines in the expected file follow a sha256 line for the archive they were made from.
zlib builds can deflate differently, which moves directory offsets and so the hashes. When a
generated fixture's bytes differ, its lines are compared with the hashes masked, and a note says so.

Usage: check.py DUMP FIXTURE_DIR EXPECTED [--update]
"""
import hashlib
import re
import subprocess
import sys
from pathlib import Path

HASH = re.compile(r"\b[0-9a-f]{16}\b")
IMAGE = re.compile(r" image \d+ ")


def mask(line):
    """A line without what depends on the archive's bytes: hashes and directory offsets."""
    return IMAGE.sub(" image - ", HASH.sub("-", line))

# ---- Properties between fixtures ----

# Pairs whose lines must be identical apart from the file name: the nav and NCX versions of one book
# (R26), and the TOC fault books, whose chapter is the same.
SAME = [("toc_nav", "toc_ncx", ("item", "toc")),
        ("toc_faults", "toc_faults_ncx", ("item",))]


def lines_of(lines, name, kinds):
    prefix = f"{name}.epub "
    out = []
    for line in lines:
        if line.startswith(prefix):
            rest = line[len(prefix):]
            if rest.split(" ", 1)[0] in kinds:
                out.append(rest)
    return out


def check_pairs(lines):
    failed = False
    for a, b, kinds in SAME:
        if lines_of(lines, a, kinds) != lines_of(lines, b, kinds):
            print(f"{a} and {b} differ in their {' and '.join(kinds)} lines")
            failed = True
    return failed


# ---- Expected lines ----

def parse_expected(text):
    """{fixture: (sha256, [lines])}"""
    out = {}
    current = None
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        name, rest = line.split(" ", 1)
        if rest.startswith("sha256 "):
            current = name
            out[name] = (rest.split(" ", 1)[1], [])
        elif current is not None:
            out[current][1].append(line)
    return out


def main():
    args = sys.argv[1:]
    update = "--update" in args
    args = [a for a in args if a != "--update"]
    if len(args) != 3:
        print(__doc__.strip().splitlines()[-1])
        return 2
    dump, fixture_dir, expected_path = args[0], Path(args[1]), Path(args[2])

    fixtures = sorted(fixture_dir.glob("*.epub"))
    if not fixtures:
        print(f"no fixtures in {fixture_dir}")
        return 1
    run = subprocess.run([dump, *map(str, fixtures)], capture_output=True, text=True)
    if run.returncode != 0:
        print(run.stdout + run.stderr)
        print(f"{dump} failed: its own agreement checks (R6, R11, R24, R50)")
        return 1
    lines = run.stdout.splitlines()
    shas = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in fixtures}

    failed = check_pairs(lines)

    if update:
        with open(expected_path, "w") as out:
            out.write("# Written by check.py --update from fixture_dump: the host's half of R51.\n")
            for name, sha in shas.items():
                out.write(f"{name} sha256 {sha}\n")
                out.writelines(line + "\n" for line in lines if line.startswith(name + " "))
        print(f"wrote {expected_path}")
        return 1 if failed else 0

    expected = parse_expected(expected_path.read_text())
    for name, sha in shas.items():
        got = [line for line in lines if line.startswith(name + " ")]
        if name not in expected:
            print(f"{name}: not in {expected_path.name}; run with --update")
            failed = True
            continue
        want_sha, want = expected[name]
        if sha != want_sha:
            print(f"note: {name} was generated differently (another zlib?): comparing without hashes")
            got = [mask(line) for line in got]
            want = [mask(line) for line in want]
        if got != want:
            failed = True
            print(f"{name} differs from {expected_path.name}:")
            for line in want:
                if line not in got:
                    print(f"  - {line}")
            for line in got:
                if line not in want:
                    print(f"  + {line}")
    for name in expected:
        if name not in shas:
            print(f"{name}: expected, but not generated")
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
