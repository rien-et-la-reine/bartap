/* Layer 2 (R3): container, package and book. Depends only on xml.c and zip.c. */
#include "internal.h"

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
