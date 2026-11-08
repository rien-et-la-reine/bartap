#!/usr/bin/env python3
"""Run each program with the same arguments and fail unless every output is identical (R33).

Usage: compare_outputs.py PROGRAM... [-- ARG...]
"""
import subprocess
import sys


def main() -> int:
    argv = sys.argv[1:]
    split = argv.index("--") if "--" in argv else len(argv)
    programs, args = argv[:split], argv[split + 1:]
    if len(programs) < 2:
        print("need at least two programs to compare")
        return 2

    outputs = [subprocess.run([p, *args], check=True, capture_output=True).stdout for p in programs]
    if not outputs[0]:
        print(f"{programs[0]} printed nothing, so there is nothing to compare")
        return 1
    reference = outputs[0].splitlines()
    failed = False
    for program, output in zip(programs[1:], outputs[1:]):
        lines = output.splitlines()
        if lines == reference:
            continue
        failed = True
        print(f"{program} differs from {programs[0]}")
        for ours, theirs in zip(reference, lines):
            if ours != theirs:
                print(f"  first difference: {ours.decode()} vs {theirs.decode()}")
                break
        else:
            print(f"  {len(reference)} lines vs {len(lines)}")
    if not failed:
        print(f"{len(programs)} builds agree on {len(reference)} documents")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
