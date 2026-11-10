/* Read-only SuperCard Pro file adapter. Part of VICE; GPL-2.0-or-later.
   Format reference: https://www.cbmstuff.com/downloads/scp/scp_image_specs.txt
   Samples are retained as native intervals, never converted to P64 positions. */
#include "scp-reader.h"
#include <stdlib.h>
#include <string.h>

#define SCP_TRACKS 168U
#define SCP_TABLE_END (16U + SCP_TRACKS * 4U)
#define SCP_MODE_RW 0x10U
#define SCP_FOOTER 0x20U

static uint32_t le32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_at(FILE *file, uint64_t size, uint64_t offset, void *buffer, size_t length)
{
    if (offset > size || length > size - offset) {
        return FLUX_INVALID;
    }
    /* The adapter's 1 GiB input bound makes this representable even where
       standard C fseek/ftell use a 32-bit long. */
    if (fseek(file, (long)offset, SEEK_SET) != 0 || fread(buffer, 1, length, file) != length) {
        return FLUX_IO;
    }
    return FLUX_OK;
}

static int utf8(const uint8_t *s, unsigned int length)
{
    unsigned int i = 0, left;
    uint32_t c, minimum;
    while (i < length) {
        c = s[i++];
        if (c < 0x80) {
            if (!c) { return 0; }
            continue;
        }
        if ((c & 0xe0) == 0xc0) { c &= 0x1f; left = 1; minimum = 0x80; }
        else if ((c & 0xf0) == 0xe0) { c &= 0x0f; left = 2; minimum = 0x800; }
        else if ((c & 0xf8) == 0xf0) { c &= 7; left = 3; minimum = 0x10000; }
        else { return 0; }
        if (left > length - i) { return 0; }
        while (left--) {
            if ((s[i] & 0xc0) != 0x80) { return 0; }
            c = (c << 6) | (s[i++] & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) { return 0; }
    }
    return 1;
}

static int footer_read(FILE *file, uint64_t size, uint64_t track_end, const uint8_t *header)
{
    uint8_t footer[48], prefix[2], *text;
    uint64_t begin;
    uint32_t offset;
    unsigned int i, length;
    int rc;
    if (!(header[8] & SCP_FOOTER)) { return FLUX_OK; }
    if (size < 48 || header[3] != 0) { return FLUX_INVALID; }
    begin = size - 48;
    if (begin < track_end) { return FLUX_INVALID; }
    rc = read_at(file, size, begin, footer, sizeof(footer));
    if (rc != FLUX_OK) { return rc; }
    if (memcmp(footer + 44, "FPCS", 4) || footer[43] < 0x16) { return FLUX_INVALID; }
    /* A future footer prepends fields; validate its known trailing portion. */
    for (i = 0; i < 6; i++) {
        offset = le32(footer + i * 4);
        if (!offset) { continue; }
        if (offset < track_end || (uint64_t)offset + 2 > begin) { return FLUX_INVALID; }
        rc = read_at(file, size, offset, prefix, 2);
        if (rc != FLUX_OK) { return rc; }
        length = prefix[0] | ((unsigned int)prefix[1] << 8);
        if ((uint64_t)offset + 3 + length > begin) { return FLUX_INVALID; }
        text = malloc(length + 1);
        if (!text) { return FLUX_NOMEM; }
        rc = read_at(file, size, (uint64_t)offset + 2, text, length + 1);
        if (rc == FLUX_OK && (text[length] || !utf8(text, length))) { rc = FLUX_INVALID; }
        free(text);
        if (rc != FLUX_OK) { return rc; }
    }
    return FLUX_OK;
}

int scp_file_read(FILE *file, flux_capture_t **capture)
{
    uint8_t header[16], table[SCP_TRACKS * 4], tdh[4 + 255 * 12], buffer[8192];
    uint64_t size, pos, end, track_end = SCP_TABLE_END, durations[255];
    uint32_t checksum = 0, offset, first = UINT32_MAX, count, data_offset;
    unsigned int track, rev, entries = SCP_TRACKS, table_end = SCP_TABLE_END;
    size_t length, i, n, total, written;
    long end_position;
    uint16_t *words = NULL;
    flux_capture_t *result = NULL;
    int rc;
    if (!capture) { return FLUX_INVALID; }
    *capture = NULL;
    if (!file || fseek(file, 0, SEEK_SET) != 0) { return FLUX_IO; }
    length = fread(header, 1, 3, file);
    if (length != 3 || memcmp(header, "SCP", 3)) {
        return ferror(file) ? FLUX_IO : SCP_FILE_NOT_MATCHED;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (end_position = ftell(file)) < 0) { return FLUX_IO; }
    size = (uint64_t)end_position;
    if (size > SCP_FILE_MAX_BYTES) { return FLUX_LIMIT; }
    rc = read_at(file, size, 0, header, sizeof(header));
    if (rc != FLUX_OK) { return rc; }
    if (header[4] != 0 || !header[5] || header[6] > header[7]
        || header[7] >= FLUX_CAPTURE_TRACKS || (header[8] & 0x40)
        || (header[9] != 0 && header[9] != 16) || header[10] > 1
        || ((header[8] & SCP_MODE_RW) && (header[5] != 1 || le32(header + 12)))) {
        return FLUX_INVALID;
    }
    rc = read_at(file, size, 16, table, sizeof(table));
    if (rc != FLUX_OK) { return rc; }
    for (track = header[6]; track <= header[7]; track++) {
        offset = le32(table + track * 4);
        if (offset && offset < first) { first = offset; }
    }
    if (first < SCP_TABLE_END) {
        if (first != 16 + 164 * 4 && first != 16 + 166 * 4) { return FLUX_INVALID; }
        rc = read_at(file, size, first, buffer, 4);
        if (rc != FLUX_OK) { return rc; }
        if (memcmp(buffer, "TRK", 3) || buffer[3] < header[6] || buffer[3] > header[7]
            || le32(table + buffer[3] * 4) != first) { return FLUX_INVALID; }
        table_end = first;
        entries = (first - 16) / 4;
        memset(table + entries * 4, 0, (SCP_TRACKS - entries) * 4);
    }
    track_end = table_end;
    if (!(header[8] & SCP_MODE_RW)) {
        for (pos = 16; pos < size; pos += length) {
            length = (size - pos < sizeof(buffer)) ? (size_t)(size - pos) : sizeof(buffer);
            rc = read_at(file, size, pos, buffer, length);
            if (rc != FLUX_OK) { return rc; }
            for (i = 0; i < length; i++) { checksum += buffer[i]; }
        }
        if (checksum != le32(header + 12)) { return FLUX_INVALID; }
    }
    result = flux_capture_create(header[5], (header[8] & 1) ? FLUX_CAPTURE_INDEXED : 0);
    if (!result) { return FLUX_NOMEM; }
    for (track = 0; track < SCP_TRACKS; track++) {
        offset = le32(table + track * 4);
        if (!offset) { continue; }
        rc = FLUX_INVALID;
        if (track < header[6] || track > header[7] || offset < table_end) { goto fail; }
        rc = read_at(file, size, offset, tdh, 4 + header[5] * 12);
        if (rc != FLUX_OK) { goto fail; }
        rc = FLUX_INVALID;
        if (memcmp(tdh, "TRK", 3) || tdh[3] != track) { goto fail; }
        end = (uint64_t)offset + 4 + header[5] * 12;
        if (end > track_end) { track_end = end; }
        total = 0;
        for (rev = 0; rev < header[5]; rev++) {
            durations[rev] = (uint64_t)le32(tdh + 4 + rev * 12) * 2;
            count = le32(tdh + 8 + rev * 12);
            data_offset = le32(tdh + 12 + rev * 12);
            rc = FLUX_INVALID;
            if (!durations[rev] || data_offset < 4U + header[5] * 12U
                || (uint64_t)offset + data_offset > size
                || (uint64_t)count * 2 > size - offset - data_offset) { goto fail; }
            rc = FLUX_LIMIT;
            if (count > FLUX_CAPTURE_MAX_WORDS - flux_capture_words(result) - total) { goto fail; }
            total += count;
            end = (uint64_t)offset + data_offset + (uint64_t)count * 2;
            if (end > track_end) { track_end = end; }
        }
        words = total ? malloc(total * sizeof(*words)) : NULL;
        if (total && !words) { rc = FLUX_NOMEM; goto fail; }
        written = 0;
        for (rev = 0; rev < header[5]; rev++) {
            count = le32(tdh + 8 + rev * 12);
            pos = (uint64_t)offset + le32(tdh + 12 + rev * 12);
            while (count) {
                n = count < sizeof(buffer) / 2 ? count : sizeof(buffer) / 2;
                rc = read_at(file, size, pos, buffer, n * 2);
                if (rc != FLUX_OK) { goto fail; }
                for (i = 0; i < n; i++) {
                    words[written++] = ((unsigned int)buffer[i * 2] << 8) | buffer[i * 2 + 1];
                }
                pos += n * 2;
                count -= (uint32_t)n;
            }
        }
        rc = flux_capture_add_track(result, track, durations, words, total, 2U * (header[11] + 1U));
        free(words);
        words = NULL;
        if (rc != FLUX_OK) { goto fail; }
    }
    rc = footer_read(file, size, track_end, header);
    if (rc != FLUX_OK) { goto fail; }
    flux_capture_seal(result);
    *capture = result;
    return FLUX_OK;
fail:
    free(words);
    flux_capture_destroy(result);
    return rc;
}
