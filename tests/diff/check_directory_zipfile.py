#!/usr/bin/env python3
"""Compare Bartap's view of real zip files with Python's zipfile, the reference (R36).

Also requires raw, none and hashed modes to agree (R24), the first match to win (R44), and every readable
entry to reach END through a stream, which proves its bytes against its CRC (R31).

Usage: check_directory_zipfile.py DUMP
"""
import io
import random
import subprocess
import sys
import tempfile
import warnings
import zipfile
from pathlib import Path

METHOD_UNSUPPORTED = 0xFF
FIND_LIMIT = 2000
RECORD_MIN = 46


def fold(name: bytes) -> bytes:
    return bytes(c + 32 if 65 <= c <= 90 else c for c in name)


def name_hash(name: bytes) -> int:
    value = 0x811C9DC5
    for c in fold(name):
        value = ((value ^ c) * 16777619) & 0xFFFFFFFF
    return value


def raw_name(info: zipfile.ZipInfo) -> bytes:
    encoding = "utf-8" if info.flag_bits & 0x800 else "cp437"
    return info.orig_filename.encode(encoding)


def expected_lines(path: Path) -> tuple[int, int, list[tuple]]:
    """What zipfile reports, in directory order."""
    with zipfile.ZipFile(path) as z:
        offset = z.start_dir
        first_seen: dict[bytes, int] = {}
        rows = []
        for info in z.filelist:
            name = raw_name(info)
            first_seen.setdefault(fold(name), offset)
            method = info.compress_type if info.compress_type in (0, 8) else METHOD_UNSUPPORTED
            rows.append((offset, info.header_offset, info.compress_size, info.file_size, info.CRC,
                         method, name_hash(name), name.hex(), first_seen[fold(name)]))
            offset += RECORD_MIN + len(name) + len(info.extra) + len(info.comment)
        return z.start_dir, offset - z.start_dir, rows


