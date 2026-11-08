/* Bookmark fuzz target: seeks with arbitrary bookmark bytes (R12, R29, R48). Built for libFuzzer
 * with BARTAP_FUZZ, and always as a standalone replay (fuzz_main.c) over generated inputs.
 *
 * Input: 23 bookmark bytes, a flags byte, then chapter 0 of the book. The flags can repair what a
 * fuzzer rarely gets right by chance (the CRC, the book hash, the chapter CRC, the version, a block
 * offset that is a tag), so wild fields reach the seek itself. For any input:
 *   - reading the bytes never reads out of bounds, and gives OK, CORRUPT or WRONG_BOOK as R12 says;
 *   - a bookmark that reads seeks in every mode, and every mode gives the same records (P1), which
 *     are well formed and end;
 *   - the same fields given straight to bartap_text_seek, past every check of the bytes, are refused
 *     for another item and otherwise seek and end. */
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

#define INPUT_MAX (1u << 20)

enum {
    FIX_CRC = 1u << 0,         /* recompute the bookmark CRC-32 last */
    FIX_BOOK = 1u << 1,        /* this book's hash */
    FIX_CHAPTER = 1u << 2,     /* item 0, and its chapter CRC-32 */
    FIX_VERSION = 1u << 3,     /* this library's version */
    FIX_BLOCK = 1u << 4,       /* the block offset becomes the offset of a '<', picked by its value */
    WARM_SOME = 1u << 5,       /* read some records before seeking */
    WARM_ALL = 1u << 6,        /* read to the end before seeking */
};

static uint8_t bytes[BARTAP_BOOKMARK_SIZE];

/* Applies the flags to the bytes, in the order the bookmark is checked. */
static void repair(uint8_t flags, const char *chapter, uint32_t len)
{
    if (flags & FIX_VERSION) {
        bytes[0] = (uint8_t)BARTAP_BOOKMARK_VERSION;
    }
    if (flags & FIX_BOOK) {
        bt_put_le32(bytes + 1, tx_book.book_hash);
    }
    if (flags & FIX_CHAPTER) {
        bt_put_le16(bytes + 5, 0);
        bt_put_le32(bytes + 15, tx_spine[0].crc32);
    }
    if ((flags & FIX_BLOCK) && len != 0) {
        uint32_t tags = 0;
        for (uint32_t i = 0; i < len; i++) {
            tags += chapter[i] == '<';
        }
        if (tags != 0) {
            uint32_t want = bt_get_le32(bytes + 7) % tags;
            for (uint32_t i = 0; i < len; i++) {
                if (chapter[i] == '<' && want-- == 0) {
                    bt_put_le32(bytes + 7, i);
                    break;
                }
            }
        }
    }
    if (flags & FIX_CRC) {
        bt_put_le32(bytes + 19, bt_crc32(0, bytes, 19));
    }
}

