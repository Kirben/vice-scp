/* Native SCP staging writer. Part of VICE; GPL-2.0-or-later. */
#include "scp-writer.h"
#include "scp-reader.h"

#include <string.h>

#define SCP_TABLE_END (16U + 168U * 4U)
#define SCP_TRACK_HEADER(revs) (4U + 12U * (revs))
#define SCP_MODE_RW 0x10U
#define SCP_FLUX_CREATOR 0x80U

static void put_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

/* Reserve the existing words plus one transition per fastest 1541 bit
   cell (52 reference clocks). TDH length only counts used words. */
static uint64_t reserved_words(const flux_track_t *track)
{
    return flux_track_word_count(track) + flux_track_duration(track, 0) / 260 + 1;
}

static int write_track(FILE *file, const flux_track_t *track, unsigned int halftrack,
                       unsigned int revolutions)
{
    uint8_t record[SCP_TRACK_HEADER(FLUX_CAPTURE_MAX_REVS)] = {'T', 'R', 'K', (uint8_t)halftrack};
    uint8_t buffer[8192];
    const uint16_t *words = flux_track_words(track);
    const uint32_t *window_words = flux_track_window_words(track);
    size_t used = flux_track_word_count(track);
    uint64_t capacity = revolutions == 1 ? reserved_words(track) : used;
    uint64_t time = 0;
    size_t offset = 0;
    unsigned int rev;
    unsigned int header_size = SCP_TRACK_HEADER(revolutions);

    for (rev = 0; rev < revolutions; ++rev) {
        uint8_t *entry = record + 4 + rev * 12;
        size_t start = offset;
        uint64_t boundary = flux_track_boundary(track, rev + 1);
        if (window_words) {
            offset += window_words[rev];
        } else {
            while (offset < used) {
                uint64_t edge = time + (uint64_t)(words[offset] ? words[offset] : 65536U)
                                       * flux_track_unit_ticks(track);
                if (rev + 1 < revolutions && edge > boundary) {
                    break;
                }
                time = edge;
                ++offset;
            }
        }
        put_le32(entry, (uint32_t)(flux_track_duration(track, rev) / 2));
        put_le32(entry + 4, (uint32_t)(offset - start));
        put_le32(entry + 8, header_size + (uint32_t)(start * 2));
    }
    offset = 0;
    if (fwrite(record, 1, header_size, file) != header_size) {
        return FLUX_IO;
    }
    while (offset < used) {
        size_t i;
        size_t count = used - offset;
        if (count > sizeof(buffer) / 2) {
            count = sizeof(buffer) / 2;
        }
        for (i = 0; i < count; ++i) {
            uint16_t word = words[offset + i];
            buffer[i * 2] = (uint8_t)(word >> 8);
            buffer[i * 2 + 1] = (uint8_t)word;
        }
        if (fwrite(buffer, 2, count, file) != count) {
            return FLUX_IO;
        }
        offset += count;
    }
    memset(buffer, 0, sizeof(buffer));
    capacity -= used;
    while (capacity) {
        size_t count = capacity > sizeof(buffer) / 2
            ? sizeof(buffer) / 2 : (size_t)capacity;
        if (fwrite(buffer, 2, count, file) != count) {
            return FLUX_IO;
        }
        capacity -= count;
    }
    return FLUX_OK;
}

/* A 1571 full-track SCP alternates physical heads in the track table. */
static unsigned int capture_slot(unsigned int slot, int double_sided)
{
    return double_sided ? (slot / 2) * 2 + (slot & 1) * FLUX_CAPTURE_TRACKS : slot;
}

static int write_checksum(FILE *file)
{
    uint8_t bytes[8192];
    uint32_t checksum = 0;
    size_t count;
    size_t i;

    if (fseek(file, 16, SEEK_SET)) {
        return FLUX_IO;
    }
    while ((count = fread(bytes, 1, sizeof(bytes), file)) != 0) {
        for (i = 0; i < count; ++i) {
            checksum += bytes[i];
        }
    }
    if (ferror(file) || fseek(file, 12, SEEK_SET)) {
        return FLUX_IO;
    }
    put_le32(bytes, checksum);
    return fwrite(bytes, 1, 4, file) == 4 ? FLUX_OK : FLUX_IO;
}

