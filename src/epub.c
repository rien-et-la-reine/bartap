/* Layer 2 (R3): container, package and XML. Depends only on ocf.c. Everything here is a stub. */
#include "internal.h"

/* ---- Byte source (R15) ---- */

void bt_bytes_init_stream(bartap_bytes_t *bytes, bartap_stream_t *stream, bartap_rolling_t *rolling)
{
    (void)bytes;
    (void)stream;
    (void)rolling;
}

void bt_bytes_init_memory(bartap_bytes_t *bytes, const uint8_t *data, uint32_t size,
                          bartap_rolling_t *rolling)
{
    (void)bytes;
    (void)data;
    (void)size;
    (void)rolling;
}

bartap_err_t bt_bytes_read(bartap_bytes_t *bytes, uint8_t *buf, uint32_t cap, uint32_t *got)
{
    (void)bytes;
    (void)buf;
    (void)cap;
    (void)got;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bt_bytes_seek(bartap_bytes_t *bytes, uint32_t offset)
{
    (void)bytes;
    (void)offset;
    return BARTAP_ERR_UNSUPPORTED;
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

/* ---- Package (R23, R25, R41, R43) ---- */

bartap_err_t bt_href_resolve(const char *base, uint32_t base_len, const char *href, uint32_t href_len,
                             char *out, uint32_t cap, uint16_t *out_len)
{
    (void)base;
    (void)base_len;
    (void)href;
    (void)href_len;
    (void)out;
    (void)cap;
    (void)out_len;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bt_read_container(bartap_book_t *book, bartap_open_work_t *work, uint8_t *window)
{
    (void)book;
    (void)work;
    (void)window;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bt_opf_first_pass(bartap_book_t *book, bartap_open_work_t *work, uint8_t *window)
{
    (void)book;
    (void)work;
    (void)window;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bt_opf_second_pass(bartap_book_t *book, bartap_open_work_t *work, uint8_t *window)
{
    (void)book;
    (void)work;
    (void)window;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bt_match_spine(bartap_book_t *book, bartap_open_work_t *work)
{
    (void)book;
    (void)work;
    return BARTAP_ERR_UNSUPPORTED;
}

/* ---- Book ---- */

bartap_err_t bartap_open(bartap_book_t *book, const bartap_source_t *source,
                         bartap_spine_item_t *spine, uint16_t spine_capacity,
                         uint8_t *dir_buf, uint32_t dir_buf_size,
                         bartap_open_work_t *work, uint8_t *window,
                         bartap_warn_fn warn_fn, void *warn_user)
{
    (void)book;
    (void)source;
    (void)spine;
    (void)spine_capacity;
    (void)dir_buf;
    (void)dir_buf_size;
    (void)work;
    (void)window;
    (void)warn_fn;
    (void)warn_user;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_book_metadata(const bartap_book_t *book, bartap_metadata_t *out)
{
    (void)book;
    (void)out;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_book_spine_count(const bartap_book_t *book, uint16_t *count, uint32_t *required)
{
    (void)book;
    (void)count;
    (void)required;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_book_spine_linear(const bartap_book_t *book, uint16_t spine_index, bool *linear)
{
    (void)book;
    (void)spine_index;
    (void)linear;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_book_cover(const bartap_book_t *book, uint32_t *dir_offset)
{
    (void)book;
    (void)dir_offset;
    return BARTAP_ERR_UNSUPPORTED;
}

uint32_t bartap_book_warnings(const bartap_book_t *book)
{
    (void)book;
    return 0;
}
