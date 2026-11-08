/* bartap_open fuzz target (R23, R24, R29, R41). Built for libFuzzer with BARTAP_FUZZ, and always as a
 * standalone replay (fuzz_main.c) over generated EPUBs, many of them damaged.
 *
 * For any archive:
 *   - opening never reads or writes out of bounds, and every loop ends (R29);
 *   - every directory mode (none, raw, hashed) and spine layout (full, compact) gives the same
 *     result, book and warnings (R24, R41);
 *   - an opened book's limits hold: spine count within capacity and required, metadata within its
 *     fields, every item's entry where the directory says;
 *   - every spine item and the cover read the same bytes and end the same way in every mode, and
 *     verify agrees with reading (R31);
 *   - the TOC and every item's text run to their ends within bounds, with well-formed entries (R26). */
#include "epub_build.h"
#include "internal.h"

#define FUZZ_CHECK(cond)                                                                   \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            fprintf(stderr, "%s:%d: fuzz check failed: %s\n", __FILE__, __LINE__, #cond);  \
            abort();                                                                       \
        }                                                                                  \
    } while (0)

#define SPINE_CAP 24u
#define READ_LIMIT (1u << 20) /* a bomb's declared size can be large; read this much at most */

static uint8_t window[BARTAP_WINDOW_SIZE];
static uint8_t dir_bufs[2][1u << 16];
static bartap_open_work_t work;

/* The modes compared: the first is the reference. Hashed gets a buffer one byte short of the
 * directory, which an index always fits (10 bytes an entry against 46 at least), and compact gets
 * a third of the slots the reference used. */
enum { M_NONE, M_RAW, M_HASHED, M_COMPACT, M_COMPACT_HASHED, M_COUNT };
static bartap_book_t books[M_COUNT];
static bartap_spine_item_t spines[M_COUNT][SPINE_CAP];
static bartap_resource_t res;
static bartap_verify_t verify;
static uint8_t chunk[4096];
static uint8_t first[READ_LIMIT];
static bartap_toc_t toc;
static bartap_text_t fz_text;
static char text_buf[64];

#define RECORD_LIMIT (1u << 20) /* stop reading text here, a bomb's declared size can be large */

/* The TOC to its end: entries point into the spine or nowhere, fragments within attribute storage. */
static void check_toc(const bartap_book_t *book, size_t size)
{
    bartap_record_t r;
    bartap_err_t err = bartap_toc_open(&toc, book, window, text_buf, sizeof text_buf, NULL, NULL);
    if (err != BARTAP_OK) {
        FUZZ_CHECK(err == BARTAP_END || err < 0);
        return;
    }
    uint32_t n = 0;
    while ((err = bartap_toc_next(&toc, &r)) == BARTAP_OK) {
        FUZZ_CHECK(++n <= 8u * (uint32_t)size + 64u);
        if (r.kind == BARTAP_REC_TOC_ENTRY) {
            FUZZ_CHECK(r.toc.spine_index == BARTAP_NO_SPINE || r.toc.spine_index < book->spine_count);
            FUZZ_CHECK(r.toc.fragment_len <= BARTAP_ATTR_MAX);
            FUZZ_CHECK((r.toc.fragment == NULL) == (r.toc.fragment_len == 0));
        } else if (r.kind == BARTAP_REC_TEXT) {
            FUZZ_CHECK(r.text.len >= 1 && r.text.len <= sizeof text_buf);
        } else {
            FUZZ_CHECK(r.kind == BARTAP_REC_STYLE);
        }
    }
    FUZZ_CHECK(err == BARTAP_END || err < 0);
}

/* Every item's text, to its end or a failure, containers balanced. */
static void check_text(const bartap_book_t *book)
{
    for (uint16_t i = 0; i < book->spine_count; i++) {
        bartap_record_t r;
        bartap_err_t err = bartap_text_open(&fz_text, book, i, window, text_buf, sizeof text_buf, NULL, NULL, NULL);
        if (err != BARTAP_OK) {
            FUZZ_CHECK(err < 0);
            continue;
        }
        int32_t depth = 0;
        uint32_t n = 0;
        while (n++ < RECORD_LIMIT && (err = bartap_text_next(&fz_text, &r)) == BARTAP_OK) {
            if (r.kind == BARTAP_REC_CONTAINER_START) {
                depth++;
            } else if (r.kind == BARTAP_REC_CONTAINER_END) {
                FUZZ_CHECK(--depth >= 0);
            } else if (r.kind == BARTAP_REC_TEXT) {
                FUZZ_CHECK(r.text.len >= 1 && r.text.len <= sizeof text_buf);
            }
        }
        FUZZ_CHECK(err == BARTAP_OK || err == BARTAP_END || err < 0);
        FUZZ_CHECK(err != BARTAP_END || depth == 0);
    }
}

