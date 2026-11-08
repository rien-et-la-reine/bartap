/* Layer 2 (R3): rolling buffer, byte source and XML. Depends only on zip.c. The tokenizer is a
 * stub. */
#include <string.h>

#include "internal.h"

/* ---- Rolling buffer (R48) ---- */

// Byte offset o lives at ring[o & (size - 1)], so the ring holds the last size bytes delivered. Down here
// rather than with the checkpoints, so the byte source can feed it without reaching up a layer.

// Sets up a rolling buffer, empty and with no checkpoints. A NULL ring with a non-zero size is
// memory mode's virtual window: offsets are counted, no bytes are stored.
bartap_err_t bartap_rolling_init(bartap_rolling_t *rolling, uint8_t *ring, uint32_t size)
{
    if (size == 0 || (size & (size - 1u)) != 0) {
        // no window at all, or not a power of two. a mask keeps division off small cores
        return BARTAP_ERR_ARG;
    }
    rolling->ring = ring;
    rolling->size = size;
    rolling->delivered = 0;
    // the text layer owns these, but a fresh ring can't have any yet
    rolling->checkpoints[0].valid = 0;
    rolling->checkpoints[1].valid = 0;
    rolling->next_checkpoint = 0;
    return BARTAP_OK;
}

// Adds freshly delivered bytes. Only the last size bytes of a long append can survive, so only
// those are copied.
void bt_ring_append(bartap_rolling_t *rolling, const uint8_t *data, uint32_t len)
{
    uint32_t end = rolling->delivered + len;

    if (rolling->ring != NULL) {
        // skip what would be overwritten before the append finishes
        if (len > rolling->size) {
            data += len - rolling->size;
            len = rolling->size;
        }
        // copy up to the wrap, then the rest from the ring's start
        uint32_t pos = (end - len) & (rolling->size - 1u);
        uint32_t first = rolling->size - pos;
        if (first > len) {
            first = len;
        }
        memcpy(rolling->ring + pos, data, first);
        if (len > first) {
            memcpy(rolling->ring, data + first, len - first);
        }
    }
    // virtual window or real, the count moves the same
    rolling->delivered = end;
}

// Copies bytes back out by their offset in the source. len 0 just asks if offset is still held.
bartap_err_t bt_ring_read(const bartap_rolling_t *rolling, uint32_t offset, uint8_t *buf,
                          uint32_t len)
{
    if (offset > rolling->delivered || len > rolling->delivered - offset) {
        // asks for bytes not delivered yet, the caller's mistake
        return BARTAP_ERR_ARG;
    }
    if (rolling->ring == NULL || rolling->delivered - offset > rolling->size) {
        // virtual window stores nothing, or the bytes have been overwritten
        return BARTAP_END;
    }
    if (len == 0) {
        // buf may be NULL here, memcpy can't take that
        return BARTAP_OK;
    }
    // copy up to the wrap, then the rest from the ring's start
    uint32_t pos = offset & (rolling->size - 1u);
    uint32_t first = rolling->size - pos;
    if (first > len) {
        first = len;
    }
    memcpy(buf, rolling->ring + pos, first);
    memcpy(buf + first, rolling->ring, len - first);
    return BARTAP_OK;
}

/* ---- Byte source (R15) ---- */

// position is the next source offset to deliver. In stream mode it can sit behind the stream (a
// replay from the ring or the inflate window) or ahead of it (bytes still to skip after a seek); the
// stream itself only moves forward, and only bytes it delivers fresh go into the ring (R48).

// Forgets what the ring holds: it indexes one pass over one source.
static void bytes_reset_ring(bartap_bytes_t *bytes)
{
    if (bytes->rolling != NULL) {
        bytes->rolling->delivered = 0;
        // checkpoints point into what was just forgotten
        bytes->rolling->checkpoints[0].valid = 0;
        bytes->rolling->checkpoints[1].valid = 0;
        bytes->rolling->next_checkpoint = 0;
    }
}

// Reads an entry stream fresh from bt_stream_open, so the ring and the stream start together.
void bt_bytes_init_stream(bartap_bytes_t *bytes, bartap_stream_t *stream, bartap_rolling_t *rolling)
{
    bytes->stream = stream;
    bytes->memory = NULL;
    bytes->memory_size = 0;
    bytes->rolling = rolling;
    bytes->position = 0;
    bytes_reset_ring(bytes);
}

// Reads an already-inflated chapter from RAM (R11). The ring, if any, only counts offsets here.
void bt_bytes_init_memory(bartap_bytes_t *bytes, const uint8_t *data, uint32_t size,
                          bartap_rolling_t *rolling)
{
    bytes->stream = NULL;
    bytes->memory = data;
    bytes->memory_size = size;
    bytes->rolling = rolling;
    bytes->position = 0;
    bytes_reset_ring(bytes);
}

// Memory mode: a copy, and the ring's count catches up with anything new.
static bartap_err_t bytes_read_memory(bartap_bytes_t *bytes, uint8_t *buf, uint32_t cap,
                                      uint32_t *got)
{
    if (bytes->position >= bytes->memory_size) {
        // nothing left, done
        return BARTAP_END;
    }
    uint32_t len = bytes->memory_size - bytes->position;
    if (len > cap) {
        len = cap;
    }
    memcpy(buf, bytes->memory + bytes->position, len);
    bytes->position += len;
    *got = len;

    // feed the ring only bytes past what it has seen, so a re-read after a seek isn't counted twice
    bartap_rolling_t *rolling = bytes->rolling;
    if (rolling != NULL && bytes->position > rolling->delivered) {
        bt_ring_append(rolling, bytes->memory + rolling->delivered,
                       bytes->position - rolling->delivered);
    }
    return BARTAP_OK;
}