int scp_file_write_staged_wrsp(FILE *file, const flux_capture_t *capture, const uint8_t *extension,
                               const uint8_t *track_range)
{
    uint8_t header[SCP_TABLE_END] = {'S', 'C', 'P', 0x25, 0, 1};
    uint64_t offset = sizeof(header) + (extension ? SCP_WRSP_EXTENSION_SIZE : 0);
    uint32_t units = 0;
    unsigned int ht;
    unsigned int flags;
    unsigned int slots;
    unsigned int rev;
    unsigned int revolutions;
    int double_sided;
    int wide;
    long file_size;

    if (!file || !flux_capture_is_sealed(capture)) {
        return FLUX_INVALID;
    }
    revolutions = flux_capture_revolutions(capture);
    header[5] = (uint8_t)revolutions;
    flags = flux_capture_flags(capture);
    wide = !!(flags & (FLUX_CAPTURE_8X50 | FLUX_CAPTURE_MFM));
    double_sided = !wide && (flags & FLUX_CAPTURE_DOUBLE_SIDED) && flux_capture_has_second_bank(capture);
    /* Both layouts use at most 84 entries, with either half-tracks or heads. */
    slots = wide ? FLUX_CAPTURE_MAX_TRACKS : FLUX_CAPTURE_TRACKS;
    if (flags & FLUX_CAPTURE_TOP_ONLY) {
        header[10] = 2;
        for (ht = 0; ht < FLUX_CAPTURE_MAX_TRACKS; ++ht) {
            if (flux_capture_track(capture, ht) && !flux_capture_slot_writable(capture, ht)) {
                return FLUX_INVALID;
            }
        }
    }
    if (double_sided) {
        /* Interleaved SCP slots represent full tracks on each head. */
        for (ht = 1; ht < FLUX_CAPTURE_MAX_TRACKS; ht += 2) {
            if (flux_capture_track(capture, ht)) {
                return FLUX_INVALID;
            }
        }
    }
    header[8] = (revolutions == 1 ? SCP_MODE_RW : 0) | SCP_FLUX_CREATOR
        | ((flags & FLUX_CAPTURE_INDEXED) ? 1 : 0)
        | ((flags & FLUX_CAPTURE_360_RPM) ? 4 : 0);
    for (ht = 0; ht < slots; ++ht) {
        const flux_track_t *track = flux_capture_track(capture, capture_slot(ht, double_sided));
        uint32_t track_units;
        uint64_t duration;
        uint64_t length;
        if (!track) {
            continue;
        }
        track_units = flux_track_unit_ticks(track);
        if (track_units < 2 || track_units > 512 || (track_units & 1)
            || (units && units != track_units)) {
            return FLUX_RANGE;
        }
        for (rev = 0; rev < revolutions; ++rev) {
            duration = flux_track_duration(track, rev);
            if ((duration & 1) || duration / 2 > UINT32_MAX) {
                return FLUX_RANGE;
            }
        }
        if (!units) {
            header[6] = (uint8_t)ht;
            units = track_units;
        }
        header[7] = (uint8_t)ht;
        length = SCP_TRACK_HEADER(revolutions) + 2 * (revolutions == 1
            ? reserved_words(track) : flux_track_word_count(track));
        if (length > SCP_FILE_MAX_BYTES - offset) {
            return FLUX_LIMIT;
        }
        put_le32(header + 16 + ht * 4, (uint32_t)offset);
        offset += length;
    }
    if (units) {
        header[11] = (uint8_t)(units / 2 - 1);
    }
    if (track_range) {
        /* Preserve declared empty slots, expanding the range for new tracks. */
        if (!units || track_range[0] < header[6]) {
            header[6] = track_range[0];
        }
        if (track_range[1] > header[7]) {
            header[7] = track_range[1];
        }
    }

    /* Guard against accidental use on an existing image before any write. */
    if (fseek(file, 0, SEEK_END)) {
        return FLUX_IO;
    }
    file_size = ftell(file);
    if (file_size < 0) {
        return FLUX_IO;
    }
    if (file_size != 0) {
        return FLUX_INVALID;
    }
    if (fwrite(header, 1, sizeof(header), file) != sizeof(header)
        || (extension && fwrite(extension, 1, SCP_WRSP_EXTENSION_SIZE, file) != SCP_WRSP_EXTENSION_SIZE)) {
        return FLUX_IO;
    }
    for (ht = 0; ht < slots; ++ht) {
        const flux_track_t *track = flux_capture_track(capture, capture_slot(ht, double_sided));
        if (track && write_track(file, track, ht, revolutions) != FLUX_OK) {
            return FLUX_IO;
        }
    }
    if (revolutions > 1 && write_checksum(file) != FLUX_OK) {
        return FLUX_IO;
    }
    return fflush(file) ? FLUX_IO : FLUX_OK;
}

