/* Prints one hash per document of the tokenizer's dump (R33). Built at several read-ahead sizes;
 * compare_outputs.py runs every build and requires identical output, so tokens cannot depend on the
 * read-ahead. Hashes the files named, such as a fuzz corpus, or else generated documents. */
#include <inttypes.h>
#include <stdio.h>

#include "xml_dump.h"
#include "xml_gen.h"

#define GENERATED 5000u

static uint8_t buf[1u << 20];

static void hash_one(const char *label, uint32_t n)
{
    xml_dump_t d = {0};
    xml_dump_memory(&d, buf, n, 64);
    printf("%s %016" PRIx64 "\n", label, xml_dump_hash(&d));
    xml_dump_free(&d);
}

int main(int argc, char **argv)
{
    char label[32];

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            FILE *f = fopen(argv[i], "rb");
            if (f == NULL) {
                perror(argv[i]);
                return 1;
            }
            size_t n = fread(buf, 1, sizeof buf, f);
            fclose(f);
            hash_one(argv[i], (uint32_t)n);
        }
        return 0;
    }
    for (uint32_t seed = 1; seed <= GENERATED; seed++) {
        snprintf(label, sizeof label, "gen%u", seed);
        hash_one(label, xml_gen(seed, buf, sizeof buf));
    }
    return 0;
}