// Stream mode, behind the stream: the bytes come back from the ring or the inflate window, never the
// card, and aren't folded into the CRC or appended again (R48).
static bartap_err_t bytes_replay(bartap_bytes_t *bytes, uint8_t *buf, uint32_t cap, uint32_t *got)
{
    uint32_t len = bytes->stream->delivered - bytes->position;
    if (len > cap) {
        len = cap;
    }
    // ring first, it usually reaches further back than the window
    bartap_err_t err = BARTAP_END;
    if (bytes->rolling != NULL) {
        err = bt_ring_read(bytes->rolling, bytes->position, buf, len);
    }
    if (err == BARTAP_END) {
        // not in the ring, try the inflate window
        err = bt_stream_reread(bytes->stream, bytes->position, buf, len);
    }
    if (err != BARTAP_OK) {
        // gone from both, which a seek should have ruled out. pass it up
        return err;
    }
    bytes->position += len;
    *got = len;
    return BARTAP_OK;
}

// Stream mode, level with or ahead of the stream: fresh bytes, which go into the ring. Bytes before
// position are skipped, using buf as scratch.
static bartap_err_t bytes_read_live(bartap_bytes_t *bytes, uint8_t *buf, uint32_t cap, uint32_t *got)
{
    bartap_stream_t *stream = bytes->stream;

    for (;;) {
        // ahead of the stream, only take what's needed to catch up
        uint32_t want = cap;
        if (bytes->position > stream->delivered && bytes->position - stream->delivered < want) {
            want = bytes->position - stream->delivered;
        }
        uint32_t len;
        uint32_t start = stream->delivered;
        bartap_err_t err = bt_stream_read(stream, buf, want, &len);
        if (err != BARTAP_OK) {
            // the end or a failure, pass it up. an end while skipping means the seek was past it
            return err;
        }
        if (bytes->rolling != NULL) {
            bt_ring_append(bytes->rolling, buf, len);
        }
        if (start >= bytes->position) {
            // caught up, these are the bytes wanted
            bytes->position += len;
            *got = len;
            return BARTAP_OK;
        }
        // still skipping, read again. want kept this from overshooting position
    }
}

// Delivers the next bytes from position on; BARTAP_END when the source is finished.
bartap_err_t bt_bytes_read(bartap_bytes_t *bytes, uint8_t *buf, uint32_t cap, uint32_t *got)
{
    *got = 0;
    if (cap == 0) {
        return BARTAP_ERR_ARG;
    }
    if (bytes->stream == NULL) {
        return bytes_read_memory(bytes, buf, cap, got);
    }
    if (bytes->position < bytes->stream->delivered) {
        return bytes_replay(bytes, buf, cap, got);
    }
    return bytes_read_live(bytes, buf, cap, got);
}

// Moves to a source offset. Behind the stream, the bytes must still be in the ring or the inflate
// window, or the stream rewinds and the ring starts over (R48). Ahead, the next read skips forward.
bartap_err_t bt_bytes_seek(bartap_bytes_t *bytes, uint32_t offset)
{
    bartap_stream_t *stream = bytes->stream;

    if (stream == NULL) {
        if (offset > bytes->memory_size) {
            // past the chapter's end
            return BARTAP_ERR_ARG;
        }
        // memory mode, every offset is in reach
        bytes->position = offset;
        return BARTAP_OK;
    }
    if (offset > stream->entry.uncompressed_size) {
        // past the entry's end, per the directory (R22)
        return BARTAP_ERR_ARG;
    }
    if (offset < stream->delivered) {
        // behind the stream. len 0 only asks whether the offset is still held
        bool held = bytes->rolling != NULL &&
                    bt_ring_read(bytes->rolling, offset, NULL, 0) == BARTAP_OK;
        if (!held) {
            held = bt_stream_reread(stream, offset, NULL, 0) == BARTAP_OK;
        }
        if (!held) {
            // gone from both, start the entry over. the next read skips up to offset
            bartap_err_t err = bt_stream_rewind(stream);
            if (err != BARTAP_OK) {
                // stream never opened, pass its failure up
                return err;
            }
            bytes_reset_ring(bytes);
        }
    }
    bytes->position = offset;
    return BARTAP_OK;
}

/* ---- XML tokenizer (R15, R33) ---- */

void bt_xml_init(bartap_xml_t *xml, bartap_bytes_t *bytes, bartap_warnings_t *warnings)
{
    (void)xml;
    (void)bytes;
    (void)warnings;
}

bartap_err_t bt_xml_next(bartap_xml_t *xml, const bartap_xml_token_t **token)
{
    (void)xml;
    (void)token;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bt_xml_text(bartap_xml_t *xml, char *buf, uint32_t cap, uint32_t *got)
{
    (void)xml;
    (void)buf;
    (void)cap;
    (void)got;
    return BARTAP_ERR_UNSUPPORTED;
}

bool bt_xml_attr(const bartap_xml_t *xml, const char *name, uint32_t name_len, const char **value,
                 uint32_t *value_len)
{
    (void)xml;
    (void)name;
    (void)name_len;
    (void)value;
    (void)value_len;
    return false;
}

bartap_err_t bt_xml_seek(bartap_xml_t *xml, uint32_t offset)
{
    (void)xml;
    (void)offset;
    return BARTAP_ERR_UNSUPPORTED;
}

uint32_t bt_utf8_decode(const uint8_t *p, uint32_t len, char *out, uint32_t *out_len)
{
    (void)p;
    (void)len;
    (void)out;
    (void)out_len;
    return 0;
}

uint32_t bt_entity_decode(const char *name, uint32_t name_len, char *out)
{
    (void)name;
    (void)name_len;
    (void)out;
    return 0;
}