/* After a seek's records: containers balance from the restated context, and nothing is malformed. */
static void check_records(const seek_out_t *o, uint32_t len)
{
    int32_t depth = 0;
    for (uint32_t i = 0; i < o->raw_n; i++) {
        const bartap_record_t *r = &o->raw[i];
        switch (r->kind) {
        case BARTAP_REC_CONTAINER_START:
            FUZZ_CHECK(++depth <= (int32_t)BARTAP_DEPTH_MAX);
            break;
        case BARTAP_REC_CONTAINER_END:
            FUZZ_CHECK(--depth >= 0);
            break;
        case BARTAP_REC_BLOCK_START:
            FUZZ_CHECK(r->block.block_offset < len && r->block.spine_index == 0);
            break;
        case BARTAP_REC_TEXT:
            FUZZ_CHECK(r->text.len >= 1);
            break;
        case BARTAP_REC_STYLE:
            FUZZ_CHECK((r->style.bits & ~0x7Fu) == 0);
            break;
        case BARTAP_REC_IMAGE:
        case BARTAP_REC_LINE_BREAK:
            break;
        default:
            FUZZ_CHECK(0);
        }
    }
    FUZZ_CHECK(depth == 0);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static char chapter[INPUT_MAX + 1u];
    if (size < BARTAP_BOOKMARK_SIZE + 1u || size - BARTAP_BOOKMARK_SIZE - 1u > INPUT_MAX) {
        return 0;
    }
    memcpy(bytes, data, BARTAP_BOOKMARK_SIZE);
    uint8_t flags = data[BARTAP_BOOKMARK_SIZE];
    size_t chapter_size = size - BARTAP_BOOKMARK_SIZE - 1u;
    memcpy(chapter, data + BARTAP_BOOKMARK_SIZE + 1u, chapter_size);
    chapter[chapter_size] = '\0';
    uint32_t len = (uint32_t)strlen(chapter);
    const char *chapters[1] = {chapter};
    tx_build(chapters, 1, NULL, NULL, (flags & 0x80u) != 0);
    sp_spine = 0;
    sp_data = (const uint8_t *)chapter;
    sp_size = len;

    /* an item that fails as a whole, such as UTF-16, has nothing to seek in */
    bartap_record_t r;
    bartap_err_t err;
    FUZZ_CHECK(bartap_text_open_memory(&tx_text, &tx_book, 0, sp_data, sp_size, tx_buf, 64, NULL, NULL, NULL) ==
               BARTAP_OK);
    while ((err = bartap_text_next(&tx_text, &r)) == BARTAP_OK) {
    }
    if (err != BARTAP_END) {
        return 0;
    }

    repair(flags, chapter, len);
    bartap_bookmark_t bm;
    err = bartap_bookmark_read(&tx_book, bytes, &bm);
    /* R12's checks 0 and 1, and the spine range: nothing else comes back from the bytes */
    bool crc_ok = bt_get_le32(bytes + 19) == bt_crc32(0, bytes, 19);
    if (!crc_ok) {
        FUZZ_CHECK(err == BARTAP_ERR_CORRUPT);
    } else if (bt_get_le32(bytes + 1) != tx_book.book_hash) {
        FUZZ_CHECK(err == BARTAP_ERR_WRONG_BOOK);
    } else if (bt_get_le16(bytes + 5) != 0) {
        FUZZ_CHECK(err == BARTAP_ERR_CORRUPT);
    } else {
        FUZZ_CHECK(err == BARTAP_OK && bm.spine_index == 0);
    }

    /* past the bytes' checks: the fields as they are, straight to the seek */
    if (err != BARTAP_OK) {
        bm.version = bytes[0];
        bm.book_hash = bt_get_le32(bytes + 1);
        bm.spine_index = bt_get_le16(bytes + 5);
        bm.block_offset = bt_get_le32(bytes + 7);
        bm.char_offset = bt_get_le32(bytes + 11);
        bm.chapter_crc32 = bt_get_le32(bytes + 15);
    }
    if (bm.spine_index != 0) {
        open_mode(MODE_MEMORY, 64);
        FUZZ_CHECK(bartap_text_seek(&tx_text, &bm) == BARTAP_ERR_ARG);
        return 0;
    }

    /* every mode lands the same way (P1) */
    int warm = (flags & WARM_ALL) ? 1 << 30 : (flags & WARM_SOME) ? (int)(len % 40u) : 0;
    ring_size = 64u << (len % 4u);
    uint32_t cap = (flags & 0x80u) ? BARTAP_TEXT_MIN : 64u;
    for (int mode = 0; mode < MODE_COUNT; mode++) {
        seek_read(mode, cap, warm, &bm, &outs[mode]);
        if (mode == 0) {
            check_records(&outs[0], len);
        } else {
            FUZZ_CHECK(same_raw(&outs[0], &outs[mode]));
        }
    }
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

static const char *const fragments[] = {
    "<p>", "</p>", "<h2>", "</h2>", "<pre>", "</pre>", "<hr/>", "<div>", "</div>", "<blockquote>",
    "</blockquote>", "<ol>", "</ol>", "<ul>", "</ul>", "<li>", "</li>", "<dl>", "<dt>", "<dd>", "</dl>",
    "<aside epub:type=\"footnote\">", "</aside>", "<i>", "</i>", "<b>", "</b>", "<br/>", "<p/>",
    "<img src=\"../images/a.png\"/>", "word", "two words here", "  ", "\n", "&amp;", "&#233;",
    "<![CDATA[ x<y ]]>", "<!-- c -->", "caf\xc3\xa9", "\xff", "<", "</span>",
};

size_t fuzz_generate(uint32_t seed, uint8_t *out, size_t cap)
{
    uint32_t state = seed * 2654435761u + 11u;
    size_t n = 0;
    for (uint32_t i = 0; i < BARTAP_BOOKMARK_SIZE; i++) {
        out[n++] = (uint8_t)gen_random(&state);
    }
    /* mostly repaired, so seeks run; sometimes raw, so the bytes' checks do */
    uint8_t flags = (uint8_t)gen_random(&state);
    if (gen_random(&state) % 8u != 0) {
        flags |= FIX_CRC | FIX_BOOK;
    }
    if (gen_random(&state) % 4u != 0) {
        flags |= FIX_CHAPTER;
    }
    out[n++] = flags;
    /* small offsets, so they land inside the chapter more often than not */
    uint32_t block = gen_random(&state) % 3000u;
    uint32_t chr = gen_random(&state) % 4u == 0 ? gen_random(&state) : gen_random(&state) % 40u;
    for (uint32_t k = 0; k < 4; k++) {
        out[7 + k] = (uint8_t)(block >> (8 * k));
        out[11 + k] = (uint8_t)(chr >> (8 * k));
    }
    uint32_t count = gen_random(&state) % 400u;
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