/* Reads an opened resource up to READ_LIMIT, into first (pass 0) or checked against it (pass 1). */
static bartap_err_t read_resource(int pass, uint32_t *total)
{
    bartap_err_t err;
    uint32_t got;
    *total = 0;
    while ((err = bartap_resource_read(&res, chunk, sizeof chunk, &got)) == BARTAP_OK) {
        FUZZ_CHECK(got > 0 && got <= sizeof chunk);
        if (*total + got > READ_LIMIT) {
            return BARTAP_MORE; /* stopped early, as far as the comparison goes */
        }
        if (pass == 0) {
            memcpy(first + *total, chunk, got);
        } else {
            FUZZ_CHECK(memcmp(first + *total, chunk, got) == 0);
        }
        *total += got;
    }
    return err;
}

/* Verify agrees with reading, a bounded number of steps at a time (R38). */
static void check_verify(const bartap_book_t *book, uint16_t index, bartap_err_t read_result)
{
    if (bartap_verify_init(&verify, book, index, window) != BARTAP_OK) {
        return;
    }
    bartap_err_t err = BARTAP_MORE;
    for (uint32_t done = 0; err == BARTAP_MORE && done <= READ_LIMIT; done += 65536u) {
        err = bartap_verify_step(&verify, 65536u);
    }
    if (read_result == BARTAP_END) {
        FUZZ_CHECK(err == BARTAP_OK);
    } else if (read_result != BARTAP_MORE) {
        FUZZ_CHECK(err == read_result);
    }
}

static void check_book(const bartap_book_t *b)
{
    uint32_t held = b->spine_compact ? (uint32_t)b->spine_capacity * BARTAP_SPINE_COMPACT_PER_ITEM
                                     : b->spine_capacity;
    FUZZ_CHECK(b->spine_count <= held && b->spine_count <= b->spine_required);
    FUZZ_CHECK(b->title_len <= BARTAP_META_MAX && b->creator_len <= BARTAP_META_MAX);
    FUZZ_CHECK(b->language_len <= BARTAP_META_MAX && b->identifier_len <= BARTAP_META_MAX);
    FUZZ_CHECK(b->creator_count >= (b->creator_len != 0 ? 1u : 0u));
    FUZZ_CHECK(b->book_hash == bt_name_hash(BT_NAME_HASH_INIT, b->identifier, b->identifier_len));
    for (uint16_t i = 0; i < b->spine_count; i++) {
        FUZZ_CHECK((bt_spine_flags(b, i) & BT_SPINE_PENDING) == 0);
        bartap_entry_t entry;
        if (bt_spine_entry(b, i, &entry) != BARTAP_OK) {
            continue;
        }
        /* the item's entry is what the directory says (R22) */
        bartap_entry_t again;
        FUZZ_CHECK(bt_dir_entry(&b->directory, entry.dir_offset, &again) == BARTAP_OK);
        FUZZ_CHECK(again.local_offset == entry.local_offset);
        FUZZ_CHECK(again.uncompressed_size == entry.uncompressed_size);
        FUZZ_CHECK(again.crc32 == entry.crc32 && again.method == entry.method);
    }
}

/* Field by field: the struct has padding, which holds whatever was there before. */
static bool same_entry(const bartap_entry_t *x, const bartap_entry_t *y)
{
    return x->dir_offset == y->dir_offset && x->local_offset == y->local_offset &&
           x->compressed_size == y->compressed_size && x->uncompressed_size == y->uncompressed_size &&
           x->crc32 == y->crc32 && x->method == y->method;
}

