/* Phase 1: the canonical serialization, against literal expected bytes. */
#include <string.h>

#include "canon.h"
#include "check.h"

typedef struct {
    uint8_t bytes[128];
    size_t len;
} capture_t;

static void capture(void *user, const uint8_t *data, size_t len)
{
    capture_t *c = user;
    if (len <= sizeof c->bytes - c->len) {
        memcpy(c->bytes + c->len, data, len);
        c->len += len;
    }
}

/* Junk in every byte first, so padding or unused union bytes in the output fail the comparison. */
static bartap_record_t junk_record(bartap_record_kind_t kind)
{
    bartap_record_t rec;
    memset(&rec, 0xAA, sizeof rec);
    rec.kind = kind;
    return rec;
}

static bool canon_matches(const bartap_record_t *rec, const uint8_t *expected, size_t len)
{
    capture_t c = {{0}, 0};
    return canon_record(rec, capture, &c) && c.len == len && memcmp(c.bytes, expected, len) == 0;
}

#define EXPECT(rec, ...)                                                       \
    do {                                                                       \
        static const uint8_t expected[] = {__VA_ARGS__};                       \
        CHECK(canon_matches(&(rec), expected, sizeof expected));               \
    } while (0)

int main(void)
{
    bartap_record_t r;

    r = junk_record(BARTAP_REC_BLOCK_START);
    r.block.kind = BARTAP_BLOCK_HEADING;
    r.block.level = 3;
    r.block.spine_index = 0x0102;
    r.block.block_offset = 0x03040506u;
    r.block.char_offset = 0x0708090Au;
    EXPECT(r, 0x01, 0x02, 0x03, 0x02, 0x01, 0x06, 0x05, 0x04, 0x03, 0x0A, 0x09, 0x08, 0x07);

    r = junk_record(BARTAP_REC_CONTAINER_START);
    r.container.kind = BARTAP_CONTAINER_LI;
    r.container.ordinal = -3; /* HTML allows start="-3" (R45) */
    EXPECT(r, 0x02, 0x04, 0xFD, 0xFF, 0xFF, 0xFF);

    r = junk_record(BARTAP_REC_CONTAINER_END);
    r.container.kind = BARTAP_CONTAINER_NOTE;
    r.container.ordinal = 0;
    EXPECT(r, 0x03, 0x07, 0x00, 0x00, 0x00, 0x00);

    r = junk_record(BARTAP_REC_TEXT);
    r.text.ptr = "hi";
    r.text.len = 2;
    EXPECT(r, 0x04, 0x02, 0x00, 0x00, 0x00, 'h', 'i');

    r = junk_record(BARTAP_REC_TEXT);
    r.text.ptr = NULL;
    r.text.len = 0;
    EXPECT(r, 0x04, 0x00, 0x00, 0x00, 0x00);

    r = junk_record(BARTAP_REC_STYLE);
    r.style.bits = BARTAP_STYLE_ITALIC | BARTAP_STYLE_STRIKE;
    EXPECT(r, 0x05, 0x41, 0x00, 0x00, 0x00);

    r = junk_record(BARTAP_REC_IMAGE);
    r.image.dir_offset = 0xDEADBEEFu;
    EXPECT(r, 0x06, 0xEF, 0xBE, 0xAD, 0xDE);

    r = junk_record(BARTAP_REC_LINE_BREAK);
    EXPECT(r, 0x07);

    r = junk_record(BARTAP_REC_TOC_ENTRY);
    r.toc.depth = 2;
    r.toc.spine_index = BARTAP_NO_SPINE;
    r.toc.fragment = "id";
    r.toc.fragment_len = 2;
    EXPECT(r, 0x08, 0x02, 0x00, 0xFF, 0xFF, 0x02, 0x00, 0x00, 0x00, 'i', 'd');

    r = junk_record(BARTAP_REC_TOC_ENTRY);
    r.toc.depth = 0;
    r.toc.spine_index = 1;
    r.toc.fragment = NULL;
    r.toc.fragment_len = 0;
    EXPECT(r, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00);

    /* Refusals write nothing. */
    capture_t c = {{0}, 0};
    r = junk_record((bartap_record_kind_t)99);
    CHECK(!canon_record(&r, capture, &c));
    r = junk_record(BARTAP_REC_TEXT);
    r.text.ptr = NULL;
    r.text.len = 1;
    CHECK(!canon_record(&r, capture, &c));
    CHECK(c.len == 0);

    /* The merged stream: a text run is 04, its bytes, 00, however it was split (R50). */
    bartap_record_t style = junk_record(BARTAP_REC_STYLE);
    style.style.bits = 1;
    bartap_record_t brk = junk_record(BARTAP_REC_LINE_BREAK);
    static const char *const splits[][3] = {{"abc", NULL, NULL}, {"a", "bc", NULL}, {"a", "b", "c"}};
    static const uint8_t merged[] = {0x05, 0x01, 0x00, 0x00, 0x00, 0x04, 'a', 'b', 'c', 0x00, 0x07,
                                     0x04, 'd', 0x00};
    for (size_t k = 0; k < sizeof splits / sizeof splits[0]; k++) {
        capture_t m = {{0}, 0};
        canon_stream_t s = {capture, &m, false};
        CHECK(canon_stream_record(&s, &style));
        for (size_t i = 0; i < 3 && splits[k][i] != NULL; i++) {
            r = junk_record(BARTAP_REC_TEXT);
            r.text.ptr = splits[k][i];
            r.text.len = (uint32_t)strlen(splits[k][i]);
            CHECK(canon_stream_record(&s, &r));
        }
        CHECK(canon_stream_record(&s, &brk));
        r = junk_record(BARTAP_REC_TEXT);
        r.text.ptr = "d";
        r.text.len = 1;
        CHECK(canon_stream_record(&s, &r));
        /* the end closes a run still open, once */
        canon_stream_end(&s);
        canon_stream_end(&s);
        CHECK(m.len == sizeof merged && memcmp(m.bytes, merged, sizeof merged) == 0);
    }
    /* a bad text record refuses and writes nothing */
    capture_t m = {{0}, 0};
    canon_stream_t s = {capture, &m, false};
    r = junk_record(BARTAP_REC_TEXT);
    r.text.ptr = NULL;
    r.text.len = 1;
    CHECK(!canon_stream_record(&s, &r));
    canon_stream_end(&s);
    CHECK(m.len == 0);

    /* FNV-1a 64 against its published values */
    uint64_t h = CANON_HASH_INIT;
    CHECK(h == 0xCBF29CE484222325ull);
    canon_hash_sink(&h, (const uint8_t *)"a", 1);
    CHECK(h == 0xAF63DC4C8601EC8Cull);
    h = CANON_HASH_INIT;
    canon_hash_sink(&h, (const uint8_t *)"foo", 3);
    CHECK(h == 0xDCB27518FED9D577ull);

    return CHECK_RESULT();
}
