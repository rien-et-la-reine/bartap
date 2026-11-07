/* Layer 2 (R3): byte source and XML. Depends only on zip.c. Everything here is a stub. */
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
