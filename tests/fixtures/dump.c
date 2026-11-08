/* Prints what Bartap makes of each EPUB named, one line per fact, as hashes of the merged canonical
 * record stream (canon.h). Test code only.
 *
 *   <file> book open=<err> warn=<hex> items=<count>/<required> nonlinear=<indexes> cover=<yes|->
 *   <file> meta <title>|<creator>|<language>|<identifier>
 *   <file> item <i> <hash> end=<err> warn=<hex> verify=<err>
 *   <file> bookmark <i> <bytes in hex> read=<err>   (at the item's last block start, character 1)
 *   <file> image <dir_offset> read=<err> size=<bytes>
 *   <file> toc <hash> entries=<count> end=<err> warn=<hex>
 *
 * The lines are the evidence other checks compare: builds with other buffer sizes must print the same
 * (R33), and the hashes are the host's half of R51. The tool checks some agreement itself and exits 1
 * on a difference: every directory mode (none, raw, hashed) and spine layout (full, compact) print
 * the same (R24, R41), as do stream and memory mode (R11) and text buffers of 4, 7 and 4096 bytes
 * (R6, R50). */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bartap.h"
#include "canon.h"

#define SPINE_MAX 1024u
#define IMAGES_MAX 256u

static uint8_t file[1u << 24];
static uint8_t item[1u << 23];
static uint8_t window[BARTAP_WINDOW_SIZE];
static uint8_t dir_buf[1u << 20];
static bartap_open_work_t work;
static bartap_spine_item_t spine[SPINE_MAX];
static bartap_book_t book;
static bartap_text_t text;
static bartap_toc_t toc;
static bartap_resource_t res;
static bartap_verify_t verify;
static char text_buf[4096];

static const uint32_t caps[] = {4096, BARTAP_TEXT_MIN, 7};

/* ---- Output, kept so the two directory modes can be compared ---- */

typedef struct {
    char s[1u << 20];
    size_t n;
} out_t;

/* The modes each book is dumped in, the first printed. Hashed gets a buffer one byte short of the
 * directory and compact a third of the slots the book needs, both sized from the first. */
enum { M_NONE, M_RAW, M_HASHED, M_COMPACT, M_COMPACT_HASHED, M_COUNT };
static const char *const mode_names[M_COUNT] = {"none", "raw", "hashed", "compact", "compact hashed"};
static out_t outs[M_COUNT];
static bool failed;

static void put(out_t *o, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (o->n < sizeof o->s) {
        int n = vsnprintf(o->s + o->n, sizeof o->s - o->n, fmt, ap);
        if (n > 0) {
            o->n = o->n + (size_t)n < sizeof o->s ? o->n + (size_t)n : sizeof o->s - 1u;
        }
    }
    va_end(ap);
}

/* Prints s with anything outside printable ASCII as \xHH, so a line stays one line. */
static void put_bytes(out_t *o, const char *s, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c >= 0x20u && c < 0x7Fu && c != '\\' && c != '|') {
            put(o, "%c", c);
        } else {
            put(o, "\\x%02X", c);
        }
    }
}

static void mismatch(const char *name, const char *what, uint32_t index)
{
    fprintf(stderr, "%s: %s %u differs\n", name, what, index);
    failed = true;
}

/* ---- Hashing a record stream ---- */

typedef struct {
    uint64_t hash;
    canon_stream_t stream;
    uint32_t records;
    uint32_t last_block;  /* the last block start's offset */
    bool has_block;
    uint32_t images[IMAGES_MAX];
    uint32_t image_n;
} hasher_t;

static void hash_begin(hasher_t *h)
{
    memset(h, 0, sizeof *h);
    h->hash = CANON_HASH_INIT;
    h->stream.sink = canon_hash_sink;
    h->stream.user = &h->hash;
}

