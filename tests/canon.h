/* Canonical serialization of records (R20): test code, not library code. Portable C with no host
 * dependencies, so a device and the host can serialize the same way (R51). */
#ifndef BARTAP_CANON_H
#define BARTAP_CANON_H

#include <stdbool.h>

#include "bartap.h"

typedef void (*canon_sink_fn)(void *user, const uint8_t *data, size_t len);

/* Writes one record field by field, multi-byte values little-endian. Struct padding and unused union
 * bytes never reach the output. Returns false, writing nothing, for an unknown kind or a NULL pointer
 * with a non-zero length.
 *
 *   BLOCK_START      01 kind:u8 level:u8 spine:u16 block_offset:u32 char_offset:u32
 *   CONTAINER_START  02 kind:u8 ordinal:i32
 *   CONTAINER_END    03 kind:u8 ordinal:i32
 *   TEXT             04 len:u32 bytes
 *   STYLE            05 bits:u32
 *   IMAGE            06 dir_offset:u32
 *   LINE_BREAK       07
 *   TOC_ENTRY        08 depth:u16 spine:u16 fragment_len:u32 bytes
 */
bool canon_record(const bartap_record_t *rec, canon_sink_fn sink, void *user);

/* The record stream with text merged, so its bytes depend only on content, never on the text
 * buffer's size (R6, R50). Every record is as canon_record writes it, except a run of consecutive
 * TEXT records, which becomes 04, the run's bytes, then 00. Text never holds a NUL (R21), so the 00
 * is unambiguous, and nothing is buffered: a device with a small text buffer streams the same bytes
 * (R51). Zero-initialize, set sink and user, then call canon_stream_end after the last record. */
typedef struct {
    canon_sink_fn sink;
    void *user;
    bool in_text;
} canon_stream_t;

bool canon_stream_record(canon_stream_t *s, const bartap_record_t *rec);
void canon_stream_end(canon_stream_t *s);

/* FNV-1a, 64 bits, as a sink: user points to the running value, which starts at CANON_HASH_INIT. */
#define CANON_HASH_INIT 0xCBF29CE484222325ull
void canon_hash_sink(void *user, const uint8_t *data, size_t len);

#endif /* BARTAP_CANON_H */
