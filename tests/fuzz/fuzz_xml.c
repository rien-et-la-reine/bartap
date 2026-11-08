/* Tokenizer fuzz target (R15, R33). Built for libFuzzer with BARTAP_FUZZ, and always as a standalone
 * replay (fuzz_main.c) over generated documents, so the checks run in every ctest.
 *
 * For any input:
 *   - every invariant below holds: offsets only move forward, text is valid UTF-8 with no control
 *     characters (R21), names are folded, attributes stay in their storage (R33), the walk ends;
 *   - text buffer sizes give the same dump (R50);
 *   - an entry stream with a small ring gives the same dump as memory mode (R11, R15);
 *   - seeking to any token's offset gives the same tokens from there as a straight read (R48).
 * Read-ahead sizes are compared across builds instead, by tests/prop/xml_hash.c (R33). */
#include <stdio.h>
#include <stdlib.h>

#include "stream_fixture.h"
#include "xml_dump.h"
#include "xml_gen.h"

#define FUZZ_CHECK(cond)                                                              \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "%s:%d: fuzz check failed: %s\n", __FILE__, __LINE__, #cond); \
            abort();                                                                  \
        }                                                                             \
    } while (0)

/* Independent of the library's decoder: well-formed UTF-8 per Unicode's table, with no C0 controls
 * but tab, LF and CR, no DEL and no C1 controls. */
static void check_text(const uint8_t *p, uint32_t n)
{
    uint32_t i = 0;
    while (i < n) {
        uint8_t b = p[i];
        uint32_t need;
        uint32_t cp;
        if (b < 0x80u) {
            FUZZ_CHECK(b >= 0x20u || b == '\t' || b == '\n' || b == '\r');
            FUZZ_CHECK(b != 0x7Fu);
            i++;
            continue;
        }
        if (b >= 0xC2u && b <= 0xDFu) {
            need = 1;
            cp = b & 0x1Fu;
        } else if (b >= 0xE0u && b <= 0xEFu) {
            need = 2;
            cp = b & 0x0Fu;
        } else {
            FUZZ_CHECK(b >= 0xF0u && b <= 0xF4u);
            need = 3;
            cp = b & 0x07u;
        }
        FUZZ_CHECK(need < n - i); /* the whole sequence is there */
        for (uint32_t k = 1; k <= need; k++) {
            FUZZ_CHECK((p[i + k] & 0xC0u) == 0x80u);
            cp = (cp << 6) | (p[i + k] & 0x3Fu);
        }
        static const uint32_t min_cp[4] = {0, 0x80u, 0x800u, 0x10000u};
        FUZZ_CHECK(cp >= min_cp[need]);
        FUZZ_CHECK(cp <= 0x10FFFFu && (cp < 0xD800u || cp > 0xDFFFu));
        FUZZ_CHECK(cp < 0x80u || cp > 0x9Fu);
        i += need + 1u;
    }
}

/* Walks the tokens checking invariants that don't need a second run. */
static void check_invariants(const uint8_t *data, uint32_t size)
{
    static bartap_xml_t xml;
    static uint8_t text[4096];
    bartap_bytes_t bytes;
    uint32_t last_end = 0;
    uint32_t tokens = 0;

    bt_bytes_init_memory(&bytes, data, size, NULL);
    bt_xml_init(&xml, &bytes, NULL, BARTAP_NO_SPINE);
    for (;;) {
        const bartap_xml_token_t *t;
        bartap_err_t err = bt_xml_next(&xml, &t);
        if (err != BARTAP_OK) {
            /* memory mode can't fail a read; only UTF-16 is refused */
            FUZZ_CHECK(err == BARTAP_END || err == BARTAP_ERR_UNSUPPORTED);
            FUZZ_CHECK(bt_xml_next(&xml, &t) == err); /* sticky */
            return;
        }
        /* every token consumes at least one byte, so the walk is bounded by the input */
        FUZZ_CHECK(++tokens <= size + 1u);
        FUZZ_CHECK(t->offset >= last_end);
        FUZZ_CHECK(t->offset <= size);
        FUZZ_CHECK(t->name_len <= BARTAP_XML_NAME_MAX);
        for (uint32_t i = 0; i < t->name_len; i++) {
            FUZZ_CHECK(!(t->name[i] >= 'A' && t->name[i] <= 'Z'));
        }
        if (t->kind == BARTAP_XML_TEXT) {
            uint32_t got;
            for (;;) {
                FUZZ_CHECK(bt_xml_text(&xml, (char *)text, sizeof text, &got) == BARTAP_OK);
                if (got == 0) {
                    break;
                }
                check_text(text, got);
            }
            FUZZ_CHECK(bt_xml_text(&xml, (char *)text, sizeof text, &got) == BARTAP_OK && got == 0);
        } else {
            uint32_t got;
            FUZZ_CHECK(bt_xml_text(&xml, (char *)text, sizeof text, &got) == BARTAP_ERR_ARG);
            FUZZ_CHECK(xml.attr_count <= BARTAP_XML_ATTRS_MAX);
            FUZZ_CHECK(xml.attr_used <= BARTAP_ATTR_MAX);
            FUZZ_CHECK(t->kind == BARTAP_XML_START_TAG || xml.attr_count == 0);
            for (uint32_t i = 0; i < xml.attr_count; i++) {
                const bartap_xml_attr_t *a = &xml.attrs[i];
                FUZZ_CHECK((uint32_t)a->name_offset + a->name_len <= xml.attr_used);
                FUZZ_CHECK((uint32_t)a->value_offset + a->value_len <= xml.attr_used);
                FUZZ_CHECK(a->name_len > 0);
            }
        }
        FUZZ_CHECK(t->end_offset >= t->offset && t->end_offset <= size);
        FUZZ_CHECK(t->end_offset > t->offset); /* every token consumes at least one byte */
        last_end = t->end_offset;
    }
}

