/* Layer 1 (R3): the archive and its entries. No XML here. */
#include <string.h>

#include "internal.h"

/* ---- Results (R42) ---- */

const char *bartap_strerror(bartap_err_t err)
{
    switch (err) {
    case BARTAP_OK: return "BARTAP_OK";
    case BARTAP_END: return "BARTAP_END";
    case BARTAP_MORE: return "BARTAP_MORE";
    case BARTAP_ERR_IO: return "BARTAP_ERR_IO";
    case BARTAP_ERR_CORRUPT: return "BARTAP_ERR_CORRUPT";
    case BARTAP_ERR_UNSUPPORTED: return "BARTAP_ERR_UNSUPPORTED";
    case BARTAP_ERR_ARG: return "BARTAP_ERR_ARG";
    case BARTAP_ERR_BUFFER_TOO_SMALL: return "BARTAP_ERR_BUFFER_TOO_SMALL";
    case BARTAP_ERR_WRONG_BOOK: return "BARTAP_ERR_WRONG_BOOK";
    }
    return "BARTAP_ERR_UNKNOWN";
}

/* ---- Warnings (R32) ---- */

void bt_warn(bartap_warnings_t *warnings, bartap_warning_t code, uint16_t spine_index,
             uint32_t offset)
{
    warnings->bits |= (uint32_t)code;
    if (warnings->fn != NULL) {
        warnings->fn(warnings->user, code, spine_index, offset);
    }
}

/* ---- I/O (R37) ---- */

bartap_err_t bt_read(const bartap_source_t *src, uint32_t offset, uint8_t *buf, uint32_t len)
{
    if (offset > src->size || len > src->size - offset) {
        return BARTAP_ERR_CORRUPT;
    }
    if (len == 0) {
        return BARTAP_OK;
    }
    return src->read(src->user, offset, buf, len) == 0 ? BARTAP_OK : BARTAP_ERR_IO;
}

/* Only reached through bt_read, which has already checked the range against the size. */
static int memory_read(void *user, uint32_t offset, uint8_t *buf, uint32_t len)
{
    const uint8_t *data = user;
    memcpy(buf, data + offset, len);
    return 0;
}

bartap_err_t bartap_source_memory(bartap_source_t *src, const void *data, uint32_t size)
{
    if (src == NULL || (data == NULL && size != 0)) {
        return BARTAP_ERR_ARG;
    }
    src->read = memory_read;
    src->user = (void *)data; /* memory_read only reads through it */
    src->size = size;
    return BARTAP_OK;
}

/* ---- Name hash (R43) ---- */

uint32_t bt_name_hash(uint32_t hash, const void *name, size_t len)
{
    const uint8_t *p = name;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = p[i];
        if (c >= 'A' && c <= 'Z') {
            c = (uint8_t)(c + ('a' - 'A'));
        }
        hash = (hash ^ c) * 16777619u;
    }
    return hash;
}

/* ---- CRC-32 (R31) ---- */

