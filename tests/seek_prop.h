/* Shared by the seek tests and the text fuzz target: a straight read as comparable items, every
 * landing point in it, seeks in each mode, and the P1 and P2 checks (R48). Test code only.
 * The includer defines CHECK: check.h's, or an abort for a fuzz target. */
#ifndef BARTAP_TEST_SEEK_PROP_H
#define BARTAP_TEST_SEEK_PROP_H

#include "text_fixture.h"

/* Limits the includer may raise before including: items in a straight read, and bytes of text kept
 * from a seek for comparing. */
#ifndef ITEMS_MAX
#define ITEMS_MAX 200000
#endif
#ifndef SP_TEXT_MAX
#define SP_TEXT_MAX (1u << 18)
#endif

/* The chapter memory modes read: spine item sp_spine's bytes. Stream modes read the book's item
 * sp_spine. */
static const uint8_t *sp_data;
static uint32_t sp_size;
static uint16_t sp_spine;

/* ---- The straight read, and every landing point in it ---- */

/* A record as a comparable item, style per character so restated styles don't matter (P2). */
typedef struct {
    char kind;
    int a;
    int b;
    uint32_t c;
} item_t;

typedef struct {
    uint32_t block;
    uint32_t chr;
    int at;      /* index into straight just after the landing */
    int depth;   /* containers open there */
} point_t;

static item_t straight[ITEMS_MAX];
static int straight_n;
static point_t points[ITEMS_MAX];
static int point_n;
static item_t got[ITEMS_MAX];

static inline int to_items(const bartap_record_t *r, int *style, item_t *out)
{
    int n = 0;
    switch (r->kind) {
    case BARTAP_REC_BLOCK_START:
        out[n++] = (item_t){'B', (int)r->block.kind, r->block.level, r->block.block_offset};
        break;
    case BARTAP_REC_CONTAINER_START:
        out[n++] = (item_t){'[', (int)r->container.kind, r->container.ordinal, 0};
        break;
    case BARTAP_REC_CONTAINER_END:
        out[n++] = (item_t){']', (int)r->container.kind, 0, 0};
        break;
    case BARTAP_REC_STYLE:
        *style = (int)r->style.bits;
        break;
    case BARTAP_REC_IMAGE:
        out[n++] = (item_t){'I', 0, 0, r->image.dir_offset};
        break;
    case BARTAP_REC_LINE_BREAK:
        out[n++] = (item_t){'|', 0, 0, 0};
        break;
    case BARTAP_REC_TEXT:
        for (uint32_t i = 0; i < r->text.len; i++) {
            out[n++] = (item_t){'T', (uint8_t)r->text.ptr[i], *style, 0};
        }
        break;
    default:
        out[n++] = (item_t){'?', (int)r->kind, 0, 0};
    }
    return n;
}

static inline bool same_item(const item_t *x, const item_t *y)
{
    return x->kind == y->kind && x->a == y->a && x->b == y->b && x->c == y->c;
}

static inline void straight_read(void)
{
    bartap_record_t r;
    item_t tmp[4096];
    int style = 0;
    int depth = 0;
    bool in_block = false;
    uint32_t block = 0;
    uint32_t chr = 0;

    straight_n = 0;
    point_n = 0;
    CHECK(bartap_text_open_memory(&tx_text, &tx_book, sp_spine, sp_data, sp_size, tx_buf, 4096, NULL,
                                  NULL, NULL) == BARTAP_OK);
    while (bartap_text_next(&tx_text, &r) == BARTAP_OK) {
        int n = to_items(&r, &style, tmp);
        CHECK(straight_n + n <= ITEMS_MAX);
        for (int i = 0; i < n; i++) {
            straight[straight_n++] = tmp[i];
            if (tmp[i].kind == 'B') {
                block = tmp[i].c;
                chr = 0;
                in_block = true;
                points[point_n++] = (point_t){block, 0, straight_n, depth};
            } else if (tmp[i].kind == '[' || tmp[i].kind == ']') {
                depth += tmp[i].kind == '[' ? 1 : -1;
                in_block = false;
            } else if (tmp[i].kind == 'T' && in_block) {
                chr++;
                points[point_n++] = (point_t){block, chr, straight_n, depth};
            }
        }
    }
    /* drop landings inside a character: the next byte would be a continuation */
    int w = 0;
    for (int i = 0; i < point_n; i++) {
        int at = points[i].at;
        if (points[i].chr > 0 && at < straight_n && straight[at].kind == 'T' &&
            ((uint8_t)straight[at].a & 0xC0u) == 0x80u) {
            continue;
        }
        points[w++] = points[i];
    }
    point_n = w;
}

/* ---- Seeking in each mode ---- */

enum { MODE_STREAM_RING, MODE_MEMORY, MODE_MEMORY_VIRTUAL, MODE_STREAM_BARE, MODE_COUNT };

static uint8_t ring[1u << 14];
static bartap_rolling_t rolling;
static uint32_t ring_size = 512;

