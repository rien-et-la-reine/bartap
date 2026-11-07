/* Interfaces between Bartap's layers: text -> package, xml -> zip (R3). Not public API. */
#ifndef BARTAP_INTERNAL_H
#define BARTAP_INTERNAL_H

#include "bartap.h"

/* ---- zip.c ---- */

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

/* Stubs from here on. Those returning bartap_err_t return BARTAP_ERR_UNSUPPORTED until implemented. */

/* Little-endian fields, byte by byte: no struct overlays, no unaligned accesses (R19). */
uint16_t bt_get_le16(const uint8_t *p);
uint32_t bt_get_le32(const uint8_t *p);
void bt_put_le16(uint8_t *p, uint16_t value);
void bt_put_le32(uint8_t *p, uint32_t value);

/* Inflate (R39). Decodes into the window until fewer than 258 bytes of space remain, or the data
 * ends, so a match never suspends. */
void bt_inflate_reset(bartap_inflate_t *inflate);
bartap_err_t bt_inflate_fill(bartap_stream_t *stream);

/* Central directory (R22, R24, R40, R43, R44). */
typedef struct {
    uint32_t offset;    /* next record */
    uint32_t remaining; /* records left, bounding the walk (R29) */
} bt_dir_cursor_t;

/* Finds the end record, reads ZIP64 where it fits, and chooses raw or none up front. */
bartap_err_t bt_dir_init(bartap_directory_t *dir, const bartap_source_t *source, uint8_t *buf,
                         uint32_t buf_size);
void bt_dir_begin(const bartap_directory_t *dir, bt_dir_cursor_t *cursor);

/* The next record, with its name hash and length. BARTAP_END at the end of the directory. */
bartap_err_t bt_dir_next(const bartap_directory_t *dir, bt_dir_cursor_t *cursor,
                         bartap_entry_t *entry, uint32_t *name_hash, uint16_t *name_len);

/* The entry at a directory-record offset, which may be untrusted (R9, R29). */
bartap_err_t bt_dir_entry(const bartap_directory_t *dir, uint32_t dir_offset, bartap_entry_t *entry);

/* The real name, always from the directory (R22). */
bartap_err_t bt_dir_name(const bartap_directory_t *dir, uint32_t dir_offset, char *buf, uint32_t cap,
                         uint16_t *len);

/* First in directory order wins (R44). BARTAP_END if no entry matches. */
bartap_err_t bt_dir_find(const bartap_directory_t *dir, const char *name, uint16_t name_len,
                         bartap_entry_t *entry);

/* Entry streams (R31). */
bartap_err_t bt_stream_open(bartap_stream_t *stream, const bartap_source_t *source,
                            const bartap_entry_t *entry, uint8_t *window);

/* BARTAP_OK with *got bytes, or BARTAP_END only once CRC and size match. */
bartap_err_t bt_stream_read(bartap_stream_t *stream, uint8_t *buf, uint32_t cap, uint32_t *got);

/* Back to the entry's start. Restarts the CRC check (R31). */
bartap_err_t bt_stream_rewind(bartap_stream_t *stream);

/* Copies already-delivered bytes back out of the window, without a second CRC fold. BARTAP_END if
 * offset has left the window (R26). */
bartap_err_t bt_stream_reread(const bartap_stream_t *stream, uint32_t offset, uint8_t *buf,
                              uint32_t len);

/* ---- xml.c ---- */

/* Byte source (R15). */
void bt_bytes_init_stream(bartap_bytes_t *bytes, bartap_stream_t *stream, bartap_rolling_t *rolling);
void bt_bytes_init_memory(bartap_bytes_t *bytes, const uint8_t *data, uint32_t size,
                          bartap_rolling_t *rolling);
bartap_err_t bt_bytes_read(bartap_bytes_t *bytes, uint8_t *buf, uint32_t cap, uint32_t *got);

/* Moves to a source offset: from memory or the ring where possible, else by rewinding (R48). */
bartap_err_t bt_bytes_seek(bartap_bytes_t *bytes, uint32_t offset);

