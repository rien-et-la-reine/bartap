#include "canon.h"

static void put_u8(uint8_t *out, size_t *n, uint8_t v)
{
    out[(*n)++] = v;
}

static void put_u16(uint8_t *out, size_t *n, uint16_t v)
{
    put_u8(out, n, (uint8_t)(v & 0xFFu));
    put_u8(out, n, (uint8_t)(v >> 8));
}

static void put_u32(uint8_t *out, size_t *n, uint32_t v)
{
    put_u16(out, n, (uint16_t)(v & 0xFFFFu));
    put_u16(out, n, (uint16_t)(v >> 16));
}

bool canon_record(const bartap_record_t *rec, canon_sink_fn sink, void *user)
{
    uint8_t head[16];
    size_t n = 0;
    const char *bytes = NULL;
    uint32_t bytes_len = 0;

    switch (rec->kind) {
    case BARTAP_REC_BLOCK_START:
        put_u8(head, &n, 0x01);
        put_u8(head, &n, (uint8_t)rec->block.kind);
        put_u8(head, &n, rec->block.level);
        put_u16(head, &n, rec->block.spine_index);
        put_u32(head, &n, rec->block.block_offset);
        put_u32(head, &n, rec->block.char_offset);
        break;
    case BARTAP_REC_CONTAINER_START:
    case BARTAP_REC_CONTAINER_END:
        put_u8(head, &n, rec->kind == BARTAP_REC_CONTAINER_START ? 0x02 : 0x03);
        put_u8(head, &n, (uint8_t)rec->container.kind);
        put_u32(head, &n, (uint32_t)rec->container.ordinal);
        break;
    case BARTAP_REC_TEXT:
        put_u8(head, &n, 0x04);
        put_u32(head, &n, rec->text.len);
        bytes = rec->text.ptr;
        bytes_len = rec->text.len;
        break;
    case BARTAP_REC_STYLE:
        put_u8(head, &n, 0x05);
        put_u32(head, &n, rec->style.bits);
        break;
    case BARTAP_REC_IMAGE:
        put_u8(head, &n, 0x06);
        put_u32(head, &n, rec->image.dir_offset);
        break;
    case BARTAP_REC_LINE_BREAK:
        put_u8(head, &n, 0x07);
        break;
    case BARTAP_REC_TOC_ENTRY:
        put_u8(head, &n, 0x08);
        put_u16(head, &n, rec->toc.depth);
        put_u16(head, &n, rec->toc.spine_index);
        put_u32(head, &n, rec->toc.fragment_len);
        bytes = rec->toc.fragment;
        bytes_len = rec->toc.fragment_len;
        break;
    default:
        return false;
    }

    if (bytes == NULL && bytes_len != 0) {
        return false;
    }
    sink(user, head, n);
    if (bytes_len != 0) {
        sink(user, (const uint8_t *)bytes, bytes_len);
    }
    return true;
}

bool canon_stream_record(canon_stream_t *s, const bartap_record_t *rec)
{
    static const uint8_t text_start = 0x04;
    if (rec->kind == BARTAP_REC_TEXT) {
        if (rec->text.ptr == NULL && rec->text.len != 0) {
            return false;
        }
        if (!s->in_text) {
            /* a run starts; its length is never written, its end is */
            s->sink(s->user, &text_start, 1);
            s->in_text = true;
        }
        if (rec->text.len != 0) {
            s->sink(s->user, (const uint8_t *)rec->text.ptr, rec->text.len);
        }
        return true;
    }
    canon_stream_end(s);
    return canon_record(rec, s->sink, s->user);
}

void canon_stream_end(canon_stream_t *s)
{
    static const uint8_t text_end = 0x00;
    if (s->in_text) {
        s->sink(s->user, &text_end, 1);
        s->in_text = false;
    }
}

void canon_hash_sink(void *user, const uint8_t *data, size_t len)
{
    uint64_t *h = user;
    uint64_t v = *h;
    for (size_t i = 0; i < len; i++) {
        v = (v ^ data[i]) * 0x100000001B3ull;
    }
    *h = v;
}
