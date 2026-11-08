/* Deterministic XML-ish documents for property checks and fuzz replays: mostly markup fragments
 * that meet at awkward places, sometimes raw bytes. Test code, not library code. */
#ifndef BARTAP_TEST_XML_GEN_H
#define BARTAP_TEST_XML_GEN_H

#include <stdint.h>
#include <string.h>

static inline uint32_t xml_gen_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Writes document number seed to out and returns its length, at most cap. */
static inline uint32_t xml_gen(uint32_t seed, uint8_t *out, uint32_t cap)
{
    static const char *const atoms[] = {
        "<p>", "</p>", "<a href=\"x&amp;y\">", "<b", ">", "<", "&", ";", "&nbsp;", "&#", "x41",
        "&#x1F600;", "<!--", "-->", "-", "<![CDATA[", "]]>", "]", "<?", "?>", "<!DOCTYPE x [", "\"",
        "'", "=", "/", " ", "\n", "\xC3", "\xA9", "\xE2\x80", "\x94", "\xF0\x9F", "\xEF\xBB\xBF",
        "\x01", "\x7F", "text", "</", "html:", "Ab", "&eacute;", "&bogus", "\xFF\xFE", "\xC2\x85",
        "<img src=a.png/>", " k=v", " epub:type='footnote'", "&#x85;", "&#0;", "</ x>",
    };
    uint32_t state = seed * 2654435761u + 1u;
    uint32_t len = 0;

    if (xml_gen_random(&state) % 4u == 0) {
        /* raw bytes now and then */
        uint32_t n = xml_gen_random(&state) % 300u;
        for (; len < n && len < cap; len++) {
            out[len] = (uint8_t)xml_gen_random(&state);
        }
        return len;
    }
    uint32_t pieces = xml_gen_random(&state) % 150u;
    for (uint32_t i = 0; i < pieces; i++) {
        const char *a = atoms[xml_gen_random(&state) % (sizeof atoms / sizeof atoms[0])];
        uint32_t n = (uint32_t)strlen(a);
        if (n > cap - len) {
            break;
        }
        memcpy(out + len, a, n);
        len += n;
    }
    return len;
}

#endif /* BARTAP_TEST_XML_GEN_H */