int scp_file_write_staged(FILE *file, const flux_capture_t *capture)
{
    return scp_file_write_staged_wrsp(file, capture, NULL, NULL);
}

int scp_file_footer_writable(const uint8_t *trailer, size_t size, uint64_t original_offset)
{
    const uint8_t *footer;
    uint32_t app;
    size_t prefix;
    size_t length;
    if (size < 48) {
        return 0;
    }
    prefix = size - 48;
    footer = trailer + prefix;
    if (memcmp(footer + 44, "FPCS", 4)) {
        return 0;
    }
    if (footer[43] == 0x16) {
        return 1;
    }

    /* Greaseweazle labels the original 48-byte layout as revision 2.4.
       Accept its exact trailer shape so unknown extension fields are safe. */
    if (footer[43] != 0x24 || prefix < 3 || memcmp(footer, (uint8_t[16]){0}, 16)
        || memcmp(footer + 20, (uint8_t[4]){0}, 4)
        || memcmp(footer + 40, (uint8_t[3]){0}, 3)) {
        return 0;
    }
    app = (uint32_t)footer[16] | ((uint32_t)footer[17] << 8)
        | ((uint32_t)footer[18] << 16) | ((uint32_t)footer[19] << 24);
    length = trailer[0] | ((unsigned int)trailer[1] << 8);
    return app == original_offset && length + 3 == prefix && length >= 13
        && !memcmp(trailer + 2, "Greaseweazle ", 13);
}

int scp_file_append_footer(FILE *file, const uint8_t *trailer, size_t size, uint64_t original_offset)
{
    uint8_t footer[48];
    unsigned int i;
    int flags;
    long position;
    size_t prefix;

    if (size < sizeof(footer) || fseek(file, 0, SEEK_END)
        || (position = ftell(file)) < 0) {
        return FLUX_IO;
    }
    if (size > SCP_FILE_MAX_BYTES - (uint64_t)position) {
        return FLUX_LIMIT;
    }
    prefix = size - sizeof(footer);
    memcpy(footer, trailer + prefix, sizeof(footer));
    if (!scp_file_footer_writable(trailer, size, original_offset)) {
        return FLUX_INVALID;
    }
    for (i = 0; i < 24; i += 4) {
        const uint8_t *p = footer + i;
        uint32_t offset = p[0] | ((uint32_t)p[1] << 8)
            | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        if (!offset) {
            continue;
        }
        if (offset < original_offset || offset - original_offset >= prefix) {
            return FLUX_INVALID;
        }
        put_le32(footer + i, (uint32_t)(position + offset - original_offset));
    }
    if (fwrite(trailer, 1, prefix, file) != prefix
        || fwrite(footer, 1, sizeof(footer), file) != sizeof(footer)
        || fseek(file, 8, SEEK_SET) || (flags = fgetc(file)) == EOF
        || fseek(file, 3, SEEK_SET) || fputc(0, file) == EOF
        || fseek(file, 8, SEEK_SET) || fputc(flags | 0x20, file) == EOF) {
        return FLUX_IO;
    }
    return flags & SCP_MODE_RW ? FLUX_OK : write_checksum(file);
}
