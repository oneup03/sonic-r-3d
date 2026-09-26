#include <stddef.h>
#include "c3d_swizzle.h"

static inline unsigned morton8(unsigned x, unsigned y)
{
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

void c3d_swizzle16(uint16_t *dst, const uint16_t *src, int w, int h, int flip)
{
    int tilesX = w >> 3;
    for (int yd = 0; yd < h; yd++) {
        int ys = flip ? (h - 1 - yd) : yd;
        const uint16_t *row = src + (size_t)ys * w;
        uint16_t *tileRow = dst + (size_t)(yd >> 3) * tilesX * 64;
        unsigned my = ((yd & 1) << 1) | ((yd & 2) << 2) | ((yd & 4) << 3);
        for (int x = 0; x < w; x++) {
            tileRow[(x >> 3) * 64 + (my | (x & 1) | ((x & 2) << 1) | ((x & 4) << 2))] = row[x];
        }
    }
    (void)morton8;
}

void c3d_swizzle32(uint32_t *dst, const uint32_t *src, int w, int h, int flip)
{
    int tilesX = w >> 3;
    for (int yd = 0; yd < h; yd++) {
        int ys = flip ? (h - 1 - yd) : yd;
        const uint32_t *row = src + (size_t)ys * w;
        uint32_t *tileRow = dst + (size_t)(yd >> 3) * tilesX * 64;
        unsigned my = ((yd & 1) << 1) | ((yd & 2) << 2) | ((yd & 4) << 3);
        for (int x = 0; x < w; x++) {
            tileRow[(x >> 3) * 64 + (my | (x & 1) | ((x & 2) << 1) | ((x & 4) << 2))] = row[x];
        }
    }
}
