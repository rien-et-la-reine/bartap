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

#endif /* BARTAP_CANON_H */
