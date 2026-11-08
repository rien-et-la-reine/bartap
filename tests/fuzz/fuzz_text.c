/* Text layer fuzz target (R4, R6, R29, R45-R50). Built for libFuzzer with BARTAP_FUZZ, and always as a
 * standalone replay (fuzz_main.c) over generated chapters full of broken markup.
 *
 * The input is chapter 0 of a built book. For any input:
 *   - reading never reads or writes out of bounds, and ends within a bound of the input's size (R29);
 *   - records are well formed: containers balance, text records hold whole UTF-8 characters within
 *     the buffer, block offsets are in the chapter and never go back, styles hold known bits, images
 *     are real entries;
 *   - the minimum buffer and stream mode give the same content as memory mode (R6, R50);
 *   - seeks to a spread of landing points satisfy P1 and P2 (R48), and an id resolves to a landing
 *     that seeks cleanly (R13). */
#include <stdlib.h>

#define FUZZ_CHECK(cond)                                                                   \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            fprintf(stderr, "%s:%d: fuzz check failed: %s\n", __FILE__, __LINE__, #cond);  \
            abort();                                                                       \
        }                                                                                  \
    } while (0)
#define CHECK(cond) FUZZ_CHECK(cond)

#include "seek_prop.h"

#define SEEKS 6
#define INPUT_MAX (1u << 20)

static tx_dump_t dumps[2];

/* Reads chapter 0 straight and checks every record. Returns false if the chapter fails as a whole,
 * which is allowed: a UTF-16 document is unsupported, for one. */