static inline void open_mode(int mode, uint32_t cap)
{
    bartap_err_t err;
    switch (mode) {
    case MODE_STREAM_RING:
        CHECK(bartap_rolling_init(&rolling, ring, ring_size) == BARTAP_OK);
        err = bartap_text_open(&tx_text, &tx_book, sp_spine, tx_window, tx_buf, cap, &rolling, NULL,
                               NULL);
        break;
    case MODE_MEMORY:
        err = bartap_text_open_memory(&tx_text, &tx_book, sp_spine, sp_data, sp_size, tx_buf, cap, NULL,
                                      NULL, NULL);
        break;
    case MODE_MEMORY_VIRTUAL:
        CHECK(bartap_rolling_init(&rolling, NULL, ring_size) == BARTAP_OK);
        err = bartap_text_open_memory(&tx_text, &tx_book, sp_spine, sp_data, sp_size, tx_buf, cap,
                                      &rolling, NULL, NULL);
        break;
    default:
        err = bartap_text_open(&tx_text, &tx_book, sp_spine, tx_window, tx_buf, cap, NULL, NULL, NULL);
        break;
    }
    CHECK(err == BARTAP_OK);
}

static inline bartap_bookmark_t bookmark(uint32_t block, uint32_t chr)
{
    bartap_bookmark_t bm = {BARTAP_BOOKMARK_VERSION, tx_book.book_hash, sp_spine, block, chr,
                            tx_spine[sp_spine].crc32};
    return bm;
}

/* The raw records after a seek, for P1, and the items after the restated context, for P2. */
typedef struct {
    bartap_record_t raw[1u << 15];
    char text[SP_TEXT_MAX];
    uint32_t raw_n;
    uint32_t text_n;
    int items;
    int contexts;     /* container starts before the block start */
    uint32_t chr;     /* the block start's character offset */
    bool has_block;
} seek_out_t;

static seek_out_t outs[MODE_COUNT];

static inline void seek_read(int mode, uint32_t cap, int warm, const bartap_bookmark_t *bm, seek_out_t *o)
{
    bartap_record_t r;
    open_mode(mode, cap);
    for (int i = 0; i < warm; i++) {
        if (bartap_text_next(&tx_text, &r) != BARTAP_OK) {
            break;
        }
    }
    CHECK(bartap_text_seek(&tx_text, bm) == BARTAP_OK);

    int style = 0;
    int phase = 0; /* context, then block start, then its style, then the rest */
    o->raw_n = 0;
    o->text_n = 0;
    o->items = 0;
    o->contexts = 0;
    o->chr = 0;
    o->has_block = false;
    bartap_err_t err;
    while ((err = bartap_text_next(&tx_text, &r)) == BARTAP_OK) {
        if (o->raw_n < sizeof o->raw / sizeof o->raw[0]) {
            o->raw[o->raw_n] = r;
            if (r.kind == BARTAP_REC_TEXT && o->text_n + r.text.len <= sizeof o->text) {
                /* the buffer is reused, keep a copy for comparing */
                memcpy(o->text + o->text_n, r.text.ptr, r.text.len);
                o->raw[o->raw_n].text.ptr = o->text + o->text_n;
                o->text_n += r.text.len;
            }
            o->raw_n++;
        }
        if (phase == 0 && r.kind == BARTAP_REC_CONTAINER_START) {
            o->contexts++;
            continue;
        }
        if (phase == 0 && r.kind == BARTAP_REC_BLOCK_START) {
            o->chr = r.block.char_offset;
            o->has_block = true;
            phase = 1;
            got[o->items++] = (item_t){'B', (int)r.block.kind, r.block.level, r.block.block_offset};
            continue;
        }
        if (phase == 1) {
            phase = 2;
            if (r.kind == BARTAP_REC_STYLE) {
                /* the restated style (R8) */
                style = (int)r.style.bits;
                continue;
            }
            CHECK(r.kind == BARTAP_REC_STYLE);
        }
        o->items += to_items(&r, &style, got + o->items);
    }
    CHECK(err == BARTAP_END);
}

static inline bool same_raw(const seek_out_t *a, const seek_out_t *b)
{
    if (a->raw_n != b->raw_n) {
        return false;
    }
    for (uint32_t i = 0; i < a->raw_n; i++) {
        const bartap_record_t *x = &a->raw[i];
        const bartap_record_t *y = &b->raw[i];
        if (x->kind != y->kind) {
            return false;
        }
        switch (x->kind) {
        case BARTAP_REC_TEXT:
            if (x->text.len != y->text.len || memcmp(x->text.ptr, y->text.ptr, x->text.len) != 0) {
                return false;
            }
            break;
        case BARTAP_REC_BLOCK_START:
            if (x->block.kind != y->block.kind || x->block.level != y->block.level ||
                x->block.block_offset != y->block.block_offset ||
                x->block.char_offset != y->block.char_offset) {
                return false;
            }
            break;
        case BARTAP_REC_CONTAINER_START:
        case BARTAP_REC_CONTAINER_END:
            if (x->container.kind != y->container.kind || x->container.ordinal != y->container.ordinal) {
                return false;
            }
            break;
        case BARTAP_REC_STYLE:
            if (x->style.bits != y->style.bits) {
                return false;
            }
            break;
        case BARTAP_REC_IMAGE:
            if (x->image.dir_offset != y->image.dir_offset) {
                return false;
            }
            break;
        default:
            break;
        }
    }
    return true;
}

/* P2 at one point: the restated context, then exactly the straight read from the point on. */
static bool p2_holds(const point_t *p, const seek_out_t *o)
{
    if (!o->has_block || o->contexts != p->depth || o->chr != p->chr) {
        return false;
    }
    if (got[0].kind != 'B' || got[0].c != p->block || o->items - 1 != straight_n - p->at) {
        return false;
    }
    for (int k = 0; k < straight_n - p->at; k++) {
        if (!same_item(&got[k + 1], &straight[p->at + k])) {
            return false;
        }
    }
    return true;
}

#endif /* BARTAP_TEST_SEEK_PROP_H */