static void hash_record(hasher_t *h, const bartap_record_t *r)
{
    h->records++;
    if (r->kind == BARTAP_REC_BLOCK_START) {
        h->last_block = r->block.block_offset;
        h->has_block = true;
    }
    if (!canon_stream_record(&h->stream, r)) {
        fprintf(stderr, "record of kind %d has no canonical form\n", (int)r->kind);
        failed = true;
    }
    if (r->kind == BARTAP_REC_IMAGE) {
        /* first sightings, in order, for reading each image once */
        bool seen = false;
        for (uint32_t i = 0; i < h->image_n && !seen; i++) {
            seen = h->images[i] == r->image.dir_offset;
        }
        if (!seen && h->image_n < IMAGES_MAX) {
            h->images[h->image_n++] = r->image.dir_offset;
        }
    }
}

static uint64_t hash_end(hasher_t *h)
{
    canon_stream_end(&h->stream);
    return h->hash;
}

/* ---- One spine item ---- */

typedef struct {
    uint64_t hash;
    bartap_err_t end;
    uint32_t warn;
} reading_t;

/* Reads item i to its end, in memory mode if data is non-NULL. */
static reading_t read_item(uint16_t i, const uint8_t *data, uint32_t size, uint32_t cap, hasher_t *h)
{
    reading_t got = {0, BARTAP_OK, 0};
    bartap_err_t err = data != NULL
                           ? bartap_text_open_memory(&text, &book, i, data, size, text_buf, cap, NULL, NULL, NULL)
                           : bartap_text_open(&text, &book, i, window, text_buf, cap, NULL, NULL, NULL);
    hash_begin(h);
    if (err == BARTAP_OK) {
        bartap_record_t r;
        while ((err = bartap_text_next(&text, &r)) == BARTAP_OK) {
            hash_record(h, &r);
        }
        got.warn = bartap_text_warnings(&text);
    }
    got.hash = hash_end(h);
    got.end = err;
    return got;
}

/* Inflates item i whole for memory mode. Returns false if it can't be read. */
static bool inflate_item(uint16_t i, uint32_t *size)
{
    if (bartap_resource_open_spine(&res, &book, i, window) != BARTAP_OK ||
        bartap_resource_size(&res) > sizeof item) {
        return false;
    }
    uint32_t n = 0;
    bartap_err_t err;
    for (;;) {
        uint32_t got = 0;
        err = bartap_resource_read(&res, item + n, (uint32_t)sizeof item - n, &got);
        n += got;
        if (err != BARTAP_OK || n == sizeof item) {
            break;
        }
    }
    *size = n;
    return err == BARTAP_END;
}

static bartap_err_t verify_item(uint16_t i)
{
    bartap_err_t err = bartap_verify_init(&verify, &book, i, window);
    while (err == BARTAP_OK || err == BARTAP_MORE) {
        err = bartap_verify_step(&verify, 4096);
        if (err == BARTAP_OK) {
            break;
        }
    }
    return err;
}