static bool read_checked(bool stream, uint32_t cap, tx_dump_t *d, uint32_t size)
{
    bartap_err_t err = stream ? bartap_text_open(&tx_text, &tx_book, 0, tx_window, tx_buf, cap, NULL, NULL, NULL)
                              : bartap_text_open_memory(&tx_text, &tx_book, 0, sp_data, sp_size, tx_buf, cap,
                                                        NULL, NULL, NULL);
    FUZZ_CHECK(err == BARTAP_OK);
    tx_dump_begin(d, 0, TX_OFFSETS);

    bartap_record_t r;
    int32_t depth = 0;
    uint32_t last_block = 0;
    uint32_t records = 0;
    while ((err = bartap_text_next(&tx_text, &r)) == BARTAP_OK) {
        /* a bound on the records: every one comes from some byte, a few per tag at most */
        FUZZ_CHECK(++records <= 8u * size + 64u);
        switch (r.kind) {
        case BARTAP_REC_TEXT: {
            const uint8_t *p = (const uint8_t *)r.text.ptr;
            FUZZ_CHECK(r.text.len >= 1 && r.text.len <= cap);
            FUZZ_CHECK(r.text.ptr == tx_buf);
            for (uint32_t i = 0; i < r.text.len;) {
                uint32_t len = p[i] < 0x80u ? 1u : p[i] < 0xC2u ? 0u : p[i] < 0xE0u ? 2u : p[i] < 0xF0u ? 3u
                                                                     : p[i] < 0xF5u ? 4u : 0u;
                FUZZ_CHECK(len != 0 && len <= r.text.len - i);
                for (uint32_t k = 1; k < len; k++) {
                    FUZZ_CHECK((p[i + k] & 0xC0u) == 0x80u);
                }
                /* control characters never reach text (R21) */
                FUZZ_CHECK(!(len == 1 && (p[i] < 0x20u && p[i] != '\t')) && p[i] != 0x7Fu);
                i += len;
            }
            break;
        }
        case BARTAP_REC_BLOCK_START:
            FUZZ_CHECK(r.block.block_offset < size && r.block.block_offset >= last_block);
            FUZZ_CHECK(r.block.char_offset == 0 && r.block.spine_index == 0);
            FUZZ_CHECK(r.block.kind >= BARTAP_BLOCK_PARAGRAPH && r.block.kind <= BARTAP_BLOCK_DT);
            FUZZ_CHECK((r.block.kind == BARTAP_BLOCK_HEADING) == (r.block.level >= 1 && r.block.level <= 6));
            last_block = r.block.block_offset;
            break;
        case BARTAP_REC_CONTAINER_START:
            FUZZ_CHECK(++depth <= (int32_t)BARTAP_DEPTH_MAX);
            FUZZ_CHECK(r.container.kind >= BARTAP_CONTAINER_BLOCKQUOTE && r.container.kind <= BARTAP_CONTAINER_NOTE);
            FUZZ_CHECK(r.container.kind == BARTAP_CONTAINER_LI || r.container.ordinal == 0);
            break;
        case BARTAP_REC_CONTAINER_END:
            FUZZ_CHECK(--depth >= 0);
            break;
        case BARTAP_REC_STYLE:
            FUZZ_CHECK((r.style.bits & ~0x7Fu) == 0);
            break;
        case BARTAP_REC_IMAGE:
            FUZZ_CHECK(r.image.dir_offset == tx_dir_offset("OEBPS/images/a.png") ||
                       r.image.dir_offset == tx_dir_offset("OEBPS/images/b c.png"));
            break;
        case BARTAP_REC_LINE_BREAK:
            break;
        default:
            FUZZ_CHECK(0);
        }
        tx_dump_record(d, &r);
    }
    tx_dump_end(d);
    if (err != BARTAP_END) {
        /* failed: only as the tokenizer may, and it sticks */
        FUZZ_CHECK(err == BARTAP_ERR_UNSUPPORTED);
        FUZZ_CHECK(bartap_text_next(&tx_text, &r) == err);
        return false;
    }
    /* balanced by construction (R45) */
    FUZZ_CHECK(depth == 0);
    FUZZ_CHECK(bartap_text_next(&tx_text, &r) == BARTAP_END);
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static char chapter[INPUT_MAX + 1u];
    if (size > INPUT_MAX) {
        return 0;
    }
    /* the chapter as an entry of its own, so stream mode reads it too */
    memcpy(chapter, data, size);
    chapter[size] = '\0';
    const char *chapters[1] = {chapter};
    /* a NUL in the input would end the entry early; keep both readings the same length */
    uint32_t len = (uint32_t)strlen(chapter);
    tx_build(chapters, 1, NULL, NULL, (size & 1u) != 0);
    sp_data = (const uint8_t *)chapter;
    sp_size = len;

    if (!read_checked(false, 4096, &dumps[0], len)) {
        return 0;
    }
    /* every reading gives the same content (R6, R50) */
    FUZZ_CHECK(read_checked(false, BARTAP_TEXT_MIN, &dumps[1], len));
    FUZZ_CHECK(strcmp(dumps[0].s, dumps[1].s) == 0);
    FUZZ_CHECK(read_checked(true, 7, &dumps[1], len));
    FUZZ_CHECK(strcmp(dumps[0].s, dumps[1].s) == 0);

    /* seeks: P2 in stream mode with a small ring, P1 against the other modes (R48) */
    straight_read();
    if (point_n == 0) {
        return 0;
    }
    ring_size = 64u << (len % 4u);
    for (int s = 0; s < SEEKS; s++) {
        const point_t *p = &points[(int)((uint32_t)s * 2654435761u % (uint32_t)point_n)];
        bartap_bookmark_t bm = bookmark(p->block, p->chr);
        int warm = s % 3 == 0 ? 0 : s % 3 == 1 ? (int)(len % 50u) : 1000000;
        uint32_t cap = s % 2 == 0 ? BARTAP_TEXT_MIN : 64u;
        for (int mode = 0; mode < MODE_COUNT; mode++) {
            seek_read(mode, cap, warm, &bm, &outs[mode]);
            FUZZ_CHECK(mode == 0 ? p2_holds(p, &outs[0]) : same_raw(&outs[0], &outs[mode]));
        }
    }

    /* an id resolves to something a seek accepts */
    bartap_bookmark_t bm;
    open_mode(MODE_MEMORY, 64);
    FUZZ_CHECK(bartap_toc_resolve(&tx_text, "a", 1, &bm) == BARTAP_OK);
    FUZZ_CHECK(bartap_text_seek(&tx_text, &bm) == BARTAP_OK);
    bartap_record_t r;
    bartap_err_t err;
    uint32_t guard = 0;
    while ((err = bartap_text_next(&tx_text, &r)) == BARTAP_OK) {
        FUZZ_CHECK(++guard <= 8u * len + 64u);
    }
    FUZZ_CHECK(err == BARTAP_END);
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

/* Fragments of markup, well formed or not, put together at random. */
static const char *const fragments[] = {
    "<p>", "</p>", "<h2>", "</h2>", "<h7>", "<pre>", "</pre>", "<hr/>", "<hr>", "</hr>", "<dt>", "</dt>",
    "<div>", "</div>", "<section>", "</section>", "<table><tr><td>", "</td></tr></table>", "<body>",
    "<blockquote>", "</blockquote>", "<ol start=\"3\">", "<ol reversed=\"\" start=\"-2\">", "<ol reversed=\"\">",
    "</ol>", "<ul>", "</ul>", "<li>", "<li value=\"9\">", "</li>", "<dl>", "</dl>", "<dd>", "</dd>",
    "<aside epub:type=\"footnote\">", "<aside>", "</aside>", "<i>", "</i>", "<b>", "</b>", "<code>", "</code>",
    "<sub>", "</sub>", "<u>", "</u>", "<del>", "</del>", "<span id=\"a\">", "</span>", "<a id=\"a\" href=\"#\">",
    "</a>", "<head>", "</head>", "<title>", "</title>", "<script>", "</script>", "<style>p{}</style>",
    "<img src=\"../images/a.png\"/>", "<img src=\"../images/b%20c.png\">", "</img>", "<img src=\"nope.png\"/>",
    "<svg><image href=\"../images/a.png\"/></svg>", "<br/>", "<br>", "</br>", "<p/>", "<blockquote/>",
    "word", "two words", "  ", "\n", "\r\n", "\t", "&amp;", "&#233;", "&#x1F600;", "&nbsp;", "&bogus;", "&#1;",
    "<![CDATA[ x<y ]]>", "<!-- c -->", "<?pi x?>", "caf\xc3\xa9", "\xe2\x82\xac", "\xff\xfe", "\x01",
    "<html:p>", "</html:p>", "<P>", "<x:unknown>", "</x:unknown>", "<", ">", "</", "&", "\"", "<p id=\"a\">",
};

size_t fuzz_generate(uint32_t seed, uint8_t *out, size_t cap)
{
    uint32_t state = seed * 2654435761u + 13u;
    size_t n = 0;
    uint32_t count = gen_random(&state) % 300u;
    if (gen_random(&state) % 4u == 0) {
        static const char head[] = "<?xml version=\"1.0\"?><html><head><title>t</title></head><body>";
        memcpy(out, head, sizeof head - 1u);
        n = sizeof head - 1u;
    }
    /* deep nesting sometimes, for the depth limit */
    if (gen_random(&state) % 8u == 0) {
        uint32_t deep = 20u + gen_random(&state) % 30u;
        for (uint32_t i = 0; i < deep && n + 16u < cap; i++) {
            const char *tag = gen_random(&state) % 2u ? "<blockquote>" : "<i>";
            memcpy(out + n, tag, strlen(tag));
            n += strlen(tag);
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        const char *f = fragments[gen_random(&state) % (sizeof fragments / sizeof fragments[0])];
        size_t len = strlen(f);
        if (n + len >= cap) {
            break;
        }
        memcpy(out + n, f, len);
        n += len;
    }
    return n;
}
