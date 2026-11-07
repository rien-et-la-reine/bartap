#!/usr/bin/env python3
"""Fail if a static library needs any external symbol beyond the four mem* functions (R34, R35).

Usage: check_symbols.py NM LIBRARY
"""
import fnmatch
import subprocess
import sys

ALLOWED = (
    "memcpy", "memset", "memcmp", "memmove",
    "__aeabi_*",               # Arm compiler helpers, including its mem* variants
    "_GLOBAL_OFFSET_TABLE_",   # position-independent builds on the host
)


def main() -> int:
    nm, library = sys.argv[1:3]
    output = subprocess.run([nm, library], check=True, capture_output=True, text=True).stdout

    # Undefined in one object and defined in another is an internal call between layers.
    defined, undefined = set(), set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 3:
            defined.add(fields[2])
        elif len(fields) == 2 and fields[0] == "U":
            undefined.add(fields[1])

    forbidden = sorted(
        name for name in undefined - defined
        if not any(fnmatch.fnmatchcase(name, pattern) for pattern in ALLOWED)
    )
    for name in forbidden:
        print(f"forbidden symbol: {name}")
    return 1 if forbidden else 0


if __name__ == "__main__":
    sys.exit(main())
