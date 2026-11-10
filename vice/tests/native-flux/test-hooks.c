/* Fault injection and Python bridge for the production native flux modules. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "scp-reader.h"
#ifdef _WIN32
#include <windows.h>
#endif

typedef union allocation_u {
    long double alignment;
    void *pointer_alignment;
    size_t size;
} allocation_t;
static int remaining = -1, live = 0, reads = -1, seeks = -1;
static long file_size = -2;

void native_test_reset(void)
{
    remaining = reads = seeks = -1;
    file_size = -2;
}
void native_test_fail_alloc(int count) { remaining = count; }
void native_test_fail_read(int count) { reads = count; }
void native_test_fail_seek(int count) { seeks = count; }
void native_test_file_size(long size) { file_size = size; }
int native_test_live(void) { return live; }

void *native_test_malloc(size_t size)
{
    allocation_t *p;
    if (remaining == 0 || size > SIZE_MAX - sizeof(*p)) { return NULL; }
    if (remaining > 0) { remaining--; }
    p = malloc(sizeof(*p) + size);
    if (!p) { return NULL; }
    p->size = size;
    live++;
    return p + 1;
}
void *native_test_calloc(size_t count, size_t size)
{
    void *p;
    if (size && count > SIZE_MAX / size) { return NULL; }
    p = native_test_malloc(count * size);
    if (p) { memset(p, 0, count * size); }
    return p;
}
void native_test_free(void *p)
{
    if (p) {
        live--;
        free((allocation_t *)p - 1);
    }
}
size_t native_test_fread(void *p, size_t size, size_t count, FILE *f)
{
    if (reads == 0) { return 0; }
    if (reads > 0) { reads--; }
    return fread(p, size, count, f);
}
int native_test_fseek(FILE *f, long offset, int whence)
{
    if (seeks == 0) { return -1; }
    if (seeks > 0) { seeks--; }
    return fseek(f, offset, whence);
}
long native_test_ftell(FILE *f)
{
    return file_size == -2 ? ftell(f) : file_size;
}

int native_test_read(const char *path, flux_capture_t **capture)
{
    FILE *file;
    int rc;
#ifdef _WIN32
    wchar_t wide[32768];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 32768)) {
        return FLUX_IO;
    }
    file = _wfopen(wide, L"rb");
#else
    file = fopen(path, "rb");
#endif
    if (!file) { return FLUX_IO; }
    rc = scp_file_read(file, capture);
    fclose(file);
    return rc;
}
