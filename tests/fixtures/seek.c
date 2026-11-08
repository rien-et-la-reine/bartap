/* P1 and P2 over the generated fixtures (R48, phase 10): in every item that reads, seeks to a
 * spread of landing points, in each mode, at two ring sizes and text buffers, from a fresh open and
 * from the item's end. Then every TOC entry resolves to a landing point that satisfies P2 (R13).
 *
 * Usage: test_fixture_seek FILE... */
#include <stdlib.h>

#include "check.h"

#define TX_SPINE_MAX 1024u
#define ITEMS_MAX (1 << 21)
#define SP_TEXT_MAX (1u << 22)
#include "seek_prop.h"

#define POINTS_PER_ITEM 24
#define TOC_MAX 512u

static uint8_t file[1u << 24];
static uint8_t item[1u << 23];
static bartap_resource_t res;
static bartap_toc_t toc;

/* TOC entries, collected before any item is read: they share the window. */
typedef struct {
    uint16_t spine;
    char fragment[256];
    uint32_t fragment_len;
} entry_t;

static entry_t entries[TOC_MAX];
static uint32_t entry_n;

/* ---- Loading ---- */

static bool load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return false;
    }
    size_t size = fread(file, 1, sizeof file, f);
    fclose(f);
    CHECK(bartap_source_memory(&tx_source, file, (uint32_t)size) == BARTAP_OK);
    return bartap_open(&tx_book, &tx_source, tx_spine, TX_SPINE_MAX, NULL, 0, &tx_work, tx_window, NULL,
                       NULL) == BARTAP_OK;
}

static void collect_toc(void)
{
    bartap_record_t r;
    entry_n = 0;
    if (bartap_toc_open(&toc, &tx_book, tx_window, tx_buf, 4096, NULL, NULL) != BARTAP_OK) {
        return;
    }
    while (bartap_toc_next(&toc, &r) == BARTAP_OK) {
        if (r.kind != BARTAP_REC_TOC_ENTRY || r.toc.spine_index == BARTAP_NO_SPINE || entry_n == TOC_MAX ||
            r.toc.fragment_len > sizeof entries[0].fragment) {
            continue;
        }
        entry_t *e = &entries[entry_n++];
        e->spine = r.toc.spine_index;
        e->fragment_len = r.toc.fragment_len;
        if (r.toc.fragment_len != 0) {
            memcpy(e->fragment, r.toc.fragment, r.toc.fragment_len);
        }
    }
}

/* Inflates item i for the memory modes, and checks it reads to its end; false to skip it. */
static bool prepare_item(uint16_t i)
{
    if (bartap_resource_open_spine(&res, &tx_book, i, tx_window) != BARTAP_OK ||
        bartap_resource_size(&res) > sizeof item) {
        return false;
    }
    uint32_t n = 0;
    bartap_err_t err;
    do {
        uint32_t read = 0;
        err = bartap_resource_read(&res, item + n, (uint32_t)sizeof item - n, &read);
        n += read;
    } while (err == BARTAP_OK);
    if (err != BARTAP_END) {
        return false;
    }
    sp_spine = i;
    sp_data = item;
    sp_size = n;

    /* an item that fails as a whole, such as UTF-16, has nothing to seek in */
    bartap_record_t r;
    CHECK(bartap_text_open_memory(&tx_text, &tx_book, i, item, n, tx_buf, 4096, NULL, NULL, NULL) ==
          BARTAP_OK);
    while ((err = bartap_text_next(&tx_text, &r)) == BARTAP_OK) {
    }
    return err == BARTAP_END;
}

/* ---- Seeks ---- */

static void seek_all_modes(const point_t *p, int s, const char *name)
{
    static const uint32_t rings[] = {512, 1u << 14};
    static const uint32_t text_caps[] = {BARTAP_TEXT_MIN, 4096};
    ring_size = rings[s % 2];
    uint32_t cap = text_caps[(s / 2) % 2];
    int warm = s % 3 == 0 ? 0 : 1 << 30;
    bartap_bookmark_t bm = bookmark(p->block, p->chr);

    for (int mode = 0; mode < MODE_COUNT; mode++) {
        seek_read(mode, cap, warm, &bm, &outs[mode]);
        bool ok = mode == 0 ? p2_holds(p, &outs[0]) : same_raw(&outs[0], &outs[mode]);
        if (!ok) {
            printf("%s item %u: seek to %u+%u fails %s in mode %d (ring %u, cap %u, warm %d)\n", name,
                   sp_spine, p->block, p->chr, mode == 0 ? "P2" : "P1", mode, ring_size, cap, warm);
            check_failures++;
        }
    }
}

