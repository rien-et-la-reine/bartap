/* Runs a fuzz target without libFuzzer: over the files named, such as a corpus or a crash to
 * reproduce, or else over generated documents, so its checks run in every ctest. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "xml_gen.h"

#define GENERATED 3000u

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv)
{
    static uint8_t buf[1u << 20];

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            FILE *f = fopen(argv[i], "rb");
            if (f == NULL) {
                perror(argv[i]);
                return 1;
            }
            size_t n = fread(buf, 1, sizeof buf, f);
            fclose(f);
            LLVMFuzzerTestOneInput(buf, n);
        }
        printf("%d inputs passed\n", argc - 1);
        return 0;
    }
    for (uint32_t seed = 1; seed <= GENERATED; seed++) {
        uint32_t n = xml_gen(seed, buf, sizeof buf);
        LLVMFuzzerTestOneInput(buf, n);
    }
    printf("%u generated inputs passed\n", GENERATED);
    return 0;
}
