/* CRC-32 against zlib, the reference implementation (R36), across lengths and split points. */
#include <zlib.h>

#include "check.h"
#include "internal.h"

/* Deterministic, so a failure reproduces. */
static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int main(void)
{
    static uint8_t data[70000];
    uint32_t state = 0x12345678u;

    for (size_t i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)(next_random(&state) & 0xFFu);
    }

    /* Every length around the small sizes, then random lengths and offsets. */
    for (size_t len = 0; len < 600; len++) {
        CHECK(bt_crc32(0, data, len) == (uint32_t)crc32(0L, data, (uInt)len));
    }
    for (int i = 0; i < 2000; i++) {
        size_t off = next_random(&state) % sizeof data;
        size_t len = next_random(&state) % (sizeof data - off + 1);
        size_t split = next_random(&state) % (len + 1);
        uint32_t expected = (uint32_t)crc32(0L, data + off, (uInt)len);
        CHECK(bt_crc32(0, data + off, len) == expected);
        CHECK(bt_crc32(bt_crc32(0, data + off, split), data + off + split, len - split) == expected);
    }

    return CHECK_RESULT();
}
