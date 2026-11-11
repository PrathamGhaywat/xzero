#undef NDEBUG
#include "../src/wallpaper.h"
#include "../src/util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int read_u32(const unsigned char *p) {
    return (unsigned int)p[0] |
           ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

int main(void) {
    char path[XZERO_PATH_MAX];
    char err[512];
    assert(wallpaper_generate_default(path, sizeof(path), err, sizeof(err)));

    FILE *f = fopen(path, "rb");
    assert(f);
    unsigned char header[54];
    assert(fread(header, 1, sizeof(header), f) == sizeof(header));
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    assert(header[0] == 'B' && header[1] == 'M');
    assert(read_u32(header + 18) == 1280);
    assert(read_u32(header + 22) == 720);
    assert(size > 1024*1024);
    remove(path);

    // Nonexistent local images must fail cleanly (and not touch the wallpaper).
    assert(wallpaper_set("/definitely/not/a/real/xzero-wallpaper.bmp", err, sizeof(err)) != 0);
    assert(err[0] != '\0');

    printf("test_wallpaper passed\n");
    return 0;
}
