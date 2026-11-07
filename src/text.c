/* Layer 3 (R3): records, TOC, bookmarks, seek. Depends only on epub.c and ocf.c. Everything here is
 * a stub. */
#include "internal.h"

/* ---- Element classes and records (R4, R6, R45, R46, R47) ---- */

uint8_t bt_tag_lookup(const char *name, uint32_t name_len)
{
    (void)name;
    (void)name_len;
    return BARTAP_TAG_COUNT;
}

bool bt_text_emit_pending(bartap_text_t *text, bartap_record_t *rec)
{
    (void)text;
    (void)rec;
    return false;
}

bartap_err_t bt_text_start_tag(bartap_text_t *text, const bartap_xml_token_t *token)
{
    (void)text;
    (void)token;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bt_text_end_tag(bartap_text_t *text, const bartap_xml_token_t *token)
{
    (void)text;
    (void)token;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bt_text_fill(bartap_text_t *text, bartap_record_t *rec)
{
    (void)text;
    (void)rec;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bt_image_lookup(bartap_text_t *text, const char *src, uint32_t src_len,
                             uint32_t *dir_offset)
{
    (void)text;
    (void)src;
    (void)src_len;
    (void)dir_offset;
    return BARTAP_ERR_UNSUPPORTED;
}

/* ---- Text streams (R11, R17, R20) ---- */

bartap_err_t bartap_text_open(bartap_text_t *text, const bartap_book_t *book, uint16_t spine_index,
                              uint8_t *window, char *text_buf, uint32_t text_cap,
                              bartap_rolling_t *rolling, bartap_warn_fn warn_fn, void *warn_user)
{
    (void)text;
    (void)book;
    (void)spine_index;
    (void)window;
    (void)text_buf;
    (void)text_cap;
    (void)rolling;
    (void)warn_fn;
    (void)warn_user;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_text_open_memory(bartap_text_t *text, const bartap_book_t *book,
                                     uint16_t spine_index, const uint8_t *data, uint32_t size,
                                     char *text_buf, uint32_t text_cap, bartap_rolling_t *rolling,
                                     bartap_warn_fn warn_fn, void *warn_user)
{
    (void)text;
    (void)book;
    (void)spine_index;
    (void)data;
    (void)size;
    (void)text_buf;
    (void)text_cap;
    (void)rolling;
    (void)warn_fn;
    (void)warn_user;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_text_next(bartap_text_t *text, bartap_record_t *rec)
{
    (void)text;
    (void)rec;
    return BARTAP_ERR_UNSUPPORTED;
}

uint32_t bartap_text_warnings(const bartap_text_t *text)
{
    (void)text;
    return 0;
}

/* ---- Seek (R12, R48, R49) ---- */

bartap_err_t bartap_text_seek(bartap_text_t *text, const bartap_bookmark_t *bookmark)
{
    (void)text;
    (void)bookmark;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bt_text_replay(bartap_text_t *text, uint32_t block_offset, uint32_t char_offset)
{
    (void)text;
    (void)block_offset;
    (void)char_offset;
    return BARTAP_ERR_UNSUPPORTED;
}

/* ---- Rolling buffer (R48) ---- */

bartap_err_t bartap_rolling_init(bartap_rolling_t *rolling, uint8_t *ring, uint32_t size)
{
    (void)rolling;
    (void)ring;
    (void)size;
    return BARTAP_ERR_UNSUPPORTED;
}

void bt_ring_append(bartap_rolling_t *rolling, const uint8_t *data, uint32_t len)
{
    (void)rolling;
    (void)data;
    (void)len;
}

bartap_err_t bt_ring_read(const bartap_rolling_t *rolling, uint32_t offset, uint8_t *buf,
                          uint32_t len)
{
    (void)rolling;
    (void)offset;
    (void)buf;
    (void)len;
    return BARTAP_ERR_UNSUPPORTED;
}

void bt_checkpoint_take(bartap_text_t *text, uint32_t block_offset, uint32_t resume_offset)
{
    (void)text;
    (void)block_offset;
    (void)resume_offset;
}

bartap_err_t bt_checkpoint_restore(bartap_text_t *text, const bartap_checkpoint_t *checkpoint)
{
    (void)text;
    (void)checkpoint;
    return BARTAP_ERR_UNSUPPORTED;
}

/* ---- Bookmarks (R12, R19) ---- */

bartap_err_t bartap_bookmark_write(const bartap_book_t *book, uint16_t spine_index,
                                   uint32_t block_offset, uint32_t char_offset, uint8_t *out)
{
    (void)book;
    (void)spine_index;
    (void)block_offset;
    (void)char_offset;
    (void)out;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_bookmark_read(const bartap_book_t *book, const uint8_t *in,
                                  bartap_bookmark_t *out)
{
    (void)book;
    (void)in;
    (void)out;
    return BARTAP_ERR_UNSUPPORTED;
}

/* ---- TOC (R13, R26) ---- */

bartap_err_t bartap_toc_open(bartap_toc_t *toc, const bartap_book_t *book, uint8_t *window,
                             char *text_buf, uint32_t text_cap, bartap_warn_fn warn_fn,
                             void *warn_user)
{
    (void)toc;
    (void)book;
    (void)window;
    (void)text_buf;
    (void)text_cap;
    (void)warn_fn;
    (void)warn_user;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_toc_next(bartap_toc_t *toc, bartap_record_t *rec)
{
    (void)toc;
    (void)rec;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_toc_resolve(bartap_text_t *text, const char *id, uint32_t id_len,
                                bartap_bookmark_t *out)
{
    (void)text;
    (void)id;
    (void)id_len;
    (void)out;
    return BARTAP_ERR_UNSUPPORTED;
}

uint32_t bartap_toc_warnings(const bartap_toc_t *toc)
{
    (void)toc;
    return 0;
}
