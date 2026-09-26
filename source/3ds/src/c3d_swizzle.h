/**
 * c3d_swizzle.h — linear -> PICA200 tiled texture layout.
 *
 * PICA textures are stored as 8x8 tiles, row-major over the image, with the
 * 64 texels inside each tile in Morton (Z) order. The GPU's row 0 is the
 * BOTTOM of the image, so `flip` writes the source top row last; with flip on,
 * GL/D3D-style UVs (v = 0 at the top) sample correctly unchanged.
 */
#ifndef C3D_SWIZZLE_H
#define C3D_SWIZZLE_H

#include <stdint.h>

void c3d_swizzle16(uint16_t *dst, const uint16_t *src, int w, int h, int flip);
void c3d_swizzle32(uint32_t *dst, const uint32_t *src, int w, int h, int flip);

#endif