/* XML tokenizer, with its own interface so it can be tested and fuzzed alone (R15, R33). */
void bt_xml_init(bartap_xml_t *xml, bartap_bytes_t *bytes, bartap_warnings_t *warnings);

/* The next token, or BARTAP_END when the source is finished. */
bartap_err_t bt_xml_next(bartap_xml_t *xml, const bartap_xml_token_t **token);

/* Decoded UTF-8 of the current text token, whole characters only. *got is 0 at the token's end. */
bartap_err_t bt_xml_text(bartap_xml_t *xml, char *buf, uint32_t cap, uint32_t *got);

/* An attribute of the current tag, by name with its prefix. false if the tag does not have it. */
bool bt_xml_attr(const bartap_xml_t *xml, const char *name, uint32_t name_len, const char **value,
                 uint32_t *value_len);

/* Resets the read-ahead and resumes at a source offset (R48). */
bartap_err_t bt_xml_seek(bartap_xml_t *xml, uint32_t offset);

/* Decodes one character at p, falling back to Windows-1252 on invalid UTF-8 (R30). Writes UTF-8 to
 * out (at least BARTAP_TEXT_MIN bytes). Returns the input bytes used, or 0 if more input is needed. */
uint32_t bt_utf8_decode(const uint8_t *p, uint32_t len, char *out, uint32_t *out_len);

/* Decodes the entity after an `&`, including character references (R30). Returns bytes written. */
uint32_t bt_entity_decode(const char *name, uint32_t name_len, char *out);

/* ---- package.c ---- */

/* Joins href to base, percent-decoding once and refusing %2F (R43). */
bartap_err_t bt_href_resolve(const char *base, uint32_t base_len, const char *href, uint32_t href_len,
                             char *out, uint32_t cap, uint16_t *out_len);

/* The steps of bartap_open (R23, R41). */
bartap_err_t bt_read_container(bartap_book_t *book, bartap_open_work_t *work, uint8_t *window);
bartap_err_t bt_opf_first_pass(bartap_book_t *book, bartap_open_work_t *work, uint8_t *window);
bartap_err_t bt_opf_second_pass(bartap_book_t *book, bartap_open_work_t *work, uint8_t *window);
bartap_err_t bt_match_spine(bartap_book_t *book, bartap_open_work_t *work);

/* ---- text.c ---- */

/* Element classes (R46). Returns a tag index below BARTAP_TAG_COUNT, or BARTAP_TAG_COUNT if unknown. */
uint8_t bt_tag_lookup(const char *name, uint32_t name_len);

/* Fills rec from any multi-record moment in progress (R20). false if none is. */
bool bt_text_emit_pending(bartap_text_t *text, bartap_record_t *rec);

/* Handle one token, updating frames and queuing records (R4, R45, R46, R47). */
bartap_err_t bt_text_start_tag(bartap_text_t *text, const bartap_xml_token_t *token);
bartap_err_t bt_text_end_tag(bartap_text_t *text, const bartap_xml_token_t *token);

/* Fills rec with text up to the buffer's size (R6, R50). */
bartap_err_t bt_text_fill(bartap_text_t *text, bartap_record_t *rec);

/* The image record's directory offset (R9). BARTAP_END if it cannot be resolved. */
bartap_err_t bt_image_lookup(bartap_text_t *text, const char *src, uint32_t src_len,
                             uint32_t *dir_offset);

/* Rolling buffer (R48). */
void bt_ring_append(bartap_rolling_t *rolling, const uint8_t *data, uint32_t len);
bartap_err_t bt_ring_read(const bartap_rolling_t *rolling, uint32_t offset, uint8_t *buf,
                          uint32_t len);
void bt_checkpoint_take(bartap_text_t *text, uint32_t block_offset, uint32_t resume_offset);
bartap_err_t bt_checkpoint_restore(bartap_text_t *text, const bartap_checkpoint_t *checkpoint);

/* Replays to a landing without copying text or looking up images (R49). */
bartap_err_t bt_text_replay(bartap_text_t *text, uint32_t block_offset, uint32_t char_offset);

#endif /* BARTAP_INTERNAL_H */
