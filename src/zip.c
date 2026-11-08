/* Layer 1 (R3): the archive and its entries. No XML here. */
#include <stdint.h>
#include <string.h>

#include "bartap.h"
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
    if (warnings->fn != NULL && offset >= warnings->quiet_below) {
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

static uint8_t name_fold(uint8_t c)
{
    return c >= 'A' && c <= 'Z' ? (uint8_t)(c + ('a' - 'A')) : c;
}

uint32_t bt_name_hash(uint32_t hash, const void *name, size_t len)
{
    const uint8_t *p = name;
    for (size_t i = 0; i < len; i++) {
        hash = (hash ^ name_fold(p[i])) * 16777619u;
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

// convert little-endian bytes to host-endian integers, and vice versa.

// Reads a little-endian 16-bit value from p.
uint16_t bt_get_le16(const uint8_t *p)
{
    // pull byte from p, place it into next available byte of uint16_t.
    uint16_t value = 0;
    value |= (uint16_t)p[0]; // least significant byte
    value |= (uint16_t) ((uint16_t)p[1] << 8); // most significant byte, casting after shifting to avoid overflow warning even though no overflow can actually occur
    return value;
}

// Reads a little-endian 32-bit value from p.
uint32_t bt_get_le32(const uint8_t *p)
{
    // pull byte from p, place it into next available byte of uint32_t.
    uint32_t value = 0;
    value |= (uint32_t)p[0]; // least significant byte
    value |= (uint32_t)p[1] << 8;  
    value |= (uint32_t)p[2] << 16;
    value |= (uint32_t)p[3] << 24; // most significant byte
    return value;
}

// Writes value to p as little-endian 16 bits.
void bt_put_le16(uint8_t *p, uint16_t value)
{
    // pull byte from value, place it into next available byte of p.
    p[0] = (uint8_t)(value & 0xFF); // least significant byte
    p[1] = (uint8_t)((value >> 8) & 0xFF); // most significant byte
    return;
}

// Writes value to p as little-endian 32 bits.
void bt_put_le32(uint8_t *p, uint32_t value)
{
    // pull byte from value, place it into next available byte of p.
    p[0] = (uint8_t)(value & 0xFF); // least significant byte
    p[1] = (uint8_t)((value >> 8) & 0xFF);
    p[2] = (uint8_t)((value >> 16) & 0xFF);
    p[3] = (uint8_t)((value >> 24) & 0xFF); // most significant byte
    return;
}

/* ---- Inflate (R39) ---- */

// clears inflate state, but does not touch the window (R27, R39), stream must handle those itself
void bt_inflate_reset(bartap_inflate_t *inflate)
{
    // Zero is BT_INFLATE_HEADER, so the next fill starts by reading a block header.
    memset(inflate, 0, sizeof(*inflate));
}

#define BT_INFLATE_MAX_MATCH 258u

// Zero is the state a fresh stream starts in: between blocks, about to read a header.
enum { BT_INFLATE_HEADER = 0, BT_INFLATE_STORED, BT_INFLATE_HUFFMAN, BT_INFLATE_DONE };

// Window bytes free to write. window_write and window_read are running totals, so bytes not yet
// delivered are never overwritten.
static uint32_t window_space(const bartap_stream_t *stream)
{
    return BARTAP_WINDOW_SIZE - (stream->window_write - stream->window_read);
}

// Bytes the directory's size still allows. Going past it is corrupt, which bounds zip bombs.
static uint32_t output_left(const bartap_stream_t *stream)
{
    return stream->entry.uncompressed_size - stream->window_write;
}

static void window_put(bartap_stream_t *stream, uint8_t byte)
{
    stream->window[stream->window_write & (BARTAP_WINDOW_SIZE - 1u)] = byte;
    stream->window_write++;
}

// The next compressed byte, refilling the input buffer when empty. Running out is corrupt.
static bartap_err_t next_byte(bartap_stream_t *stream, uint8_t *byte)
{
    if (stream->input_pos == stream->input_len) {
        uint32_t left = stream->entry.compressed_size - stream->compressed_read;
        uint32_t len = left < (uint32_t)BARTAP_INPUT_BUF ? left : (uint32_t)BARTAP_INPUT_BUF;
        if (left == 0) {
            return BARTAP_ERR_CORRUPT;
        }
        bartap_err_t err = bt_read(stream->source, stream->data_offset + stream->compressed_read,
                                   stream->input, len);
        if (err != BARTAP_OK) {
            return err;
        }
        stream->compressed_read += len;
        stream->input_pos = 0;
        stream->input_len = len;
    }
    *byte = stream->input[stream->input_pos++];
    return BARTAP_OK;
}

// Up to 16 bits, least significant first. Leftover bits stay in bit_buf between calls.
static bartap_err_t read_bits(bartap_stream_t *stream, uint8_t count, uint32_t *value)
{
    bartap_inflate_t *inf = &stream->inflate;
    while (inf->bit_count < count) {
        uint8_t byte;
        bartap_err_t err = next_byte(stream, &byte);
        if (err != BARTAP_OK) {
            return err;
        }
        inf->bit_buf |= (uint32_t)byte << inf->bit_count;
        inf->bit_count = (uint8_t)(inf->bit_count + 8u);
    }
    *value = inf->bit_buf & ((1u << count) - 1u);
    inf->bit_buf >>= count;
    inf->bit_count = (uint8_t)(inf->bit_count - count);
    return BARTAP_OK;
}

// Builds a canonical Huffman table from code lengths, each below 16. Returns the unused code space:
// 0 is complete (or no codes at all), positive is incomplete, negative is over-subscribed.
static int32_t build_codes(uint16_t *count, uint16_t *symbol, const uint8_t *lengths, uint16_t n)
{
    uint16_t offsets[16];
    int32_t left = 1;

    memset(count, 0, 16u * sizeof(*count));
    for (uint16_t i = 0; i < n; i++) {
        count[lengths[i]]++;
    }
    if (count[0] == n) {
        return 0;
    }
    for (uint8_t len = 1; len < 16u; len++) {
        left = left * 2 - (int32_t)count[len];
        if (left < 0) {
            return left;
        }
    }
    offsets[1] = 0;
    for (uint8_t len = 1; len < 15u; len++) {
        offsets[len + 1] = (uint16_t)(offsets[len] + count[len]);
    }
    for (uint16_t i = 0; i < n; i++) {
        if (lengths[i] != 0) {
            symbol[offsets[lengths[i]]++] = i;
        }
    }
    return left;
}

// A dynamic table must be complete, except that a single one-bit code is allowed (as in zlib).
static bool codes_usable(int32_t left, const uint16_t *count, uint16_t n)
{
    return left == 0 || (left > 0 && (uint32_t)count[0] + count[1] == n);
}

// Walks one bit at a time. Slower than a lookup table, which R39 defers until profiling asks.
static bartap_err_t decode_symbol(bartap_stream_t *stream, const uint16_t *count,
                                  const uint16_t *symbol, uint16_t *out)
{
    int32_t code = 0;
    int32_t first = 0;
    int32_t index = 0;

    for (uint8_t len = 1; len < 16u; len++) {
        uint32_t bit;
        bartap_err_t err = read_bits(stream, 1, &bit);
        if (err != BARTAP_OK) {
            return err;
        }
        code |= (int32_t)bit;
        int32_t codes = count[len];
        if (code - codes < first) {
            *out = symbol[index + (code - first)];
            return BARTAP_OK;
        }
        index += codes;
        first = (first + codes) * 2;
        code *= 2;
    }
    return BARTAP_ERR_CORRUPT;
}

static void build_fixed_tables(bartap_inflate_t *inf)
{
    uint8_t lengths[288];

    memset(lengths, 8, 144);
    memset(lengths + 144, 9, 112);
    memset(lengths + 256, 7, 24);
    memset(lengths + 280, 8, 8);
    (void)build_codes(inf->lit_count, inf->lit_symbol, lengths, 288);

    // All 32 get codes so the two invalid symbols decode and are rejected by inflate_symbol.
    memset(lengths, 5, 32);
    (void)build_codes(inf->dist_count, inf->dist_symbol, lengths, 32);
}

static bartap_err_t read_dynamic_tables(bartap_stream_t *stream)
{
    static const uint8_t order[19] = {16, 17, 18, 0,  8, 7,  9, 6,  10, 5,
                                      11, 4,  12, 3, 13, 2, 14, 1, 15};
    bartap_inflate_t *inf = &stream->inflate;
    uint8_t lengths[286 + 30]; // the most codes a valid block can declare
    uint16_t cl_count[16];
    uint16_t cl_symbol[19];
    uint32_t value;

    bartap_err_t err = read_bits(stream, 14, &value);
    if (err != BARTAP_OK) {
        return err;
    }
    uint16_t lit_n = (uint16_t)((value & 31u) + 257u);
    uint16_t dist_n = (uint16_t)(((value >> 5) & 31u) + 1u);
    uint16_t cl_n = (uint16_t)(((value >> 10) & 15u) + 4u);
    if (lit_n > 286u || dist_n > 30u) {
        return BARTAP_ERR_CORRUPT;
    }

    memset(lengths, 0, 19);
    for (uint16_t i = 0; i < cl_n; i++) {
        err = read_bits(stream, 3, &value);
        if (err != BARTAP_OK) {
            return err;
        }
        lengths[order[i]] = (uint8_t)value;
    }
    if (build_codes(cl_count, cl_symbol, lengths, 19) != 0) {
        return BARTAP_ERR_CORRUPT;
    }

    uint32_t total = (uint32_t)lit_n + dist_n;
    uint32_t i = 0;
    while (i < total) {
        uint16_t sym;
        err = decode_symbol(stream, cl_count, cl_symbol, &sym);
        if (err != BARTAP_OK) {
            return err;
        }
        if (sym < 16u) {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        if (sym == 16u && i == 0) {
            return BARTAP_ERR_CORRUPT; // nothing to repeat
        }
        uint8_t repeated = sym == 16u ? lengths[i - 1u] : 0u;
        uint8_t extra = sym == 16u ? 2u : (sym == 17u ? 3u : 7u);
        err = read_bits(stream, extra, &value);
        if (err != BARTAP_OK) {
            return err;
        }
        uint32_t repeat = value + (sym == 18u ? 11u : 3u);
        if (repeat > total - i) {
            return BARTAP_ERR_CORRUPT;
        }
        memset(lengths + i, repeated, repeat);
        i += repeat;
    }

    if (lengths[256] == 0) {
        return BARTAP_ERR_CORRUPT; // no end-of-block code
    }
    int32_t left = build_codes(inf->lit_count, inf->lit_symbol, lengths, lit_n);
    if (!codes_usable(left, inf->lit_count, lit_n)) {
        return BARTAP_ERR_CORRUPT;
    }
    left = build_codes(inf->dist_count, inf->dist_symbol, lengths + lit_n, dist_n);
    if (!codes_usable(left, inf->dist_count, dist_n)) {
        return BARTAP_ERR_CORRUPT;
    }
    return BARTAP_OK;
}

static bartap_err_t read_block_header(bartap_stream_t *stream)
{
    bartap_inflate_t *inf = &stream->inflate;
    uint32_t value;

    bartap_err_t err = read_bits(stream, 3, &value);
    if (err != BARTAP_OK) {
        return err;
    }
    inf->final_block = (uint8_t)(value & 1u);
    inf->block_type = (uint8_t)(value >> 1);

    switch (inf->block_type) {
    case 0: {
        uint32_t len;
        uint32_t nlen;
        // Drop the padding. read_bits takes one byte at a time, so only padding is left in bit_buf.
        inf->bit_buf = 0;
        inf->bit_count = 0;
        err = read_bits(stream, 16, &len);
        if (err == BARTAP_OK) {
            err = read_bits(stream, 16, &nlen);
        }
        if (err != BARTAP_OK) {
            return err;
        }
        if ((len ^ nlen) != 0xFFFFu) {
            return BARTAP_ERR_CORRUPT;
        }
        inf->stored_left = len;
        inf->state = BT_INFLATE_STORED;
        return BARTAP_OK;
    }
    case 1:
        build_fixed_tables(inf);
        inf->state = BT_INFLATE_HUFFMAN;
        return BARTAP_OK;
    case 2:
        err = read_dynamic_tables(stream);
        if (err != BARTAP_OK) {
            return err;
        }
        inf->state = BT_INFLATE_HUFFMAN;
        return BARTAP_OK;
    default:
        return BARTAP_ERR_CORRUPT;
    }
}

static void end_block(bartap_inflate_t *inf)
{
    inf->state = inf->final_block ? BT_INFLATE_DONE : BT_INFLATE_HEADER;
}

// Copies what fits, and stored_left says where to carry on when the window has room again.
static bartap_err_t copy_stored(bartap_stream_t *stream)
{
    bartap_inflate_t *inf = &stream->inflate;
    uint32_t space = window_space(stream);
    uint32_t len = inf->stored_left < space ? inf->stored_left : space;

    if (len > output_left(stream)) {
        return BARTAP_ERR_CORRUPT;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint32_t value;
        bartap_err_t err = read_bits(stream, 8, &value);
        if (err != BARTAP_OK) {
            return err;
        }
        window_put(stream, (uint8_t)value);
    }
    inf->stored_left -= len;
    if (inf->stored_left == 0) {
        end_block(inf);
    }
    return BARTAP_OK;
}

// One symbol: a literal, the end of the block, or a whole match.
static bartap_err_t inflate_symbol(bartap_stream_t *stream)
{
    static const uint16_t length_base[29] = {3,  4,  5,  6,  7,  8,  9,  10,  11,  13,
                                             15, 17, 19, 23, 27, 31, 35, 43,  51,  59,
                                             67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                             2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dist_base[30] = {1,    2,    3,    4,    5,    7,    9,    13,
                                           17,   25,   33,   49,   65,   97,   129,  193,
                                           257,  385,  513,  769,  1025, 1537, 2049, 3073,
                                           4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                           6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    bartap_inflate_t *inf = &stream->inflate;
    uint16_t sym;
    uint32_t extra;

    bartap_err_t err = decode_symbol(stream, inf->lit_count, inf->lit_symbol, &sym);
    if (err != BARTAP_OK) {
        return err;
    }
    if (sym < 256u) {
        if (output_left(stream) == 0) {
            return BARTAP_ERR_CORRUPT;
        }
        window_put(stream, (uint8_t)sym);
        return BARTAP_OK;
    }
    if (sym == 256u) {
        end_block(inf);
        return BARTAP_OK;
    }
    if (sym > 285u) {
        return BARTAP_ERR_CORRUPT;
    }

    err = read_bits(stream, length_extra[sym - 257u], &extra);
    if (err != BARTAP_OK) {
        return err;
    }
    uint32_t length = length_base[sym - 257u] + extra;

    err = decode_symbol(stream, inf->dist_count, inf->dist_symbol, &sym);
    if (err != BARTAP_OK) {
        return err;
    }
    if (sym > 29u) {
        return BARTAP_ERR_CORRUPT;
    }
    err = read_bits(stream, dist_extra[sym], &extra);
    if (err != BARTAP_OK) {
        return err;
    }
    uint32_t distance = dist_base[sym] + extra;

    // window_write counts all output so far, so this rejects reaching before the entry's start.
    if (distance > stream->window_write || length > output_left(stream)) {
        return BARTAP_ERR_CORRUPT;
    }
    for (uint32_t i = 0; i < length; i++) {
        window_put(stream,
                   stream->window[(stream->window_write - distance) & (BARTAP_WINDOW_SIZE - 1u)]);
    }
    return BARTAP_OK;
}

// pull compressed input as needed and decode into stream
bartap_err_t bt_inflate_fill(bartap_stream_t *stream)
{
    bartap_inflate_t *inf = &stream->inflate;

    // Each pass does one bounded unit of work, so the space check runs before every symbol.
    for (;;) {
        bartap_err_t err;

        if (window_space(stream) < BT_INFLATE_MAX_MATCH) {
            return BARTAP_OK;
        }

        switch (inf->state) {
        case BT_INFLATE_DONE:
            return BARTAP_OK;

        case BT_INFLATE_HEADER:
            err = read_block_header(stream);
            break;

        case BT_INFLATE_STORED:
            err = copy_stored(stream);
            break;

        case BT_INFLATE_HUFFMAN:
            err = inflate_symbol(stream);
            break;

        default:
            return BARTAP_ERR_CORRUPT;
        }

        if (err != BARTAP_OK) {
            return err;
        }
    }
}

/* ---- Central directory (R22, R24, R40, R43, R44) ---- */

#define BT_END_SIZE 22u
#define BT_END_SIGNATURE 0x06054B50u
#define BT_END_COMMENT_MAX 65535u
#define BT_ZIP64_LOCATOR_SIZE 20u
#define BT_ZIP64_LOCATOR_SIGNATURE 0x07064B50u
#define BT_ZIP64_END_SIZE 56u
#define BT_ZIP64_END_SIGNATURE 0x06064B50u
#define BT_DIR_RECORD_MIN 46u
#define BT_END_SCAN_CHUNK 128u

// end record values
typedef struct {
    uint32_t count;
    uint32_t size;
    uint32_t offset;
} bt_end_values_t;

// Replaces the values with the ZIP64 record's, if a locator sits before the end record. Without one
// *found is false and the classic values stand: a file can really have 65,535 entries. *limit
// becomes where the record starts, since the directory has to end before it.
static bartap_err_t read_zip64_end(const bartap_source_t *source, uint32_t end_pos,
                                   bt_end_values_t *values, uint32_t *limit, bool *found)
{
    uint8_t locator[BT_ZIP64_LOCATOR_SIZE];
    uint8_t record[BT_ZIP64_END_SIZE];

    // if no locator, use classic values
    *found = false;
    if (end_pos < BT_ZIP64_LOCATOR_SIZE) {
        return BARTAP_OK;
    }

    // locator candidate, read it and check
    uint32_t locator_pos = end_pos - BT_ZIP64_LOCATOR_SIZE;
    bartap_err_t err = bt_read(source, locator_pos, locator, BT_ZIP64_LOCATOR_SIZE);
    if (err != BARTAP_OK) {
        // error reading locator, pass error up
        return err;
    }
    if (bt_get_le32(locator) != BT_ZIP64_LOCATOR_SIGNATURE) {
        // signature mismatch, not a ZIP64 locator, treat as not present
        return BARTAP_OK;
    }

    // locator found
    *found = true;

    // check locator values are supported
    if (bt_get_le32(locator + 4) != 0 || bt_get_le32(locator + 16) > 1u ||
        bt_get_le32(locator + 12) != 0) {
        return BARTAP_ERR_UNSUPPORTED; // a split archive, or an offset above 32 bits (R40)
    }

    uint32_t record_pos = bt_get_le32(locator + 8);
    if (locator_pos < BT_ZIP64_END_SIZE || record_pos > locator_pos - BT_ZIP64_END_SIZE) {
        // record clips file start or overlaps locator, not possible, corrupt
        return BARTAP_ERR_CORRUPT;
    }

    err = bt_read(source, record_pos, record, BT_ZIP64_END_SIZE);
    if (err != BARTAP_OK) {
        // error reading record, pass error up
        return err;
    }

    if (bt_get_le32(record) != BT_ZIP64_END_SIGNATURE) {
        // signature mismatch, not a ZIP64 record, corrupt
        return BARTAP_ERR_CORRUPT;
    }

    if (bt_get_le32(record + 16) != 0 || bt_get_le32(record + 20) != 0 ||
        bt_get_le32(record + 36) != 0 || bt_get_le32(record + 44) != 0 ||
        bt_get_le32(record + 52) != 0) {
        return BARTAP_ERR_UNSUPPORTED; // a split archive, or a value above 32 bits (R40)
    }

    // read the candidate values
    values->count = bt_get_le32(record + 32);
    values->size = bt_get_le32(record + 40);
    values->offset = bt_get_le32(record + 48);
    *limit = record_pos;
    return BARTAP_OK;
}

// Evaluates one candidate end record, following ZIP64 and checking the values are possible (R29).
static bartap_err_t read_end_values(const bartap_source_t *source, uint32_t end_pos,
                                    const uint8_t *end, bt_end_values_t *values)
{
    uint16_t disk = bt_get_le16(end + 4);
    uint16_t dir_disk = bt_get_le16(end + 6);
    uint32_t limit = end_pos;

    if ((disk != 0 && disk != 0xFFFFu) || (dir_disk != 0 && dir_disk != 0xFFFFu)) {
        return BARTAP_ERR_UNSUPPORTED; // a split archive
    }

    // read candidate values (may be ZIP64 placeholders)
    values->count = bt_get_le16(end + 10);
    values->size = bt_get_le32(end + 12);
    values->offset = bt_get_le32(end + 16);

    // if any are placeholders, read the ZIP64 record and replace them
    if (disk == 0xFFFFu || dir_disk == 0xFFFFu || values->count == 0xFFFFu ||
        values->size == UINT32_MAX || values->offset == UINT32_MAX) {
        bool found;
        bartap_err_t err = read_zip64_end(source, end_pos, values, &limit, &found);
        if (err != BARTAP_OK) {
            return err;
        }
        if (!found && (disk != 0 || dir_disk != 0)) {
            // placeholder with no replacement and non-zero disk number, potentially split
            return BARTAP_ERR_UNSUPPORTED;
        }
    }

    // check if directory fits in the file, and that the count is possible (R29)
    if (values->offset > limit || values->size > limit - values->offset ||
        values->count > values->size / BT_DIR_RECORD_MIN) {
        // values are out of bounds
        return BARTAP_ERR_CORRUPT;
    }
    return BARTAP_OK;
}

// Scans backwards for the end record. The latest usable one whose comment reaches exactly to the
// end of the file wins, and failing that the latest usable one, which accepts bytes added after an
// archive (R30). Comment bytes can look like a record, so a signature alone proves nothing.
static bartap_err_t find_end_record(const bartap_source_t *source, bt_end_values_t *values)
{
    uint8_t chunk[BT_END_SCAN_CHUNK];
    uint8_t end[BT_END_SIZE];
    bt_end_values_t candidate;
    bool have_fallback = false;
    bool saw_unsupported = false;

    // reject archives smaller than end record size
    if (source->size < BT_END_SIZE) {
        return BARTAP_ERR_CORRUPT;
    }
    // start at end, search progressively further up for end record. covers for padding and comments
    uint32_t newest = source->size - BT_END_SIZE;
    uint32_t oldest = newest < BT_END_COMMENT_MAX ? 0 : newest - BT_END_COMMENT_MAX;
    uint32_t above = newest + 1u; // candidates at or above this have been checked

    while (above > oldest) {
        // Chunks overlap by 3 bytes, so no signature is split across two of them.
        uint32_t span = above - oldest;
        if (span > BT_END_SCAN_CHUNK - 3u) {
            span = BT_END_SCAN_CHUNK - 3u;
        }
        uint32_t base = above - span;
        bartap_err_t err = bt_read(source, base, chunk, span + 3u);
        if (err != BARTAP_OK) {
            return err;
        }
        // scan backwards for the signature, and check each candidate end record
        for (uint32_t i = span; i-- != 0;) {
            if (bt_get_le32(chunk + i) != BT_END_SIGNATURE) {
                continue;
            }
            uint32_t pos = base + i;
            err = bt_read(source, pos, end, BT_END_SIZE);
            if (err == BARTAP_OK) {
                // read candidate end record
                err = read_end_values(source, pos, end, &candidate);
            }
            if (err == BARTAP_ERR_UNSUPPORTED) {
                // unsupported features, not fallback candidate
                saw_unsupported = true;
            } else if (err == BARTAP_OK) {
                // found valid end record
                // check if it reached end of file
                if (BT_END_SIZE + (uint32_t)bt_get_le16(end + 20) == source->size - pos) {
                    // reached end of file, valid and conformant end record
                    *values = candidate;
                    return BARTAP_OK;
                }
                // didn't reach end, fallback candidate (latest one in the file)
                if (!have_fallback) {
                    *values = candidate;
                    have_fallback = true;
                }
            } else if (err != BARTAP_ERR_CORRUPT) {
                return err;
            }
        }
        above = base;
    }

    if (have_fallback) {
        return BARTAP_OK;
    }
    return saw_unsupported ? BARTAP_ERR_UNSUPPORTED : BARTAP_ERR_CORRUPT;
}

// Locates the central directory and chooses raw or card-backed access up front. dir is only written
// on success.
bartap_err_t bt_dir_init(bartap_directory_t *dir, const bartap_source_t *source, uint8_t *buf,
                         uint32_t buf_size)
{
    bt_end_values_t values;
    const uint8_t *raw = NULL;

    bartap_err_t err = find_end_record(source, &values);
    if (err != BARTAP_OK) {
        return err;
    }

    // A directory that does not fit is never partly buffered (R24).
    if (buf != NULL && values.size <= buf_size) {
        err = bt_read(source, values.offset, buf, values.size);
        if (err != BARTAP_OK) {
            return err;
        }
        raw = buf;
    }

    dir->source = source;
    dir->raw = raw;
    dir->offset = values.offset;
    dir->size = values.size;
    dir->count = values.count;
    return BARTAP_OK;
}

#define BT_DIR_RECORD_SIGNATURE 0x02014B50u
#define BT_ZIP64_EXTRA_ID 0x0001u
#define BT_METHOD_UNSUPPORTED 0xFFu // fits a spine item's method, and no stream will open it
#define BT_NAME_CHUNK 64u
#define BT_ZIP64_VALUE_SIZE 8u
#define BT_EXTRA_HEADER_SIZE 4u

// Reads directory bytes by their offset in the file, from RAM or the card. Every lookup comes
// through here, so both modes behave the same (R24) and nothing reads outside the directory (R29).
static bartap_err_t dir_read(const bartap_directory_t *dir, uint32_t offset, uint8_t *out,
                             uint32_t len)
{
    // reject offsets before the directory
    if (offset < dir->offset) {
        return BARTAP_ERR_CORRUPT;
    }
    // check the whole read stays inside the directory (R29)
    uint32_t relative = offset - dir->offset;
    if (relative > dir->size || len > dir->size - relative) {
        return BARTAP_ERR_CORRUPT;
    }
    if (dir->raw == NULL) {
        // none mode, read from the card
        return bt_read(dir->source, offset, out, len);
    }
    // raw mode, copy from RAM. out can be NULL when there's nothing to copy, memcpy can't take that
    if (len != 0) {
        memcpy(out, dir->raw + relative, len);
    }
    return BARTAP_OK;
}

// Takes the next 8-byte value from a ZIP64 extra field.
static bartap_err_t read_zip64_value(const bartap_directory_t *dir, uint32_t *pos, uint32_t *left,
                                     uint32_t *value, bool *unsupported)
{
    uint8_t bytes[BT_ZIP64_VALUE_SIZE];

    if (*left < BT_ZIP64_VALUE_SIZE) {
        // not enough left in the field for a value, corrupt
        return BARTAP_ERR_CORRUPT;
    }
    bartap_err_t err = dir_read(dir, *pos, bytes, BT_ZIP64_VALUE_SIZE);
    if (err != BARTAP_OK) {
        // error reading value, pass error up
        return err;
    }
    // keep the low 32 bits
    *value = bt_get_le32(bytes);
    if (bt_get_le32(bytes + 4) != 0) {
        *unsupported = true; // above 32 bits (R40)
    }
    // move past the value
    *pos += BT_ZIP64_VALUE_SIZE;
    *left -= BT_ZIP64_VALUE_SIZE;
    return BARTAP_OK;
}

// Replaces an entry's placeholders from its ZIP64 extra field. The field holds a value only for
// each placeholder, in this order. A placeholder with no field to replace it is corrupt.
static bartap_err_t read_zip64_extra(const bartap_directory_t *dir, uint32_t pos, uint32_t left,
                                     bartap_entry_t *entry, bool disk_placeholder,
                                     bool *unsupported)
{
    uint8_t header[BT_EXTRA_HEADER_SIZE];

    // walk the extra fields until the ZIP64 one. each starts with a 4-byte header, id then size
    while (left >= BT_EXTRA_HEADER_SIZE) {
        bartap_err_t err = dir_read(dir, pos, header, BT_EXTRA_HEADER_SIZE);
        if (err != BARTAP_OK) {
            // error reading header, pass error up
            return err;
        }
        uint32_t id = bt_get_le16(header);
        uint32_t size = bt_get_le16(header + 2);
        pos += BT_EXTRA_HEADER_SIZE;
        left -= BT_EXTRA_HEADER_SIZE;
        if (size > left) {
            // field claims more than the extra data holds, corrupt
            return BARTAP_ERR_CORRUPT;
        }
        if (id != BT_ZIP64_EXTRA_ID) {
            // not the ZIP64 field, skip it
            pos += size;
            left -= size;
            continue;
        }

        // ZIP64 field found. only placeholders have a value, size now tracks what's left of it
        if (entry->uncompressed_size == UINT32_MAX) {
            err = read_zip64_value(dir, &pos, &size, &entry->uncompressed_size, unsupported);
            if (err != BARTAP_OK) {
                return err;
            }
        }
        if (entry->compressed_size == UINT32_MAX) {
            err = read_zip64_value(dir, &pos, &size, &entry->compressed_size, unsupported);
            if (err != BARTAP_OK) {
                return err;
            }
        }
        if (entry->local_offset == UINT32_MAX) {
            err = read_zip64_value(dir, &pos, &size, &entry->local_offset, unsupported);
            if (err != BARTAP_OK) {
                return err;
            }
        }
        // disk number is 4 bytes in the field, not 8, so it's read separately
        if (disk_placeholder) {
            if (size < BT_EXTRA_HEADER_SIZE) { // the same size as the disk number
                // no room for the disk number, corrupt
                return BARTAP_ERR_CORRUPT;
            }
            err = dir_read(dir, pos, header, BT_EXTRA_HEADER_SIZE);
            if (err != BARTAP_OK) {
                // error reading disk number, pass error up
                return err;
            }
            if (bt_get_le32(header) != 0) {
                *unsupported = true; // on another disk
            }
        }
        return BARTAP_OK;
    }
    // no ZIP64 field, nothing to replace the placeholder, corrupt
    return BARTAP_ERR_CORRUPT;
}

// Reads the record at an offset that may be untrusted. An entry Bartap cannot read is still
// returned, marked by its method: one encrypted file should not stop a walk, only that file's own
// open. Bad structure is corrupt, because the lengths leading to the next record cannot be trusted.
static bartap_err_t parse_record(const bartap_directory_t *dir, uint32_t offset,
                                 bartap_entry_t *entry, uint16_t *name_len, uint32_t *record_size)
{
    uint8_t record[BT_DIR_RECORD_MIN];
    bartap_entry_t parsed;

    // read the fixed part of the record
    bartap_err_t err = dir_read(dir, offset, record, BT_DIR_RECORD_MIN);
    if (err != BARTAP_OK) {
        // error reading record, pass error up
        return err;
    }
    if (bt_get_le32(record) != BT_DIR_RECORD_SIGNATURE) {
        // signature mismatch, not a record, corrupt
        return BARTAP_ERR_CORRUPT;
    }

    // name, extra and comment follow the fixed part, the record is all four together
    uint16_t names = bt_get_le16(record + 28);
    uint16_t extras = bt_get_le16(record + 30);
    uint32_t size = BT_DIR_RECORD_MIN + (uint32_t)names + extras + bt_get_le16(record + 32);
    // whole record has to fit in what's left of the directory (R29)
    if (size > dir->size - (offset - dir->offset)) {
        return BARTAP_ERR_CORRUPT;
    }

    // read candidate values (sizes and offset may be ZIP64 placeholders)
    parsed.dir_offset = offset;
    parsed.crc32 = bt_get_le32(record + 16);
    parsed.compressed_size = bt_get_le32(record + 20);
    parsed.uncompressed_size = bt_get_le32(record + 24);
    parsed.local_offset = bt_get_le32(record + 42);
    parsed.method = bt_get_le16(record + 10);

    // entries we can't read are still valid records, so note it and carry on
    uint16_t disk = bt_get_le16(record + 34);
    bool unsupported = (bt_get_le16(record + 8) & 1u) != 0 || // encrypted
                       (disk != 0 && disk != 0xFFFFu);        // on another disk

    // if any are placeholders, read the ZIP64 extra field and replace them
    if (parsed.uncompressed_size == UINT32_MAX || parsed.compressed_size == UINT32_MAX ||
        parsed.local_offset == UINT32_MAX || disk == 0xFFFFu) {
        err = read_zip64_extra(dir, offset + BT_DIR_RECORD_MIN + names, extras, &parsed,
                               disk == 0xFFFFu, &unsupported);
        if (err != BARTAP_OK) {
            // bad or unreadable ZIP64 field, pass error up
            return err;
        }
    }

    // Normalized here, or method 264 would become 8 in a spine item's single byte.
    if (unsupported || (parsed.method != 0 && parsed.method != 8)) {
        parsed.method = BT_METHOD_UNSUPPORTED;
    }

    // record is good, only write results now so a failure leaves nothing half-filled
    *entry = parsed;
    *name_len = names;
    *record_size = size;
    return BARTAP_OK;
}

// Points a cursor at the directory's first record.
void bt_dir_begin(const bartap_directory_t *dir, bt_dir_cursor_t *cursor)
{
    cursor->offset = dir->offset;
}

// Returns the next record with its name hash and length, or BARTAP_END after the last. Walks by the
// directory's size, so a wrong count changes nothing: every record is at least 46 bytes, which
// bounds the walk. On failure the cursor stays put.
bartap_err_t bt_dir_next(const bartap_directory_t *dir, bt_dir_cursor_t *cursor,
                         bartap_entry_t *entry, uint32_t *name_hash, uint16_t *name_len)
{
    uint8_t chunk[BT_NAME_CHUNK];
    uint32_t record_size;
    uint16_t names;

    if (cursor->offset - dir->offset >= dir->size) {
        // reached the end of the directory, done
        return BARTAP_END;
    }
    bartap_err_t err = parse_record(dir, cursor->offset, entry, &names, &record_size);
    if (err != BARTAP_OK) {
        // bad record, pass error up. cursor hasn't moved
        return err;
    }

    // hash the name in chunks, feeding the previous result back in
    uint32_t hash = BT_NAME_HASH_INIT;
    uint32_t pos = cursor->offset + BT_DIR_RECORD_MIN;
    uint32_t left = names;
    while (left != 0) {
        uint32_t len = left < BT_NAME_CHUNK ? left : BT_NAME_CHUNK;
        err = dir_read(dir, pos, chunk, len);
        if (err != BARTAP_OK) {
            // error reading name, pass error up
            return err;
        }
        hash = bt_name_hash(hash, chunk, len);
        pos += len;
        left -= len;
    }

    // success, move the cursor to the next record
    *name_hash = hash;
    *name_len = names;
    cursor->offset += record_size;
    return BARTAP_OK;
}

// Loads the entry at a directory-record offset, which may be untrusted.
bartap_err_t bt_dir_entry(const bartap_directory_t *dir, uint32_t dir_offset, bartap_entry_t *entry)
{
    uint32_t record_size;
    uint16_t names;

    // only the entry is wanted, name length and record size are thrown away
    return parse_record(dir, dir_offset, entry, &names, &record_size);
}

// Copies an entry's real name out of its directory record, not NUL-terminated. If it does not fit,
// *len still says how much room it needs, and nothing is copied.
bartap_err_t bt_dir_name(const bartap_directory_t *dir, uint32_t dir_offset, char *buf, uint32_t cap,
                         uint16_t *len)
{
    bartap_entry_t entry;
    uint32_t record_size;
    uint16_t names;

    bartap_err_t err = parse_record(dir, dir_offset, &entry, &names, &record_size);
    if (err != BARTAP_OK) {
        // bad record, pass error up
        return err;
    }
    // set before the size check, so the caller learns how much room it needs
    *len = names;
    if (names > cap) {
        return BARTAP_ERR_BUFFER_TOO_SMALL;
    }
    // name sits right after the fixed part
    return dir_read(dir, dir_offset + BT_DIR_RECORD_MIN, (uint8_t *)buf, names);
}

// Compares a record's name with a known one under the same folding as the hash (R43). The lengths
// have already matched.
static bartap_err_t name_matches(const bartap_directory_t *dir, uint32_t dir_offset,
                                 const char *name, uint16_t name_len, bool *matches)
{
    uint8_t chunk[BT_NAME_CHUNK];
    uint32_t pos = dir_offset + BT_DIR_RECORD_MIN;
    uint32_t done = 0;

    *matches = false;
    // compare in chunks, stopping at the first difference
    while (done < name_len) {
        uint32_t len = name_len - done < BT_NAME_CHUNK ? name_len - done : BT_NAME_CHUNK;
        bartap_err_t err = dir_read(dir, pos + done, chunk, len);
        if (err != BARTAP_OK) {
            // error reading name, pass error up
            return err;
        }
        for (uint32_t i = 0; i < len; i++) {
            if (name_fold(chunk[i]) != name_fold((uint8_t)name[done + i])) {
                // names differ, which isn't an error. *matches stays false
                return BARTAP_OK;
            }
        }
        done += len;
    }
    // every byte matched
    *matches = true;
    return BARTAP_OK;
}

// Finds an entry by name; the first match in directory order wins, else BARTAP_END. The hash only
// filters: the name is known here, so a collision is never returned as a match.
bartap_err_t bt_dir_find(const bartap_directory_t *dir, const char *name, uint16_t name_len,
                         bartap_entry_t *entry)
{
    uint32_t want = bt_name_hash(BT_NAME_HASH_INIT, name, name_len);
    bt_dir_cursor_t cursor;
    bartap_entry_t candidate;
    uint32_t hash;
    uint16_t len;
    bartap_err_t err;

    // walk in directory order, so the first match wins (R44)
    bt_dir_begin(dir, &cursor);
    while ((err = bt_dir_next(dir, &cursor, &candidate, &hash, &len)) == BARTAP_OK) {
        if (len != name_len || hash != want) {
            // can't be this entry, skip the name compare
            continue;
        }
        bool matches;
        // hash and length match, compare the real name in case of a collision
        err = name_matches(dir, candidate.dir_offset, name, name_len, &matches);
        if (err != BARTAP_OK) {
            // error reading name, pass error up
            return err;
        }
        if (matches) {
            // match found
            *entry = candidate;
            return BARTAP_OK;
        }
    }
    // BARTAP_END if nothing matched, otherwise the walk's failure
    return err;
}

/* ---- Entry streams (R31) ---- */

#define BT_LOCAL_HEADER_SIZE 30u
#define BT_LOCAL_HEADER_SIGNATURE 0x04034B50u

// Back to the entry's first byte. A stored entry is decoded as one final stored block, so both
// methods share the window, the size limit and the end-of-entry checks.
static void stream_start(bartap_stream_t *stream)
{
    stream->compressed_read = 0;
    stream->delivered = 0;
    stream->crc32 = 0;
    stream->window_write = 0;
    stream->window_read = 0;
    stream->window_valid = 0;
    stream->input_pos = 0;
    stream->input_len = 0;
    stream->status = BARTAP_OK;
    bt_inflate_reset(&stream->inflate);
    if (stream->entry.method == 0) {
        stream->inflate.state = BT_INFLATE_STORED;
        stream->inflate.stored_left = stream->entry.uncompressed_size;
        stream->inflate.final_block = 1;
    }
}

// The local header only locates the data: names, sizes and CRC come from the directory (R22).
static bartap_err_t stream_find_data(bartap_stream_t *stream)
{
    const uint32_t size = stream->source->size;
    uint8_t header[BT_LOCAL_HEADER_SIZE];

    bartap_err_t err = bt_read(stream->source, stream->entry.local_offset, header,
                               BT_LOCAL_HEADER_SIZE);
    if (err != BARTAP_OK) {
        return err;
    }
    if (bt_get_le32(header) != BT_LOCAL_HEADER_SIGNATURE) {
        return BARTAP_ERR_CORRUPT;
    }
    uint32_t skip = BT_LOCAL_HEADER_SIZE + (uint32_t)bt_get_le16(header + 26) +
                    bt_get_le16(header + 28);
    if (skip > size || stream->entry.local_offset > size - skip) {
        return BARTAP_ERR_CORRUPT;
    }
    stream->data_offset = stream->entry.local_offset + skip;
    if (stream->entry.compressed_size > size - stream->data_offset) {
        return BARTAP_ERR_CORRUPT;
    }
    return BARTAP_OK;
}

// Copies bytes from the window by their offset in the entry. The caller has checked the range.
static void window_copy(const bartap_stream_t *stream, uint32_t offset, uint8_t *buf, uint32_t len)
{
    uint32_t pos = offset & (BARTAP_WINDOW_SIZE - 1u);
    uint32_t first = BARTAP_WINDOW_SIZE - pos;

    if (first > len) {
        first = len;
    }
    memcpy(buf, stream->window + pos, first);
    memcpy(buf + first, stream->window, len - first);
}

// Starts streaming an entry's inflated bytes through the caller's window. After a failure the
// stream only returns that failure: a NULL source marks it as never having opened.
bartap_err_t bt_stream_open(bartap_stream_t *stream, const bartap_source_t *source,
                            const bartap_entry_t *entry, uint8_t *window)
{
    bartap_err_t err = BARTAP_ERR_UNSUPPORTED;

    stream->source = source;
    stream->entry = *entry;
    stream->window = window;
    stream->data_offset = 0;
    stream_start(stream);

    if (stream->entry.method == 0 || stream->entry.method == 8) {
        err = stream_find_data(stream);
    }
    if (err != BARTAP_OK) {
        stream->source = NULL;
        stream->status = err;
    }
    return err;
}

// Makes sure undelivered bytes wait in the window: BARTAP_OK if some do, else BARTAP_END once the
// CRC and size match, or an error. Sets the sticky status for the end and failures.
static bartap_err_t stream_ready(bartap_stream_t *stream)
{
    if (stream->status != BARTAP_OK) {
        return stream->status;
    }
    if (stream->window_write == stream->window_read) {
        bartap_err_t err = bt_inflate_fill(stream);
        // Before checking err: a failed fill may still have overwritten older bytes.
        stream->window_valid = stream->window_write < BARTAP_WINDOW_SIZE ? stream->window_write
                                                                         : BARTAP_WINDOW_SIZE;
        if (err != BARTAP_OK) {
            stream->status = err;
            return err;
        }
        // An empty window always has room, so a fill that added nothing has reached the end.
        if (stream->window_write == stream->window_read) {
            bool intact = stream->window_write == stream->entry.uncompressed_size &&
                          stream->crc32 == stream->entry.crc32;
            stream->status = intact ? BARTAP_END : BARTAP_ERR_CORRUPT;
            return stream->status;
        }
    }
    return BARTAP_OK;
}

// Delivers the next bytes; BARTAP_END only once the CRC and size match. A failure or the end is
// sticky until a rewind.
bartap_err_t bt_stream_read(bartap_stream_t *stream, uint8_t *buf, uint32_t cap, uint32_t *got)
{
    *got = 0;
    if (stream->status != BARTAP_OK) {
        return stream->status;
    }
    if (cap == 0) {
        return BARTAP_ERR_ARG;
    }
    bartap_err_t err = stream_ready(stream);
    if (err != BARTAP_OK) {
        // the end or a failure, pass it up
        return err;
    }

    uint32_t len = stream->window_write - stream->window_read;
    if (len > cap) {
        len = cap;
    }
    window_copy(stream, stream->window_read, buf, len);
    stream->crc32 = bt_crc32(stream->crc32, buf, len);
    stream->window_read += len;
    stream->delivered += len;
    *got = len;
    return BARTAP_OK;
}

// Returns to the entry's start and restarts the CRC check. Clears a failure or the end.
bartap_err_t bt_stream_rewind(bartap_stream_t *stream)
{
    if (stream->source == NULL) {
        return stream->status;
    }
    stream_start(stream);
    return BARTAP_OK;
}

// Copies already-delivered bytes back out of the window; BARTAP_END if they have left it, and
// BARTAP_ERR_ARG if they have not been delivered yet.
bartap_err_t bt_stream_reread(const bartap_stream_t *stream, uint32_t offset, uint8_t *buf,
                              uint32_t len)
{
    if (offset > stream->delivered || len > stream->delivered - offset) {
        return BARTAP_ERR_ARG;
    }
    if (offset < stream->window_write - stream->window_valid) {
        return BARTAP_END;
    }
    if (len != 0) {
        window_copy(stream, offset, buf, len);
    }
    return BARTAP_OK;
}

/* ---- Verify (R31, R38) ---- */

// Marks a stream as never having opened, as bt_stream_open does on a failure, so every later call
// only returns err.
static bartap_err_t stream_fail(bartap_stream_t *stream, bartap_err_t err)
{
    stream->source = NULL;
    stream->status = err;
    return err;
}

// The entry facts a spine slot already holds, copied from its directory record when the book
// opened (R22), so a spine lookup needs no directory read.
bartap_err_t bt_spine_entry(const bartap_book_t *book, uint16_t spine_index, bartap_entry_t *entry)
{
    if (spine_index >= book->spine_count) {
        // past the spine the book holds, the caller's mistake
        return BARTAP_ERR_ARG;
    }
    // copy the facts across, the path hash and name length aren't part of an entry
    const bartap_spine_item_t *item = &book->spine[spine_index];
    if (item->dir_offset == BARTAP_NO_DIR_OFFSET) {
        // no archive entry matched it at open, already warned about. the book is damaged
        return BARTAP_ERR_CORRUPT;
    }
    entry->dir_offset = item->dir_offset;
    entry->local_offset = item->local_offset;
    entry->compressed_size = item->compressed_size;
    entry->uncompressed_size = item->uncompressed_size;
    entry->crc32 = item->crc32;
    entry->method = item->method;
    return BARTAP_OK;
}

// Prepares an incremental CRC check of a spine item's entry.
bartap_err_t bartap_verify_init(bartap_verify_t *verify, const bartap_book_t *book,
                                uint16_t spine_index, uint8_t *window)
{
    bartap_entry_t entry;

    bartap_err_t err = bt_spine_entry(book, spine_index, &entry);
    if (err != BARTAP_OK) {
        // bad index, mark the stream unopened so a step only repeats this
        return stream_fail(&verify->stream, err);
    }
    // a verify is just a stream nobody reads from
    return bt_stream_open(&verify->stream, book->source, &entry, window);
}

// Prepares an incremental CRC check of the entry at a directory-record offset.
bartap_err_t bartap_verify_init_dir(bartap_verify_t *verify, const bartap_book_t *book,
                                    uint32_t dir_offset, uint8_t *window)
{
    bartap_entry_t entry;

    // the offset is untrusted, bt_dir_entry checks it (R29)
    bartap_err_t err = bt_dir_entry(&book->directory, dir_offset, &entry);
    if (err != BARTAP_OK) {
        // bad record, mark the stream unopened and pass error up
        return stream_fail(&verify->stream, err);
    }
    return bt_stream_open(&verify->stream, book->source, &entry, window);
}

// Inflates up to max_bytes; BARTAP_MORE, BARTAP_OK when the entry checks out, or an error. The CRC
// is folded straight over the window, so no buffer is needed. Repeats its result once finished.
bartap_err_t bartap_verify_step(bartap_verify_t *verify, uint32_t max_bytes)
{
    bartap_stream_t *stream = &verify->stream;

    if (max_bytes == 0) {
        // no budget means no progress, same as a zero cap on a read
        return BARTAP_ERR_ARG;
    }
    while (max_bytes > 0) {
        // refill the window if everything in it has been checked
        bartap_err_t err = stream_ready(stream);
        if (err == BARTAP_END) {
            // size and CRC matched, the entry checks out
            return BARTAP_OK;
        }
        if (err != BARTAP_OK) {
            // bad data or a failed read, pass error up
            return err;
        }
        // take what's waiting, but only up to the wrap so it's one run of bytes
        uint32_t pos = stream->window_read & (BARTAP_WINDOW_SIZE - 1u);
        uint32_t len = stream->window_write - stream->window_read;
        if (len > BARTAP_WINDOW_SIZE - pos) {
            len = BARTAP_WINDOW_SIZE - pos; // the next pass takes the rest
        }
        if (len > max_bytes) {
            len = max_bytes;
        }
        // fold into the CRC and mark the bytes delivered, freeing their room in the window
        stream->crc32 = bt_crc32(stream->crc32, stream->window + pos, len);
        stream->window_read += len;
        stream->delivered += len;
        max_bytes -= len;
    }
    // budget spent. even at the entry's last byte, the end check waits for the next step
    return BARTAP_MORE;
}

/* ---- Resources (R38) ---- */

// Opens a resource by name for reading. BARTAP_END if no entry has that name; the first in directory order wins (R44).
bartap_err_t bartap_resource_open(bartap_resource_t *res, const bartap_book_t *book,
                                  const char *path, uint32_t path_len, uint8_t *window)
{
    bartap_entry_t entry;

    if (path_len > 0xFFFFu) {
        // longer than any name a directory record can hold, so nothing matches
        return stream_fail(&res->stream, BARTAP_END);
    }
    bartap_err_t err = bt_dir_find(&book->directory, path, (uint16_t)path_len, &entry);
    if (err != BARTAP_OK) {
        // no match or a bad directory, mark the stream unopened and pass it up
        return stream_fail(&res->stream, err);
    }
    // an unsupported method fails here, not at the first read
    return bt_stream_open(&res->stream, book->source, &entry, window);
}

// Opens a resource from an untrusted directory-record offset, such as an image record's.
bartap_err_t bartap_resource_open_dir(bartap_resource_t *res, const bartap_book_t *book,
                                      uint32_t dir_offset, uint8_t *window)
{
    bartap_entry_t entry;

    // the offset is untrusted, bt_dir_entry checks it (R29)
    bartap_err_t err = bt_dir_entry(&book->directory, dir_offset, &entry);
    if (err != BARTAP_OK) {
        // bad record, mark the stream unopened and pass error up
        return stream_fail(&res->stream, err);
    }
    return bt_stream_open(&res->stream, book->source, &entry, window);
}

// Opens a whole chapter by spine index, such as for inflating it into PSRAM.
bartap_err_t bartap_resource_open_spine(bartap_resource_t *res, const bartap_book_t *book,
                                        uint16_t spine_index, uint8_t *window)
{
    bartap_entry_t entry;

    bartap_err_t err = bt_spine_entry(book, spine_index, &entry);
    if (err != BARTAP_OK) {
        // bad index, mark the stream unopened so a read only repeats this
        return stream_fail(&res->stream, err);
    }
    return bt_stream_open(&res->stream, book->source, &entry, window);
}

// Returns the entry's inflated size, for sizing a destination. 0 if the resource never opened.
uint32_t bartap_resource_size(const bartap_resource_t *res)
{
    if (res->stream.source == NULL) {
        // failed open, the entry may be half-filled or missing
        return 0;
    }
    // from the directory (R22). a read still fails if the data disagrees
    return res->stream.entry.uncompressed_size;
}

// Delivers the next bytes; BARTAP_END only once the CRC and size match.
bartap_err_t bartap_resource_read(bartap_resource_t *res, uint8_t *buf, uint32_t cap, uint32_t *got)
{
    // a resource is just a stream, the checks all happen there (R31)
    return bt_stream_read(&res->stream, buf, cap, got);
}