static void seek_item(const char *name)
{
    straight_read();
    if (point_n == 0) {
        return;
    }
    int step = point_n / POINTS_PER_ITEM + 1;
    int s = 0;
    for (int k = 0; k < point_n; k += step) {
        seek_all_modes(&points[k], s++, name);
    }
    /* the last landing point, at the end of the last block */
    seek_all_modes(&points[point_n - 1], s, name);
}

/* The records of a fresh open read to the end, kept as seek_read keeps them. */
static void fresh_read(seek_out_t *o)
{
    bartap_record_t r;
    bartap_err_t err;
    open_mode(MODE_STREAM_BARE, 64);
    o->raw_n = 0;
    o->text_n = 0;
    while ((err = bartap_text_next(&tx_text, &r)) == BARTAP_OK) {
        if (o->raw_n < sizeof o->raw / sizeof o->raw[0]) {
            o->raw[o->raw_n] = r;
            if (r.kind == BARTAP_REC_TEXT && o->text_n + r.text.len <= sizeof o->text) {
                memcpy(o->text + o->text_n, r.text.ptr, r.text.len);
                o->raw[o->raw_n].text.ptr = o->text + o->text_n;
                o->text_n += r.text.len;
            }
            o->raw_n++;
        }
    }
    CHECK(err == BARTAP_END);
}

/* Every TOC entry with a fragment into this item lands on a point of the straight read, and the
 * records from there are P2's. A fragment not found gives the chapter start with a warning, and the
 * records from there are a fresh read's (R13, R48). An entry with no fragment needs no resolving:
 * the caller opens the item. */
static void follow_toc(const char *name)
{
    for (uint32_t k = 0; k < entry_n; k++) {
        const entry_t *e = &entries[k];
        if (e->spine != sp_spine || e->fragment_len == 0) {
            continue;
        }
        bartap_bookmark_t bm;
        open_mode(MODE_MEMORY, 4096);
        CHECK(bartap_toc_resolve(&tx_text, e->fragment, e->fragment_len, &bm) == BARTAP_OK);
        if ((bartap_text_warnings(&tx_text) & BARTAP_WARN_BROKEN_LINK) != 0) {
            CHECK(bm.block_offset == 0 && bm.char_offset == 0);
            ring_size = 512;
            seek_read(MODE_STREAM_RING, 64, 1 << 30, &bm, &outs[0]);
            fresh_read(&outs[1]);
            if (!same_raw(&outs[0], &outs[1])) {
                printf("%s item %u: #%.*s, not found, doesn't seek to the chapter start\n", name, sp_spine,
                       (int)e->fragment_len, e->fragment);
                check_failures++;
            }
            continue;
        }
        const point_t *p = NULL;
        for (int i = 0; i < point_n && p == NULL; i++) {
            if (points[i].block == bm.block_offset && points[i].chr == bm.char_offset) {
                p = &points[i];
            }
        }
        if (p == NULL) {
            printf("%s item %u: #%.*s resolves to %u+%u, not a landing point\n", name, sp_spine,
                   (int)e->fragment_len, e->fragment, bm.block_offset, bm.char_offset);
            check_failures++;
            continue;
        }
        ring_size = 512;
        seek_read(MODE_STREAM_RING, 64, 0, &bm, &outs[0]);
        if (!p2_holds(p, &outs[0])) {
            printf("%s item %u: #%.*s fails P2 after resolving\n", name, sp_spine, (int)e->fragment_len,
                   e->fragment);
            check_failures++;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s FILE...\n", argv[0]);
        return 2;
    }
    for (int a = 1; a < argc; a++) {
        const char *name = strrchr(argv[a], '/') != NULL ? strrchr(argv[a], '/') + 1 : argv[a];
        if (!load(argv[a])) {
            printf("%s: didn't open\n", name);
            check_failures++;
            continue;
        }
        collect_toc();
        uint16_t count = 0;
        uint32_t required = 0;
        bartap_book_spine_count(&tx_book, &count, &required);
        uint32_t seeked = 0;
        for (uint16_t i = 0; i < count; i++) {
            if (prepare_item(i)) {
                seek_item(name);
                follow_toc(name);
                seeked++;
            }
        }
        printf("%s: %u of %u items, %u toc entries\n", name, seeked, count, entry_n);
    }
    return CHECK_RESULT();
}
