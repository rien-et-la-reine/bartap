/* Prints what Bartap sees in a zip file, for check_directory_zipfile.py to compare with Python's
 * zipfile (R36). Walks in raw mode and then none, and reads every entry to its end, so each real
 * file is also a test of the streams.
 *
 * One line per entry, in decimal except the name:
 *   dir_offset local compressed uncompressed crc method hash name_hex stream_result find_offset */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define FIND_LIMIT 2000u /* bt_dir_find on every entry of a huge directory would take too long */

static uint8_t window[BARTAP_WINDOW_SIZE];
static bartap_stream_t stream;
static uint8_t out[BARTAP_WINDOW_SIZE];
static char name[65536];

static bartap_err_t read_to_end(const bartap_source_t *source, const bartap_entry_t *entry)
{
    uint32_t got;
    bartap_err_t err = bt_stream_open(&stream, source, entry, window);

    while (err == BARTAP_OK) {
        err = bt_stream_read(&stream, out, sizeof out, &got);
    }
    return err;
}

static int dump(const bartap_source_t *source, uint8_t *buf, uint32_t buf_size)
{
    bartap_directory_t dir;
    bt_dir_cursor_t cursor;
    bartap_entry_t entry;
    bartap_entry_t again;
    uint32_t hash;
    uint16_t len;
    uint16_t name_len;
    bartap_err_t err = bt_dir_init(&dir, source, buf, buf_size);

    if (err != BARTAP_OK) {
        printf("init %s\n", bartap_strerror(err));
        return 0;
    }
    printf("directory %u %u %s\n", (unsigned)dir.offset, (unsigned)dir.size,
           dir.raw != NULL ? "raw" : "none");

    bt_dir_begin(&dir, &cursor);
    while ((err = bt_dir_next(&dir, &cursor, &entry, &hash, &len)) == BARTAP_OK) {
        if (bt_dir_entry(&dir, entry.dir_offset, &again) != BARTAP_OK ||
            again.dir_offset != entry.dir_offset || again.local_offset != entry.local_offset ||
            again.compressed_size != entry.compressed_size ||
            again.uncompressed_size != entry.uncompressed_size || again.crc32 != entry.crc32 ||
            again.method != entry.method) {
            printf("bt_dir_entry disagrees with bt_dir_next\n");
            return 1;
        }
        if (bt_dir_name(&dir, entry.dir_offset, name, sizeof name, &name_len) != BARTAP_OK ||
            name_len != len) {
            printf("bt_dir_name disagrees with bt_dir_next\n");
            return 1;
        }

        long find_offset = -1;
        if (dir.count <= FIND_LIMIT) {
            if (bt_dir_find(&dir, name, name_len, &again) != BARTAP_OK) {
                printf("bt_dir_find cannot find a name from the directory\n");
                return 1;
            }
            find_offset = (long)again.dir_offset;
        }

        printf("%u %u %u %u %u %u %u ", (unsigned)entry.dir_offset, (unsigned)entry.local_offset,
               (unsigned)entry.compressed_size, (unsigned)entry.uncompressed_size,
               (unsigned)entry.crc32, (unsigned)entry.method, (unsigned)hash);
        for (uint32_t i = 0; i < name_len; i++) {
            printf("%02x", (unsigned)(uint8_t)name[i]);
        }
        printf(" %s %ld\n", bartap_strerror(read_to_end(source, &entry)), find_offset);
    }
    printf("end %s\n", bartap_strerror(err));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL || fseek(f, 0, SEEK_END) != 0) {
        return 2;
    }
    long size = ftell(f);
    rewind(f);
    uint8_t *bytes = malloc((size_t)size + 1u);
    uint8_t *buf = malloc((size_t)size + 1u);
    if (size < 0 || bytes == NULL || buf == NULL ||
        fread(bytes, 1, (size_t)size, f) != (size_t)size) {
        return 2;
    }
    fclose(f);

    bartap_source_t source;
    if (bartap_source_memory(&source, bytes, (uint32_t)size) != BARTAP_OK) {
        return 2;
    }

    int failed = dump(&source, buf, (uint32_t)size);
    printf("---\n");
    failed |= dump(&source, NULL, 0);

    free(bytes);
    free(buf);
    return failed;
}