/* Reflected polynomial 0xEDB88320. Const, so it stays in flash on a microcontroller. */
static const uint32_t crc32_table[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu,
    0xE963A535u, 0x9E6495A3u, 0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
    0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u,
    0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u,
    0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
    0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu, 0x35B5A8FAu, 0x42B2986Cu,
    0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u,
    0xCFBA9599u, 0xB8BDA50Fu, 0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
    0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du, 0x76DC4190u, 0x01DB7106u,
    0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du,
    0x91646C97u, 0xE6635C01u, 0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
    0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u,
    0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u,
    0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
    0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u, 0x5005713Cu, 0x270241AAu,
    0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u,
    0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
    0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u, 0xE3630B12u, 0x94643B84u,
    0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu,
    0x196C3671u, 0x6E6B06E7u, 0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
    0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u, 0xD6D6A3E8u, 0xA1D1937Eu,
    0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u,
    0x316E8EEFu, 0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
    0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu, 0xB2BD0B28u,
    0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu,
    0x72076785u, 0x05005713u, 0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
    0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u,
    0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u,
    0x616BFFD3u, 0x166CCF45u, 0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
    0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu,
    0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u,
    0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

uint32_t bt_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

/* ---- Everything below is a stub ---- */

// convert little-endian bytes to host-endian integers, and vice versa.

// Reads a little-endian 16-bit value from p.
uint16_t bt_get_le16(const uint8_t *p)
{
    (void)p;
    return 0;
}

// Reads a little-endian 32-bit value from p.
uint32_t bt_get_le32(const uint8_t *p)
{
    (void)p;
    return 0;
}

// Writes value to p as little-endian 16 bits.
void bt_put_le16(uint8_t *p, uint16_t value)
{
    (void)p;
    (void)value;
}

// Writes value to p as little-endian 32 bits.
void bt_put_le32(uint8_t *p, uint32_t value)
{
    (void)p;
    (void)value;
}

/* ---- Inflate (R39) ---- */

// clears inflate state, but does not touch the window (R27, R39), stream must handle those itself
void bt_inflate_reset(bartap_inflate_t *inflate)
{
    (void)inflate;
}

// pull compressed input as needed and decode into stream
bartap_err_t bt_inflate_fill(bartap_stream_t *stream)
{
    (void)stream;
    return BARTAP_ERR_UNSUPPORTED;
}

/* ---- Central directory (R22, R24, R40, R43, R44) ---- */

// 
// Locates the central directory and chooses raw or card-backed access up front.
bartap_err_t bt_dir_init(bartap_directory_t *dir, const bartap_source_t *source, uint8_t *buf,
                         uint32_t buf_size)
{
    (void)dir;
    (void)source;
    (void)buf;
    (void)buf_size;
    return BARTAP_ERR_UNSUPPORTED;
}

// Points a cursor at the directory's first record.
void bt_dir_begin(const bartap_directory_t *dir, bt_dir_cursor_t *cursor)
{
    (void)dir;
    (void)cursor;
}

// Returns the next record with its name hash and length, or BARTAP_END after the last.
bartap_err_t bt_dir_next(const bartap_directory_t *dir, bt_dir_cursor_t *cursor,
                         bartap_entry_t *entry, uint32_t *name_hash, uint16_t *name_len)
{
    (void)dir;
    (void)cursor;
    (void)entry;
    (void)name_hash;
    (void)name_len;
    return BARTAP_ERR_UNSUPPORTED;
}

// Loads the entry at a directory-record offset, which may be untrusted.
bartap_err_t bt_dir_entry(const bartap_directory_t *dir, uint32_t dir_offset, bartap_entry_t *entry)
{
    (void)dir;
    (void)dir_offset;
    (void)entry;
    return BARTAP_ERR_UNSUPPORTED;
}

// Copies an entry's real name out of its directory record.
bartap_err_t bt_dir_name(const bartap_directory_t *dir, uint32_t dir_offset, char *buf, uint32_t cap,
                         uint16_t *len)
{
    (void)dir;
    (void)dir_offset;
    (void)buf;
    (void)cap;
    (void)len;
    return BARTAP_ERR_UNSUPPORTED;
}

// Finds an entry by name; the first match in directory order wins, else BARTAP_END.
bartap_err_t bt_dir_find(const bartap_directory_t *dir, const char *name, uint16_t name_len,
                         bartap_entry_t *entry)
{
    (void)dir;
    (void)name;
    (void)name_len;
    (void)entry;
    return BARTAP_ERR_UNSUPPORTED;
}

/* ---- Entry streams (R31) ---- */

// Starts streaming an entry's inflated bytes through the caller's window.
bartap_err_t bt_stream_open(bartap_stream_t *stream, const bartap_source_t *source,
                            const bartap_entry_t *entry, uint8_t *window)
{
    (void)stream;
    (void)source;
    (void)entry;
    (void)window;
    return BARTAP_ERR_UNSUPPORTED;
}

// Delivers the next bytes; BARTAP_END only once the CRC and size match.
bartap_err_t bt_stream_read(bartap_stream_t *stream, uint8_t *buf, uint32_t cap, uint32_t *got)
{
    (void)stream;
    (void)buf;
    (void)cap;
    (void)got;
    return BARTAP_ERR_UNSUPPORTED;
}

// Returns to the entry's start and restarts the CRC check.
bartap_err_t bt_stream_rewind(bartap_stream_t *stream)
{
    (void)stream;
    return BARTAP_ERR_UNSUPPORTED;
}

// Copies already-delivered bytes back out of the window; BARTAP_END if they have left it.
bartap_err_t bt_stream_reread(const bartap_stream_t *stream, uint32_t offset, uint8_t *buf,
                              uint32_t len)
{
    (void)stream;
    (void)offset;
    (void)buf;
    (void)len;
    return BARTAP_ERR_UNSUPPORTED;
}

/* ---- Verify (R31, R38) ---- */

// Prepares an incremental CRC check of a spine item's entry.
bartap_err_t bartap_verify_init(bartap_verify_t *verify, const bartap_book_t *book,
                                uint16_t spine_index, uint8_t *window)
{
    (void)verify;
    (void)book;
    (void)spine_index;
    (void)window;
    return BARTAP_ERR_UNSUPPORTED;
}

// Prepares an incremental CRC check of the entry at a directory-record offset.
bartap_err_t bartap_verify_init_dir(bartap_verify_t *verify, const bartap_book_t *book,
                                    uint32_t dir_offset, uint8_t *window)
{
    (void)verify;
    (void)book;
    (void)dir_offset;
    (void)window;
    return BARTAP_ERR_UNSUPPORTED;
}

// Inflates up to max_bytes; BARTAP_MORE, BARTAP_OK when the entry checks out, or an error.
bartap_err_t bartap_verify_step(bartap_verify_t *verify, uint32_t max_bytes)
{
    (void)verify;
    (void)max_bytes;
    return BARTAP_ERR_UNSUPPORTED;
}

/* ---- Resources (R38) ---- */

// Opens a resource by name for reading.
bartap_err_t bartap_resource_open(bartap_resource_t *res, const bartap_book_t *book,
                                  const char *path, uint32_t path_len, uint8_t *window)
{
    (void)res;
    (void)book;
    (void)path;
    (void)path_len;
    (void)window;
    return BARTAP_ERR_UNSUPPORTED;
}

// Opens a resource from an untrusted directory-record offset, such as an image record's.
bartap_err_t bartap_resource_open_dir(bartap_resource_t *res, const bartap_book_t *book,
                                      uint32_t dir_offset, uint8_t *window)
{
    (void)res;
    (void)book;
    (void)dir_offset;
    (void)window;
    return BARTAP_ERR_UNSUPPORTED;
}

// Opens a whole chapter by spine index, such as for inflating it into PSRAM.
bartap_err_t bartap_resource_open_spine(bartap_resource_t *res, const bartap_book_t *book,
                                        uint16_t spine_index, uint8_t *window)
{
    (void)res;
    (void)book;
    (void)spine_index;
    (void)window;
    return BARTAP_ERR_UNSUPPORTED;
}

// Returns the entry's inflated size, for sizing a destination.
uint32_t bartap_resource_size(const bartap_resource_t *res)
{
    (void)res;
    return 0;
}

// Delivers the next bytes; BARTAP_END only once the CRC and size match.
bartap_err_t bartap_resource_read(bartap_resource_t *res, uint8_t *buf, uint32_t cap, uint32_t *got)
{
    (void)res;
    (void)buf;
    (void)cap;
    (void)got;
    return BARTAP_ERR_UNSUPPORTED;
}
