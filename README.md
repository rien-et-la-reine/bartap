# bartap

A zero-heap, zero-dependency EPUB reading library in ISO C11, for microcontrollers. The caller owns all
memory and pulls content one record at a time.

**Status:** the library is complete. It is tested on the host over generated fixtures, built and sized
for the Arm Cortex-M33, and run big-endian under QEMU. A real-book corpus and the on-device
measurements (R51) are still to come. `REWRITE-DECISIONS.md` is the
authoritative design; decisions are cited below as R-numbers.

## What it does

- Opens an EPUB from any random-access source: a read callback, or an archive in memory.
- Streams each spine item as layout records: block starts, containers (lists, quotes, notes), text,
  style changes, images and line breaks. Broken markup is recovered from, never rejected (R47).
- Reads the table of contents from the EPUB 3 nav or the NCX, and resolves an entry to a position.
- Saves and restores reading positions as 23-byte bookmarks. A bookmark lands exactly while the
  chapter and the library's text rules are unchanged. If either changes, it falls back to the nearest
  block or to the chapter start (R12).
- Seeks quickly with an optional rolling buffer of recent bytes and checkpoints (R48).
- Streams any entry (images, the cover) with CRC and size checks, and verifies entries in bounded
  steps (R31, R38).

It never allocates and calls nothing but `memcpy`, `memmove`, `memset` and `memcmp`. Every buffer is
passed in, so its memory use is fixed at compile time.

## Using it

```c
#include "bartap.h"

static uint8_t window[BARTAP_WINDOW_SIZE]; /* the inflate window, one open object at a time */
static bartap_open_work_t work;            /* scratch for bartap_open only */
static bartap_spine_item_t spine[256];
static bartap_book_t book;
static bartap_text_t text;
static char text_buf[256];                 /* at least BARTAP_TEXT_MIN */

int read_sd(void *user, uint32_t offset, uint8_t *buf, uint32_t len); /* 0 on success */

bartap_err_t read_chapter(uint32_t archive_size, uint16_t chapter)
{
    bartap_source_t src = {read_sd, NULL, archive_size};
    bartap_err_t err = bartap_open(&book, &src, spine, 256, NULL, 0, &work, window, NULL, NULL);
    if (err != BARTAP_OK) {
        return err;
    }
    err = bartap_text_open(&text, &book, chapter, window, text_buf, sizeof text_buf, NULL, NULL, NULL);
    if (err != BARTAP_OK) {
        return err;
    }
    bartap_record_t rec;
    while ((err = bartap_text_next(&text, &rec)) == BARTAP_OK) {
        switch (rec.kind) {
        case BARTAP_REC_BLOCK_START: /* new paragraph, heading, pre, hr or dt */ break;
        case BARTAP_REC_TEXT:        /* rec.text.ptr, rec.text.len: whole UTF-8 characters */ break;
        case BARTAP_REC_STYLE:       /* rec.style.bits for the text that follows */ break;
        default:                     break;
        }
    }
    return err == BARTAP_END ? BARTAP_OK : err;
}
```

- **Positions:** call `bartap_bookmark_write` with a block start's `block_offset` and a character
  offset into the block. Later, call `bartap_bookmark_read` and then `bartap_text_seek` on the opened
  item. After a seek, the records restate the containers and style around the landing point (R49).
- **Directory and spine memory:** `bartap_open` picks modes from what fits, and none changes any
  result. A `dir_buf` that holds the whole central directory keeps it in RAM. One that only holds an
  index of it (10 bytes per entry) makes each lookup read one record instead of walking the
  directory on the card. A spine array with fewer slots than the book has items holds up to three
  compact items per slot, each looked up when opened.
- **Faster seeks:** give `bartap_text_open` a `bartap_rolling_t` set up by `bartap_rolling_init` over a
  power-of-two ring. Seeks backwards are then served from the ring and checkpoints, not by re-reading
  the chapter from its start.
- **Memory mode:** if a chapter fits in RAM (PSRAM, say), inflate it with `bartap_resource_open_spine`
  and read it with `bartap_text_open_memory`. Seeks then never touch the card.
- **TOC:** `bartap_toc_open` and `bartap_toc_next` give `BARTAP_REC_TOC_ENTRY` records with styled
  label text. `bartap_toc_resolve` turns an entry's fragment into a bookmark.
- **Images:** an image record holds a directory offset. Pass it to `bartap_resource_open_dir`, then
  call `bartap_resource_read`. A window belongs to the object opened on it until that object is done
  with (R27), so reading an image while its chapter stays open needs a second window, or the
  chapter in memory mode.
- **Warnings:** every object keeps sticky warning bits (`bartap_*_warnings`), and each open call takes
  an optional callback with the byte offset of each warning (R32).

`include/bartap.h` documents each call, its buffers and its lifetimes.

## Build and test

```sh
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
cmake -B build-san -DBARTAP_SANITIZE=ON && cmake --build build-san && ctest --test-dir build-san --output-on-failure
cmake -B build-fuzz -DBARTAP_FUZZ=ON -DCMAKE_C_COMPILER=clang && cmake --build build-fuzz
```

The tests need Python 3 and zlib. The fuzz build produces libFuzzer targets, with ASan and UBSan, in
`build-fuzz/tests/` (`fuzz_xml`, `fuzz_xml_readahead1`, `fuzz_open`, `fuzz_text`, `fuzz_toc`,
`fuzz_bookmark`). Every build also runs the same targets as standalone replays over generated inputs.

- **Fixtures:** `tests/fixtures/gen.py` builds deterministic EPUBs into the build tree. Each one's
  dump must match `tests/fixtures/expected.txt`. After an intended change in output, refresh that
  file with `tests/fixtures/check.py --update`.
- **Cross checks:** with `arm-none-eabi-gcc`, the `arm_cortex_m33` test checks symbols and reports
  `sizeof`, code size and stack frames (`ctest -R arm_cortex_m33 -V`). With `qemu-armeb` as well,
  `fixtures_big_endian` runs the fixture dump big-endian and compares it with the host's. Without
  the tools, both tests are skipped.

Two build options tune memory against speed without changing any output (R33):
`BARTAP_INPUT_BUF` (compressed bytes per inflate refill, default 512) and `BARTAP_XML_READAHEAD`
(tokenizer lookahead, default 256).

To use the library from another project, `add_subdirectory` this repository and link `bartap`; tests
are off by default in that case.
