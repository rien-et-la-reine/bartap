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

    return CHECK_RESULT();
}