static void dump_item(out_t *o, const char *name, uint16_t i, uint32_t *images, uint32_t *image_n)
{
    static hasher_t h;
    reading_t first = read_item(i, NULL, 0, caps[0], &h);
    uint32_t last_block = h.last_block;
    bool has_block = h.has_block;

    /* gather the item's images for reading later */
    for (uint32_t k = 0; k < h.image_n; k++) {
        bool seen = false;
        for (uint32_t m = 0; m < *image_n && !seen; m++) {
            seen = images[m] == h.images[k];
        }
        if (!seen && *image_n < IMAGES_MAX) {
            images[(*image_n)++] = h.images[k];
        }
    }

    /* every text buffer size gives the same content, in either mode (R6, R11, R50) */
    for (uint32_t c = 1; c < sizeof caps / sizeof caps[0]; c++) {
        reading_t other = read_item(i, NULL, 0, caps[c], &h);
        if (other.hash != first.hash || other.end != first.end || other.warn != first.warn) {
            mismatch(name, "stream text buffer size, item", i);
        }
    }
    uint32_t size = 0;
    if (inflate_item(i, &size)) {
        for (uint32_t c = 0; c < sizeof caps / sizeof caps[0]; c++) {
            reading_t other = read_item(i, item, size, caps[c], &h);
            if (other.hash != first.hash || other.end != first.end || other.warn != first.warn) {
                mismatch(name, "memory mode, item", i);
            }
        }
    }
    put(o, "%s item %u %016" PRIx64 " end=%d warn=%x verify=%d\n", name, i, first.hash, first.end,
        first.warn, verify_item(i));

    /* the serialized bytes, byte order and all (R12, R19), and that they read back */
    if (has_block) {
        uint8_t bytes[BARTAP_BOOKMARK_SIZE];
        bartap_bookmark_t back;
        bartap_err_t err = bartap_bookmark_write(&book, i, last_block, 1, bytes);
        put(o, "%s bookmark %u ", name, i);
        for (uint32_t k = 0; k < sizeof bytes; k++) {
            put(o, "%02x", err == BARTAP_OK ? bytes[k] : 0u);
        }
        bartap_err_t read = err == BARTAP_OK ? bartap_bookmark_read(&book, bytes, &back) : err;
        if (read == BARTAP_OK && (back.spine_index != i || back.block_offset != last_block ||
                                  back.char_offset != 1)) {
            mismatch(name, "bookmark round trip, item", i);
        }
        put(o, " read=%d\n", read);
    }
}

/* ---- The TOC ---- */

static void dump_toc(out_t *o, const char *name)
{
    static hasher_t h;
    uint64_t first = 0;
    for (uint32_t c = 0; c < sizeof caps / sizeof caps[0]; c++) {
        bartap_err_t err = bartap_toc_open(&toc, &book, window, text_buf, caps[c], NULL, NULL);
        if (err != BARTAP_OK) {
            if (c == 0) {
                put(o, "%s toc - end=%d\n", name, err);
            }
            return;
        }
        uint32_t entries = 0;
        bartap_record_t r;
        hash_begin(&h);
        while ((err = bartap_toc_next(&toc, &r)) == BARTAP_OK) {
            entries += r.kind == BARTAP_REC_TOC_ENTRY;
            hash_record(&h, &r);
        }
        uint64_t hash = hash_end(&h);
        if (c == 0) {
            first = hash;
            put(o, "%s toc %016" PRIx64 " entries=%u end=%d warn=%x\n", name, hash, entries, err,
                bartap_toc_warnings(&toc));
        } else if (hash != first) {
            mismatch(name, "toc text buffer size", caps[c]);
        }
    }
}

/* ---- One book ---- */