def check(dump: str, label: str, path: Path) -> list[str]:
    start, size, rows = expected_lines(path)
    result = subprocess.run([dump, str(path)], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        output = result.stdout[-400:] + result.stderr[-400:]
        return [f"{label}: dump failed ({result.returncode}): {output}"]

    raw, none, hashed = result.stdout.split("---\n")
    raw_lines = raw.splitlines()
    none_lines = none.splitlines()
    hashed_lines = hashed.splitlines()
    errors = []

    # Identical apart from the mode's own name in the first line (R24).
    if raw_lines[1:] != none_lines[1:]:
        errors.append(f"{label}: raw and none disagree")
    if raw_lines[1:] != hashed_lines[1:]:
        errors.append(f"{label}: raw and hashed disagree")
    # an empty directory fits the empty buffer whole
    want_hashed = "raw" if size == 0 else "hashed"
    if hashed_lines[0] != f"directory {start} {size} {want_hashed}":
        errors.append(f"{label}: hashed directory line {hashed_lines[0]!r}, want {start} {size}")
    if raw_lines[0] != f"directory {start} {size} raw":
        errors.append(f"{label}: raw directory line {raw_lines[0]!r}, want {start} {size}")
    if none_lines[0] != f"directory {start} {size} none":
        errors.append(f"{label}: none directory line {none_lines[0]!r}, want {start} {size}")
    if raw_lines[-1] != "end BARTAP_END":
        errors.append(f"{label}: walk finished with {raw_lines[-1]!r}")

    lines = raw_lines[1:-1]
    if len(lines) != len(rows):
        errors.append(f"{label}: {len(lines)} entries, zipfile has {len(rows)}")

    for line, row in zip(lines, rows):
        fields = line.split(" ")
        if len(fields) != 10:
            errors.append(f"{label}: bad line {line!r}")
            continue
        got = tuple(int(f) for f in fields[:7]) + (fields[7],)
        if got != row[:8]:
            errors.append(f"{label}: got {got}, zipfile has {row[:8]}")

        want_stream = "BARTAP_ERR_UNSUPPORTED" if row[5] == METHOD_UNSUPPORTED else "BARTAP_END"
        if fields[8] != want_stream:
            errors.append(f"{label}: {row[7]} read as {fields[8]}, want {want_stream}")

        find = int(fields[9])
        if len(rows) <= FIND_LIMIT and find != row[8]:
            errors.append(f"{label}: {row[7]} found at {find}, want the first at {row[8]}")
    return errors[:20]


def write_zip(path: Path, entries, comment: bytes = b"", suffix: bytes = b"") -> None:
    with zipfile.ZipFile(path, "w") as z:
        for entry in entries:
            info, data = entry
            z.writestr(info, data)
        z.comment = comment
    if suffix:
        with open(path, "ab") as f:
            f.write(suffix)


def entry(name: str, data: bytes, method: int = zipfile.ZIP_DEFLATED, extra: bytes = b"",
          comment: bytes = b"") -> tuple:
    info = zipfile.ZipInfo(name)
    info.compress_type = method
    info.extra = extra
    info.comment = comment
    return info, data


def book_entries(rng: random.Random) -> list[tuple]:
    words = [bytes(rng.choices(b"abcdefghij ", k=rng.randint(2, 9))) for _ in range(200)]
    text = b" ".join(rng.choices(words, k=20000))
    return [
        entry("mimetype", b"application/epub+zip", zipfile.ZIP_STORED),
        entry("META-INF/container.xml", b"<container/>"),
        entry("OEBPS/content.opf", text[:3000]),
        entry("OEBPS/Text/chapter one.xhtml", text),
        entry("OEBPS/Images/cover.jpg", rng.randbytes(50000), zipfile.ZIP_STORED),
        entry("OEBPS/empty-stored", b"", zipfile.ZIP_STORED),
        entry("OEBPS/empty-deflated", b""),
        entry("OEBPS/zeros", bytes(1 << 20)),
    ]


def build_cases(folder: Path) -> list[tuple[str, Path]]:
    rng = random.Random(0x5EED)
    cases = []

    def case(label: str, *args, **kwargs) -> None:
        path = folder / f"{label}.zip"
        write_zip(path, *args, **kwargs)
        cases.append((label, path))

    case("book", book_entries(rng))
    case("empty", [])
    case("comment", book_entries(rng), comment=b"a comment")
    case("longest comment", [entry("a", b"data")], comment=bytes(65535))
    case("padding", book_entries(rng), suffix=bytes(500))
    case("comment and padding", [entry("a", b"data")], comment=b"note", suffix=bytes(64))
    case("extra fields", [
        entry("a", b"data", extra=b"\x99\x99\x04\x00abcd"),
        entry("b", b"data", zipfile.ZIP_STORED, extra=b"\x99\x99\x00\x00\x98\x98\x02\x00xy"),
    ])
    case("entry comments", [
        entry("a", b"data", comment=b"first"),
        entry("b", b"more", comment=b"x" * 900),
    ])
    case("unicode names", [
        entry("OEBPS/café.xhtml", b"data"),
        entry("OEBPS/日本語.xhtml", b"data"),
        entry("OEBPS/CAFÉ.xhtml", b"data"),
    ])
    case("long name", [entry("d/" * 2000 + "file", b"data"), entry("x" * 65535, b"data")])
    case("case duplicates", [
        entry("OEBPS/Text.xhtml", b"first"),
        entry("oebps/text.xhtml", b"second"),
        entry("OEBPS/TEXT.XHTML", b"third"),
    ])
    case("exact duplicates", [entry("same", b"first"), entry("same", b"second")])
    case("bzip2", [entry("a", b"data" * 100, zipfile.ZIP_BZIP2), entry("b", b"data")])
    case("lzma", [entry("a", b"data" * 100, zipfile.ZIP_LZMA), entry("b", b"data")])

    # ZIP64 end records, from having more entries than the classic field holds.
    case("many entries", [entry(f"f{i}", b"", zipfile.ZIP_STORED) for i in range(70000)])
    case("exactly 65535 entries", [entry(f"f{i}", b"", zipfile.ZIP_STORED) for i in range(65535)])

    # ZIP64 extra fields, written into each record for local offsets... zipfile only does that past
    # 4 GB, so the extra field is covered by hand in tests/unit/test_directory.c instead.
    path = folder / "force zip64.zip"
    with zipfile.ZipFile(path, "w") as z:
        with z.open(zipfile.ZipInfo("a"), "w", force_zip64=True) as f:
            f.write(b"data")
    cases.append(("force zip64", path))

    return cases


def main() -> int:
    dump = sys.argv[1]
    warnings.simplefilter("ignore")
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        for label, path in build_cases(Path(tmp)):
            errors = check(dump, label, path)
            print(f"{'FAIL' if errors else 'ok  '} {label}")
            failures += errors
    for failure in failures:
        print(failure)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
