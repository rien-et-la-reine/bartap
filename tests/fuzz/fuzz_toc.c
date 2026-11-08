/* TOC fuzz target (R13, R26, R29). Built for libFuzzer with BARTAP_FUZZ, and always as a standalone
 * replay (fuzz_main.c) over generated nav and NCX documents.
 *
 * The input's first byte picks the format and whether the book deflates its entries; the rest is the
 * TOC document, in a book with two chapters it can point into. For any input:
 *   - iterating never reads or writes out of bounds, and ends within a bound of the input's size;
 *   - records are well formed: only entries, styles and whole-UTF-8 text, spine indexes in range,
 *     a fragment exactly when it has a length;
 *   - text buffers of 4, 7 and 4096 bytes give the same content (R6, R50), and so does a book with
 *     the document stored instead of deflated;
 *   - every entry with a fragment resolves to a bookmark that seeks and reads to the end (R13). */
#include <stdlib.h>

#define FUZZ_CHECK(cond)                                                                   \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            fprintf(stderr, "%s:%d: fuzz check failed: %s\n", __FILE__, __LINE__, #cond);  \
            abort();                                                                       \
        }                                                                                  \
    } while (0)

#include "text_fixture.h"

#define INPUT_MAX (1u << 20)
#define ENTRIES_MAX 64u

static const char *const chapters[2] = {
    TX_DOC_HEAD "<p id=\"a\">A</p><div id=\"b\"><p>B <i>b</i></p></div><ol><li id=\"c\">C</li></ol>" TX_DOC_TAIL,
    TX_DOC_HEAD "<h1 id=\"x\">X</h1><p>Y <span id=\"y\">z</span> after</p>" TX_DOC_TAIL,
};

static tx_dump_t dumps[2];
static bartap_toc_t toc;

typedef struct {
    uint16_t spine;
    char fragment[1024];
    uint32_t len;
} entry_t;

static entry_t entries[ENTRIES_MAX];
static uint32_t entry_n;

/* Iterates the TOC, checking every record; collects entries when asked. Returns the final result. */
static bartap_err_t read_checked(uint32_t cap, tx_dump_t *d, uint32_t size, bool collect)
{
    bartap_err_t err = bartap_toc_open(&toc, &tx_book, tx_window, tx_buf, cap, NULL, NULL);
    tx_dump_begin(d, 0, 0);
    if (err != BARTAP_OK) {
        /* no TOC at all in the book is END, and only that */
        FUZZ_CHECK(err == BARTAP_END);
        return err;
    }
    bartap_record_t r;
    uint32_t records = 0;
    while ((err = bartap_toc_next(&toc, &r)) == BARTAP_OK) {
        FUZZ_CHECK(++records <= 8u * size + 64u);
        switch (r.kind) {
        case BARTAP_REC_TOC_ENTRY:
            FUZZ_CHECK(r.toc.spine_index == BARTAP_NO_SPINE || r.toc.spine_index < 2u);
            FUZZ_CHECK((r.toc.fragment != NULL) == (r.toc.fragment_len != 0));
            /* a fragment lives in the tokenizer's attribute storage (R26, R33) */
            FUZZ_CHECK(r.toc.fragment_len <= sizeof entries[0].fragment);
            if (collect && r.toc.spine_index != BARTAP_NO_SPINE && r.toc.fragment_len != 0 &&
                entry_n < ENTRIES_MAX) {
                entry_t *e = &entries[entry_n++];
                e->spine = r.toc.spine_index;
                e->len = r.toc.fragment_len;
                memcpy(e->fragment, r.toc.fragment, r.toc.fragment_len);
            }
            break;
        case BARTAP_REC_STYLE:
            FUZZ_CHECK((r.style.bits & ~0x7Fu) == 0);
            break;
        case BARTAP_REC_TEXT: {
            const uint8_t *p = (const uint8_t *)r.text.ptr;
            FUZZ_CHECK(r.text.len >= 1 && r.text.len <= cap && r.text.ptr == tx_buf);
            for (uint32_t i = 0; i < r.text.len;) {
                uint32_t len = p[i] < 0x80u ? 1u : p[i] < 0xC2u ? 0u : p[i] < 0xE0u ? 2u : p[i] < 0xF0u ? 3u
                                                                     : p[i] < 0xF5u ? 4u : 0u;
                FUZZ_CHECK(len != 0 && len <= r.text.len - i);
                FUZZ_CHECK(!(len == 1 && p[i] < 0x20u && p[i] != '\t') && p[i] != 0x7Fu);
                i += len;
            }
            break;
        }
        default:
            /* labels are text and style only */
            FUZZ_CHECK(0);
        }
        tx_dump_record(d, &r);
    }
    tx_dump_end(d);
    if (err != BARTAP_END) {
        /* failed: only as the tokenizer may, and it sticks */
        FUZZ_CHECK(err == BARTAP_ERR_UNSUPPORTED);
        FUZZ_CHECK(bartap_toc_next(&toc, &r) == err);
    }
    return err;
}

