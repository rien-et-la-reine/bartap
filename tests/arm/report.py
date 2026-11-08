#!/usr/bin/env python3
"""Cross-compiles Bartap for the Pico 2's Cortex-M33 (R34, R51, phase 10), then:

  - checks the library needs nothing but mem* and __aeabi_* helpers (check_symbols.py);
  - reports sizeof each caller-owned type on the target, from sizes.c's objects;
  - reports code and data size per layer, and the largest stack frames (-fstack-usage, static:
    R51 measures the real high-water marks on the device).

Fails on any compiler warning, as the host build does.

Usage: report.py CC NM SIZE SOURCE_DIR WORK_DIR
"""
import re
import subprocess
import sys
from pathlib import Path

FLAGS = ["-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft", "-Os", "-std=c11", "-ffunction-sections",
         "-fdata-sections", "-fno-stack-protector", "-fstack-usage",
         "-Wall", "-Wextra", "-Wconversion", "-pedantic", "-Werror"]
LAYERS = ["zip", "xml", "package", "text"]


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def main():
    if len(sys.argv) != 6:
        print(__doc__.strip().splitlines()[-1])
        return 2
    cc, nm, size, source, work = sys.argv[1], sys.argv[2], sys.argv[3], Path(sys.argv[4]), Path(sys.argv[5])
    work.mkdir(parents=True, exist_ok=True)
    include = ["-I", str(source / "include"), "-I", str(source / "src")]

    # the library, one object per layer
    objects = []
    for layer in LAYERS:
        obj = work / f"{layer}.o"
        run([cc, *FLAGS, *include, "-c", str(source / "src" / f"{layer}.c"), "-o", str(obj)])
        objects.append(obj)
    library = work / "libbartap.a"
    library.unlink(missing_ok=True)
    run([cc.replace("gcc", "ar") if cc.endswith("gcc") else "ar", "rcs", str(library), *map(str, objects)])

    symbols = subprocess.run([sys.executable, str(source / "tests" / "check_symbols.py"), nm, str(library)],
                             capture_output=True, text=True)
    print(symbols.stdout, end="")
    if symbols.returncode != 0:
        print("the Arm library needs a forbidden symbol (R34, R35)")
        return 1
    print("symbols: only mem* and __aeabi_* helpers")

    # sizeof on the target
    sizes_obj = work / "sizes.o"
    run([cc, *[f for f in FLAGS if f != "-fstack-usage"], *include, "-c",
         str(source / "tests" / "arm" / "sizes.c"), "-o", str(sizes_obj)])
    print("\nsizeof on Cortex-M33:")
    for line in run([nm, "-S", "--size-sort", str(sizes_obj)]).splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[3].startswith("size_"):
            print(f"  {fields[3][5:]:<22} {int(fields[1], 16):>7}")

    # code and data per layer
    print("\ncode and data (bytes):")
    total = [0, 0, 0]
    print(f"  {'layer':<10} {'text':>7} {'data':>7} {'bss':>7}")
    for line in run([size, *map(str, objects)]).splitlines()[1:]:
        text, data, bss = (int(x) for x in line.split()[:3])
        name = Path(line.split()[-1]).stem
        total = [total[0] + text, total[1] + data, total[2] + bss]
        print(f"  {name:<10} {text:>7} {data:>7} {bss:>7}")
    print(f"  {'total':<10} {total[0]:>7} {total[1]:>7} {total[2]:>7}")
    if total[1] != 0 or total[2] != 0:
        print("the library has static data: all state is the caller's (R34)")
        return 1

    # static frame sizes
    frames = []
    for su in work.glob("*.su"):
        for line in su.read_text().splitlines():
            m = re.match(r".*:\d+:\d+:(\S+)\s+(\d+)\s+(\S+)", line)
            if m:
                frames.append((int(m.group(2)), m.group(1), m.group(3)))
    frames.sort(reverse=True)
    print("\nlargest stack frames (static, one frame each):")
    for frame, name, kind in frames[:8]:
        print(f"  {name:<34} {frame:>6}  {kind}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