static void dump_book(out_t *o, const char *name, uint32_t size, uint8_t *buf, uint32_t buf_size,
                      uint16_t spine_cap)
{
    static uint32_t images[IMAGES_MAX];
    uint32_t image_n = 0;
    bartap_source_t src;

    o->n = 0;
    o->s[0] = '\0';
    if (bartap_source_memory(&src, file, size) != BARTAP_OK) {
        put(o, "%s source refused\n", name);
        return;
    }
    bartap_err_t err = bartap_open(&book, &src, spine, spine_cap, buf, buf_size, &work, window, NULL, NULL);
    if (err != BARTAP_OK) {
        put(o, "%s book open=%d\n", name, err);
        return;
    }
    uint16_t count = 0;
    uint32_t required = 0;
    uint32_t cover = 0;
    bartap_book_spine_count(&book, &count, &required);
    put(o, "%s book open=0 warn=%x items=%u/%u nonlinear=", name, bartap_book_warnings(&book), count,
        required);
    bool any = false;
    for (uint16_t i = 0; i < count; i++) {
        bool linear = true;
        if (bartap_book_spine_linear(&book, i, &linear) == BARTAP_OK && !linear) {
            put(o, "%s%u", any ? "," : "", i);
            any = true;
        }
    }
    bool has_cover = bartap_book_cover(&book, &cover) == BARTAP_OK;
    put(o, "%s cover=%s\n", any ? "" : "-", has_cover ? "yes" : "-");

    bartap_metadata_t m;
    if (bartap_book_metadata(&book, &m) == BARTAP_OK) {
        put(o, "%s meta ", name);
        put_bytes(o, m.title, m.title_len);
        put(o, "|");
        put_bytes(o, m.creator, m.creator_len);
        put(o, "|");
        put_bytes(o, m.language, m.language_len);
        put(o, "|");
        put_bytes(o, m.identifier, m.identifier_len);
        put(o, "\n");
    }

    for (uint16_t i = 0; i < count; i++) {
        dump_item(o, name, i, images, &image_n);
    }
    if (has_cover && image_n < IMAGES_MAX) {
        bool seen = false;
        for (uint32_t k = 0; k < image_n && !seen; k++) {
            seen = images[k] == cover;
        }
        if (!seen) {
            images[image_n++] = cover;
        }
    }
    /* each image reads to END, its CRC and size checked (R9, R31) */
    for (uint32_t k = 0; k < image_n; k++) {
        uint32_t total = 0;
        err = bartap_resource_open_dir(&res, &book, images[k], window);
        while (err == BARTAP_OK) {
            uint32_t got = 0;
            err = bartap_resource_read(&res, item, sizeof item, &got);
            total += got;
        }
        put(o, "%s image %u read=%d size=%u\n", name, images[k], err, total);
    }
    dump_toc(o, name);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE...\n", argv[0]);
        return 2;
    }
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (f == NULL) {
            perror(argv[a]);
            return 2;
        }
        size_t size = fread(file, 1, sizeof file, f);
        bool whole = feof(f) != 0;
        fclose(f);
        if (!whole) {
            fprintf(stderr, "%s: larger than %zu bytes\n", argv[a], sizeof file);
            return 2;
        }
        /* the name without its directory, so lines don't depend on where the build is */
        const char *name = strrchr(argv[a], '/') != NULL ? strrchr(argv[a], '/') + 1 : argv[a];
        dump_book(&outs[M_NONE], name, (uint32_t)size, NULL, 0, SPINE_MAX);
        uint32_t dir_size = book.directory.size;
        uint32_t short_size = dir_size > 1u ? dir_size - 1u : 0u;
        uint16_t compact_cap = (uint16_t)((book.spine_count + 2u) / 3u);
        bool opened = book.spine_count != 0 || book.directory.size != 0;
        for (int m = M_RAW; m < M_COUNT; m++) {
            bool hashed = m == M_HASHED || m == M_COMPACT_HASHED;
            bool compact = m == M_COMPACT || m == M_COMPACT_HASHED;
            uint8_t *buf = m == M_RAW || (hashed && short_size != 0) ? dir_buf : NULL;
            uint32_t buf_size = m == M_RAW ? (uint32_t)sizeof dir_buf : hashed ? short_size : 0u;
            dump_book(&outs[m], name, (uint32_t)size, buf, buf_size, compact ? compact_cap : SPINE_MAX);
            if (outs[0].n != outs[m].n || memcmp(outs[0].s, outs[m].s, outs[0].n) != 0) {
                fprintf(stderr, "%s: %s mode differs from none\n", name, mode_names[m]);
                failed = true;
            }
            /* and it was that mode */
            bool got = m == M_RAW ? book.directory.raw != NULL
                       : hashed   ? book.directory.index != NULL || short_size == 0
                                  : true;
            got = got && (!compact || book.spine_compact == (book.spine_count > compact_cap));
            if (opened && !got) {
                fprintf(stderr, "%s: %s mode not taken\n", name, mode_names[m]);
                failed = true;
            }
        }
        fputs(outs[0].s, stdout);
    }
    return failed ? 1 : 0;
}