static void build(const uint8_t *doc_bytes, bool ncx, bool deflate)
{
    tx_toc_method = deflate ? EPUB_DEFLATED : EPUB_STORED;
    tx_build(chapters, 2, ncx ? NULL : (const char *)doc_bytes, ncx ? (const char *)doc_bytes : NULL, deflate);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static char doc[INPUT_MAX + 1u];
    if (size < 1 || size - 1u > INPUT_MAX) {
        return 0;
    }
    bool ncx = (data[0] & 1u) != 0;
    bool deflate = (data[0] & 2u) != 0;
    memcpy(doc, data + 1, size - 1u);
    doc[size - 1u] = '\0';
    /* a NUL ends the entry early; bound by what was stored */
    uint32_t len = (uint32_t)strlen(doc);
    build((const uint8_t *)doc, ncx, deflate);

    entry_n = 0;
    bartap_err_t first = read_checked(4096, &dumps[0], len, true);
    static const uint32_t caps[] = {BARTAP_TEXT_MIN, 7};
    for (uint32_t c = 0; c < sizeof caps / sizeof caps[0]; c++) {
        FUZZ_CHECK(read_checked(caps[c], &dumps[1], len, false) == first);
        FUZZ_CHECK(strcmp(dumps[0].s, dumps[1].s) == 0);
    }

    /* every entry resolves, then seeks and reads to the end (R13) */
    for (uint32_t k = 0; k < entry_n; k++) {
        const entry_t *e = &entries[k];
        const char *chapter = chapters[e->spine];
        bartap_bookmark_t bm;
        FUZZ_CHECK(bartap_text_open_memory(&tx_text, &tx_book, e->spine, (const uint8_t *)chapter,
                                           (uint32_t)strlen(chapter), tx_buf, 64, NULL, NULL, NULL) == BARTAP_OK);
        FUZZ_CHECK(bartap_toc_resolve(&tx_text, e->fragment, e->len, &bm) == BARTAP_OK);
        FUZZ_CHECK(bm.spine_index == e->spine);
        FUZZ_CHECK(bartap_text_seek(&tx_text, &bm) == BARTAP_OK);
        bartap_record_t r;
        bartap_err_t err;
        uint32_t guard = 0;
        while ((err = bartap_text_next(&tx_text, &r)) == BARTAP_OK) {
            FUZZ_CHECK(++guard <= 10000u);
        }
        FUZZ_CHECK(err == BARTAP_END);
    }

    /* the same document stored or deflated reads the same (the NCX re-read takes either path) */
    build((const uint8_t *)doc, ncx, !deflate);
    FUZZ_CHECK(read_checked(4096, &dumps[1], len, false) == first);
    FUZZ_CHECK(strcmp(dumps[0].s, dumps[1].s) == 0);
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

static const char *const nav_fragments[] = {
    "<nav epub:type=\"toc\">", "<nav epub:type=\"landmarks\">", "<nav>", "</nav>", "<ol>", "</ol>", "<li>",
    "</li>", "<a href=\"text/c0.xhtml#a\">", "<a href=\"text/c0.xhtml#b\">", "<a href=\"text/c1.xhtml#y\">",
    "<a href=\"text/c1.xhtml\">", "<a href=\"text/c0.xhtml#missing\">", "<a href=\"text/c0.xhtml#%62\">",
    "<a href=\"text/c9.xhtml\">", "<a href=\"http://example.com/\">", "<a href=\"#a\">", "<a>", "</a>",
    "<span>", "</span>", "<i>", "</i>", "<b>", "</b>", "<code>", "</code>", "<h1>", "</h1>", "<p>", "</p>",
};

static const char *const ncx_fragments[] = {
    "<navMap>", "</navMap>", "<navPoint>", "<navPoint id=\"x\">", "</navPoint>", "<navPoint/>",
    "<navLabel><text>", "</text></navLabel>", "<navLabel>", "</navLabel>", "<text>", "</text>",
    "<content src=\"text/c0.xhtml#a\"/>", "<content src=\"text/c1.xhtml#x\"/>", "<content src=\"text/c0.xhtml\"/>",
    "<content src=\"text/c0.xhtml#nope\"/>", "<content src=\"zzz.xhtml\"/>", "<content/>", "<content src=\"\">",
    "<pageList>", "</pageList>",
};

static const char *const shared_fragments[] = {
    "label", "two words", "  ", "\n", "&amp;", "&#233;", "&#x1F600;", "\xf0\x9f\x98\x80", "&bogus;", "\x01",
    "caf\xc3\xa9", "\xff",
    "<!-- c -->", "<![CDATA[x]]>", "<", "\"", "</", ">",
};

#define COUNT(a) (sizeof a / sizeof a[0])

static size_t put_str(uint8_t *out, size_t n, size_t cap, const char *s)
{
    size_t len = strlen(s);
    if (n + len >= cap) {
        return n;
    }
    memcpy(out + n, s, len);
    return n + len;
}

size_t fuzz_generate(uint32_t seed, uint8_t *out, size_t cap)
{
    uint32_t state = seed * 2654435761u + 7u;
    uint8_t flags = (uint8_t)(gen_random(&state) & 3u);
    bool ncx = (flags & 1u) != 0;
    size_t n = 0;
    out[n++] = flags;
    if (gen_random(&state) % 4u != 0) {
        n = put_str(out, n, cap, ncx ? "<?xml version=\"1.0\"?><ncx><navMap>"
                                     : "<?xml version=\"1.0\"?><html xmlns:epub=\"http://www.idpf.org/2007/ops\">"
                                       "<body><nav epub:type=\"toc\"><ol>");
    }
    uint32_t count = gen_random(&state) % 200u;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t pick = gen_random(&state) % 3u;
        const char *f = pick == 0 ? shared_fragments[gen_random(&state) % COUNT(shared_fragments)]
                        : ncx     ? ncx_fragments[gen_random(&state) % COUNT(ncx_fragments)]
                                  : nav_fragments[gen_random(&state) % COUNT(nav_fragments)];
        n = put_str(out, n, cap, f);
    }
    /* sometimes a label longer than the window, for the NCX's rewind (R26) */
    if (gen_random(&state) % 16u == 0) {
        n = put_str(out, n, cap, ncx ? "<navPoint><navLabel><text>" : "<li><a href=\"text/c0.xhtml#b\">");
        for (uint32_t i = 0; i < 3000u; i++) {
            n = put_str(out, n, cap, "long label words ");
        }
        n = put_str(out, n, cap, ncx ? "</text></navLabel><content src=\"text/c0.xhtml#b\"/></navPoint>" : "</a></li>");
    }
    return n;
}
