/* Diagnostic fixture exporter only: exact 200 ms SCP streams to P64.
   Part of VICE; GPL-2.0-or-later. Does not modify its input file. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "p64.h"
void *lib_malloc(size_t n) { return malloc(n); }
void *lib_realloc(void *p, size_t n) { return realloc(p, n); }
void lib_free(void *p) { free(p); }
static uint32_t le32(const unsigned char *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
int main(int argc, char **argv)
{
    unsigned char header[688];
    unsigned char record[4 + 255 * 12];
    unsigned char sample[2];
    unsigned int ht;
    unsigned int rev;
    unsigned int i;
    unsigned int word;
    uint32_t offset;
    uint32_t count;
    uint64_t duration;
    uint64_t time;
    FILE *input;
    FILE *output;
    TP64Image image;
    TP64MemoryStream stream;
    if (argc != 3) {
        return 1;
    }
    input = fopen(argv[1], "rb");
    if (!input || fread(header, 1, sizeof(header), input) != sizeof(header)
        || header[0] != 'S' || header[1] != 'C' || header[2] != 'P'
        || !header[5] || header[9] || header[11]) { return 2; }
    P64ImageCreate(&image);
    image.WriteProtected = 1;
    for (ht = 0; ht < 84; ht++) {
        offset = le32(header + 16 + ht * 4);
        if (!offset) {
            continue;
        }
        if (fseek(input, offset, SEEK_SET)
            || fread(record, 1, 4 + 12 * header[5], input) != 4U + 12U * header[5]) { return 3; }
        duration = 0;
        for (rev = 0; rev < header[5]; rev++) {
            duration += le32(record + 4 + rev * 12);
        }
        if (duration != 8000000) { /* No angular/time normalization. */
            return 4;
        }
        time = 0;
        for (rev = 0; rev < header[5]; rev++) {
            count = le32(record + 8 + rev * 12);
            if (fseek(input, offset + le32(record + 12 + rev * 12), SEEK_SET)) {
                return 5;
            }
            for (i = 0; i < count; i++) {
                if (fread(sample, 1, 2, input) != 2) {
                    return 6;
                }
                word = ((unsigned int)sample[0] << 8) | sample[1];
                time += word ? word : 65536;
                if (!word || time >= 8000000) {
                    continue;
                }
                if ((time * 2) % 5) { /* Require exact 16 MHz positions. */
                    return 7;
                }
                P64PulseStreamAddPulse(&image.PulseStreams[0][ht + 2],
                                       (uint32_t)(time * 2 / 5), 0xffffffffU);
            }
        }
    }
    fclose(input);
    P64MemoryStreamCreate(&stream);
    if (!P64ImageWriteToStream(&image, &stream)) {
        return 8;
    }
    output = fopen(argv[2], "wb");
    if (!output || fwrite(stream.Data, 1, stream.Size, output) != stream.Size) {
        return 9;
    }
    fclose(output);
    P64MemoryStreamDestroy(&stream);
    P64ImageDestroy(&image);
    return 0;
}