/* The same document through a stored zip entry and a small ring, as text streams read it. */
static void check_stream_mode(const uint8_t *data, uint32_t size, const xml_dump_t *memory)
{
    static uint8_t window[BARTAP_WINDOW_SIZE];
    static uint8_t ring[256];
    static bartap_stream_t stream;
    static bartap_xml_t xml;
    static bartap_rolling_t rolling;
    bartap_bytes_t bytes;
    xml_dump_t d = {0};
    fixture_t f;

    fixture_build(&f, data, size, 0, size, bt_crc32(0, data, size));
    FUZZ_CHECK(bt_stream_open(&stream, &f.source, &f.entry, window) == BARTAP_OK);
    FUZZ_CHECK(bartap_rolling_init(&rolling, ring, sizeof ring) == BARTAP_OK);
    bt_bytes_init_stream(&bytes, &stream, &rolling);
    bartap_warnings_t w = dump_warnings(&d);
    bt_xml_init(&xml, &bytes, &w, 7);
    xml_dump_run(&d, &xml, 64);
    FUZZ_CHECK(d.len == memory->len && memcmp(d.buf, memory->buf, d.len) == 0);
    xml_dump_free(&d);
    fixture_free(&f);
}

/* Seeks to one token's offset, chosen by the input, and compares the rest with the straight read. */
static void check_seek(const uint8_t *data, uint32_t size, const xml_dump_t *straight)
{
    static bartap_xml_t xml;
    bartap_bytes_t bytes;
    xml_dump_t d = {0};

    if (straight->count == 0) {
        return;
    }
    size_t k = (size * 31u + (size != 0 ? data[size / 2u] : 0)) % straight->count;
    bartap_warnings_t w = dump_warnings(&d);
    bt_bytes_init_memory(&bytes, data, size, NULL);
    bt_xml_init(&xml, &bytes, &w, 7);
    /* read a little first, so the seek has state to throw away */
    const bartap_xml_token_t *t;
    (void)bt_xml_next(&xml, &t);
    d.pending_len = 0;
    FUZZ_CHECK(bt_xml_seek(&xml, straight->offsets[k]) == BARTAP_OK);
    xml_dump_run(&d, &xml, 64);
    size_t from = straight->starts[k];
    FUZZ_CHECK(d.len == straight->len - from);
    FUZZ_CHECK(memcmp(d.buf, straight->buf + from, d.len) == 0);
    xml_dump_free(&d);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
size_t fuzz_generate(uint32_t seed, uint8_t *out, size_t cap);

/* For the standalone replay, fuzz_main.c. */
size_t fuzz_generate(uint32_t seed, uint8_t *out, size_t cap)
{
    return xml_gen(seed, out, cap > UINT32_MAX ? UINT32_MAX : (uint32_t)cap);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 1u << 20) {
        return 0;
    }
    uint32_t n = (uint32_t)size;
    xml_dump_t big = {0};
    xml_dump_t small = {0};
    xml_dump_t odd = {0};

    check_invariants(data, n);

    xml_dump_memory(&big, data, n, 4096);
    xml_dump_memory(&small, data, n, BARTAP_TEXT_MIN);
    xml_dump_memory(&odd, data, n, BARTAP_TEXT_MIN + n % 61u);
    FUZZ_CHECK(big.len == small.len && memcmp(big.buf, small.buf, big.len) == 0);
    FUZZ_CHECK(big.len == odd.len && memcmp(big.buf, odd.buf, big.len) == 0);

    check_stream_mode(data, n, &big);
    check_seek(data, n, &big);

    xml_dump_free(&big);
    xml_dump_free(&small);
    xml_dump_free(&odd);
    return 0;
}
