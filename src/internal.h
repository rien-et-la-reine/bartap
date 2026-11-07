/* Interfaces between Bartap's layers: text -> epub -> ocf (R3). Not part of the public API. */
#ifndef BARTAP_INTERNAL_H
#define BARTAP_INTERNAL_H

#include "bartap.h"

/* ---- ocf.c ---- */

/* zlib convention: start from 0, and feed the previous result back in to continue. */
uint32_t bt_crc32(uint32_t crc, const uint8_t *data, size_t len);

/* 32-bit FNV-1a over ASCII-folded bytes (R43). Frozen, because bookmarks store it (R12). Start from
 * BT_NAME_HASH_INIT, and feed the previous result back in to continue. */
#define BT_NAME_HASH_INIT 0x811C9DC5u
uint32_t bt_name_hash(uint32_t hash, const void *name, size_t len);

/* The one gate to the read callback: every offset and length is checked here before use (R29, R37).
 * A range outside the archive is BARTAP_ERR_CORRUPT and never reaches the callback. */
bartap_err_t bt_read(const bartap_source_t *src, uint32_t offset, uint8_t *buf, uint32_t len);

/* Sets the sticky bit and calls the callback, if any (R32). */
void bt_warn(bartap_warnings_t *warnings, bartap_warning_t code, uint16_t spine_index,
             uint32_t offset);

#endif /* BARTAP_INTERNAL_H */