/* Two books agree on everything a caller can see, and on every item's entry. */
static void same_book(const bartap_book_t *a, const bartap_book_t *b)
{
    FUZZ_CHECK(a->warnings.bits == b->warnings.bits);
    FUZZ_CHECK(a->spine_count == b->spine_count && a->spine_required == b->spine_required);
    FUZZ_CHECK(a->book_hash == b->book_hash && a->creator_count == b->creator_count);
    FUZZ_CHECK(a->title_len == b->title_len && memcmp(a->title, b->title, a->title_len) == 0);
    FUZZ_CHECK(a->nav_dir_offset == b->nav_dir_offset && a->ncx_dir_offset == b->ncx_dir_offset);
    FUZZ_CHECK(a->cover_dir_offset == b->cover_dir_offset);
    for (uint16_t i = 0; i < a->spine_count; i++) {
        uint32_t hash[2];
        uint16_t len[2];
        bt_spine_path(a, i, &hash[0], &len[0]);
        bt_spine_path(b, i, &hash[1], &len[1]);
        FUZZ_CHECK(hash[0] == hash[1] && len[0] == len[1]);
        FUZZ_CHECK((bt_spine_flags(a, i) & BT_SPINE_NONLINEAR) == (bt_spine_flags(b, i) & BT_SPINE_NONLINEAR));
        bartap_entry_t entry[2];
        bartap_err_t err[2] = {bt_spine_entry(a, i, &entry[0]), bt_spine_entry(b, i, &entry[1])};
        FUZZ_CHECK(err[0] == err[1]);
        if (err[0] == BARTAP_OK) {
            FUZZ_CHECK(same_entry(&entry[0], &entry[1]));
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    bartap_source_t source;
    bartap_err_t err[M_COUNT];

    if (size > UINT32_MAX || bartap_source_memory(&source, data, (uint32_t)size) != BARTAP_OK) {
        return 0;
    }
    err[M_NONE] = bartap_open(&books[M_NONE], &source, spines[M_NONE], SPINE_CAP, NULL, 0, &work, window,
                              NULL, NULL);
    err[M_RAW] = bartap_open(&books[M_RAW], &source, spines[M_RAW], SPINE_CAP, dir_bufs[0],
                             sizeof dir_bufs[0], &work, window, NULL, NULL);
    /* sized from the reference's directory, when it got that far */
    uint32_t dir_size = books[M_NONE].directory.size;
    uint32_t short_size = dir_size > 1u && dir_size - 1u < sizeof dir_bufs[1] ? dir_size - 1u : 0u;
    uint16_t compact_cap = (uint16_t)((books[M_NONE].spine_count + 2u) / 3u);
    err[M_HASHED] = bartap_open(&books[M_HASHED], &source, spines[M_HASHED], SPINE_CAP,
                                short_size != 0 ? dir_bufs[1] : NULL, short_size, &work, window, NULL, NULL);
    err[M_COMPACT] = bartap_open(&books[M_COMPACT], &source, spines[M_COMPACT], compact_cap, NULL, 0, &work,
                                 window, NULL, NULL);
    err[M_COMPACT_HASHED] = bartap_open(&books[M_COMPACT_HASHED], &source, spines[M_COMPACT_HASHED],
                                        compact_cap, short_size != 0 ? dir_bufs[1] : NULL, short_size,
                                        &work, window, NULL, NULL);
    /* R24, R41: the modes agree on everything */
    for (int m = 1; m < M_COUNT; m++) {
        FUZZ_CHECK(err[m] == err[M_NONE]);
        FUZZ_CHECK(books[m].warnings.bits == books[M_NONE].warnings.bits);
    }
    if (err[M_NONE] != BARTAP_OK) {
        FUZZ_CHECK(err[M_NONE] < 0);
        return 0;
    }
    /* each mode is the one asked for: hashed whenever its index was built */
    FUZZ_CHECK(books[M_NONE].directory.raw == NULL && books[M_NONE].directory.index == NULL);
    FUZZ_CHECK(dir_size > sizeof dir_bufs[0] || books[M_RAW].directory.raw != NULL);
    FUZZ_CHECK(books[M_HASHED].directory.raw == NULL);
    FUZZ_CHECK(books[M_COMPACT].spine_compact == (books[M_NONE].spine_count > compact_cap));
    const bartap_book_t *a = &books[M_NONE];
    for (int m = 0; m < M_COUNT; m++) {
        if (m != M_NONE) {
            same_book(a, &books[m]);
        }
        check_book(&books[m]);
    }

    /* every spine item reads the same in every mode, and verify agrees */
    for (uint16_t i = 0; i < a->spine_count; i++) {
        bartap_err_t result[M_COUNT];
        uint32_t total[M_COUNT];
        for (int m = 0; m < M_COUNT; m++) {
            bartap_err_t open = bartap_resource_open_spine(&res, &books[m], i, window);
            result[m] = open == BARTAP_OK ? read_resource(m == 0 ? 0 : 1, &total[m]) : open;
            if (open != BARTAP_OK) {
                total[m] = 0;
            }
            FUZZ_CHECK(result[m] == result[0] && total[m] == total[0]);
            check_verify(&books[m], i, result[0]);
        }
        FUZZ_CHECK(result[0] != BARTAP_OK);
    }
    /* the cover, from its directory offset */
    uint32_t cover;
    if (bartap_book_cover(a, &cover) == BARTAP_OK) {
        uint32_t total;
        if (bartap_resource_open_dir(&res, a, cover, window) == BARTAP_OK) {
            (void)read_resource(0, &total);
        }
    }
    check_toc(a, size);
    check_text(a);
    /* and through the lookups a compact item makes, against an index */
    check_toc(&books[M_COMPACT_HASHED], size);
    check_text(&books[M_COMPACT_HASHED]);
    return 0;
}

/* ---- Generated inputs, for the standalone replay ---- */

size_t fuzz_generate(uint32_t seed, uint8_t *out, size_t cap);

static uint32_t gen_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* An EPUB built from random choices, then sometimes damaged a few bytes at a time. */
size_t fuzz_generate(uint32_t seed, uint8_t *out, size_t cap)
{
    static epub_t e;
    static const char *const hrefs[] = {"c%u.xhtml", "../c%u.xhtml", "./x/../c%u.xhtml",
                                        "C%u.XHTML", "c%u.xhtml#f", "http://x/c%u",
                                        "c%%2F%u", "/c%u.xhtml", "c%u.xhtml?q"};
    uint32_t state = seed * 2654435761u + 7u;
    epub_str_t manifest = {0};
    epub_str_t refs = {0};
    epub_str_t text = {0};
    uint32_t chapters = gen_random(&state) % 30u;

    epub_begin(&e);
    if (gen_random(&state) % 4u != 0) {
        epub_add_mimetype(&e);
    }
    if (gen_random(&state) % 10u != 0) {
        epub_add_container(&e, gen_random(&state) % 2u ? "c.opf" : "x/c.opf");
    }
    for (uint32_t i = 0; i < chapters; i++) {
        char name[48];
        snprintf(name, sizeof name, "c%u.xhtml", i);
        const char *href = hrefs[gen_random(&state) % (sizeof hrefs / sizeof hrefs[0])];
        str_add(&manifest, "<item id=\"i%u\" href=\"", i);
        str_add(&manifest, href, i);
        str_add(&manifest, "\"%s/>", gen_random(&state) % 8u == 0 ? " properties=\"nav cover-image\"" : "");
        str_add(&refs, "<itemref idref=\"i%u\"%s/>", gen_random(&state) % chapters,
                gen_random(&state) % 5u == 0 ? " linear=\"no\"" : "");
        if (gen_random(&state) % 8u != 0) {
            text.len = 0;
            str_add(&text, "<p>%u</p>", i);
            uint32_t pad = gen_random(&state) % 3000u;
            for (uint32_t k = 0; k < pad; k++) {
                str_add(&text, "x");
            }
            epub_add(&e, name, text.s, gen_random(&state) % 2u ? EPUB_STORED : EPUB_DEFLATED);
        }
    }
    if (chapters > 0 && gen_random(&state) % 3u == 0) {
        /* a nav, its links resolvable or not */
        text.len = 0;
        str_add(&text, "<html><body><nav epub:type=\"toc\"><ol>");
        for (uint32_t i = 0; i < chapters; i++) {
            str_add(&text, "<li><a href=\"%s#f%u\">L%u</a>%s</li>", gen_random(&state) % 2u ? "c0.xhtml" : "../x.xhtml",
                    i, i, gen_random(&state) % 4u == 0 ? "<ol><li><span>s</span></li></ol>" : "");
        }
        str_add(&text, "</ol></nav></body></html>");
        epub_add(&e, "nav.xhtml", text.s, EPUB_DEFLATED);
        str_add(&manifest, "<item id=\"nav\" href=\"nav.xhtml\" properties=\"nav\"/>");
    }
    const char *opf = gen_random(&state) % 2u ? "c.opf" : "x/c.opf";
    epub_add_opf(&e, opf, "u",
                 gen_random(&state) % 2u ? "<dc:title>T</dc:title><dc:identifier id=\"u\">id</dc:identifier>"
                                         : "<dc:creator>a</dc:creator><dc:creator>b</dc:creator>",
                 manifest.s != NULL ? manifest.s : "", refs.s != NULL ? refs.s : "",
                 gen_random(&state) % 3u == 0 ? "i0" : NULL);
    epub_finish(&e);

    /* damage: a few bytes anywhere, sometimes */
    uint32_t flips = gen_random(&state) % 3u == 0 ? 1u + gen_random(&state) % 8u : 0u;
    for (uint32_t i = 0; i < flips && e.len > 0; i++) {
        e.bytes[gen_random(&state) % e.len] = (uint8_t)gen_random(&state);
    }
    size_t n = e.len < cap ? e.len : cap;
    memcpy(out, e.bytes, n);
    str_free(&manifest);
    str_free(&refs);
    str_free(&text);
    return n;
}
