/**
 * r_c3d_texture.c — tpage textures for the citro3d backend.
 *
 * Mirrors render_gl.c's GL_* texture functions: the game keeps 16bpp pixel
 * buffers (texture.c, g_tpagePixelBuf[]) and the backend uploads lazily on
 * first bind after a dirty mark. Conversion: R5 G5 pad B5 -> GPU_RGBA5551
 * with the colour key as alpha 0, or GPU_RGB565 for no-key pages (env maps,
 * wallpaper, sky). Full RGBA8 sources (tinted wallpaper) go up as GPU_RGBA8.
 *
 * Every upload allocates a fresh linear buffer and the previous one is freed
 * two frames later: the GPU may still be reading it for the frame in flight.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r_c3d_internal.h"
#include "c3d_swizzle.h"
#include "sonicr_types.h"
#include "sonicr_globals.h"

typedef struct {
    C3D_Tex tex;
    int inited;
    int w, h;
    GPU_TEXCOLOR fmt;
    int dirty;
    int noColorKey;
    int green6;
    int keepPixels;
    unsigned char *pendingRGBA;
    int pendingW, pendingH;
    unsigned char *rgba8;
} C3dTpage;

/* Textures in VRAM read faster than from FCRAM and free the memory bus for
 * the CPU; fall back to the linear heap when VRAM is full. */
#define RC3D_TEX_VRAM 1

static C3dTpage s_tp[RC3D_TPAGES];
static int s_inited = 0;
int g_rc3dUploadCount = 0;

/* conversion staging: 512x512 x 32-bit worst case (sky / RGBA8 wallpaper) */
static uint32_t *s_stage = NULL;
static uint32_t *s_swz = NULL;
#define STAGE_PIXELS (512 * 512)

/* deferred deletes */
#define DEFER_MAX 96
static C3D_Tex s_defer[DEFER_MAX];
static int s_deferAge[DEFER_MAX];
static int s_deferCount = 0;

static void defer_delete(C3D_Tex *t)
{
    if (s_deferCount >= DEFER_MAX) {
        /* ring is full: the oldest entry has been out of use for many frames */
        C3D_TexDelete(&s_defer[0]);
        memmove(&s_defer[0], &s_defer[1], sizeof(C3D_Tex) * (DEFER_MAX - 1));
        memmove(&s_deferAge[0], &s_deferAge[1], sizeof(int) * (DEFER_MAX - 1));
        s_deferCount--;
    }
    s_defer[s_deferCount] = *t;
    s_deferAge[s_deferCount] = 0;
    s_deferCount++;
}

void RC3D_TexEndFrame(void)
{
    int keep = 0;
    for (int i = 0; i < s_deferCount; i++) {
        if (++s_deferAge[i] >= 3) {
            C3D_TexDelete(&s_defer[i]);
        } else {
            s_defer[keep] = s_defer[i];
            s_deferAge[keep] = s_deferAge[i];
            keep++;
        }
    }
    s_deferCount = keep;
}

void RC3D_TexInit(void)
{
    if (s_inited) return;
    memset(s_tp, 0, sizeof(s_tp));
    for (int i = 0; i < RC3D_TPAGES; i++) {
        s_tp[i].dirty = 1;
    }
    /* Linear: VRAM uploads go through the GX copy engine, which only reads
     * linear memory. The stage buffer is CPU-only and can stay on the heap. */
    s_stage = (uint32_t *)malloc(STAGE_PIXELS * 4);
    s_swz   = (uint32_t *)linearAlloc(STAGE_PIXELS * 4);
    s_inited = 1;
}

static void tp_dims(int tpage, int *w, int *h)
{
    int tw = g_tpageWidth[tpage], th = g_tpageHeight[tpage];
    *w = (tw > 0) ? tw : 256;
    *h = (th > 0) ? th : 256;
}

/* (Re)create the C3D_Tex for a page with the given size/format and hand the
 * old storage to the deferred list. */
static int tp_realloc(C3dTpage *t, int w, int h, GPU_TEXCOLOR fmt)
{
    if (t->inited) {
        defer_delete(&t->tex);
        t->inited = 0;
    }
    memset(&t->tex, 0, sizeof(t->tex));
    int ok = 0;
#if RC3D_TEX_VRAM
    ok = C3D_TexInitVRAM(&t->tex, (u16)w, (u16)h, fmt);
#endif
    if (!ok) {
        memset(&t->tex, 0, sizeof(t->tex));
        ok = C3D_TexInit(&t->tex, (u16)w, (u16)h, fmt);
    }
    if (!ok) {
        fprintf(stderr, "c3d: texture %dx%d fmt %d failed (linear free %u KB)\n",
                w, h, (int)fmt, (unsigned)(linearSpaceFree() / 1024));
        return 0;
    }
    C3D_TexSetWrap(&t->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetFilter(&t->tex, GPU_NEAREST, GPU_NEAREST);
    t->inited = 1;
    t->w = w; t->h = h; t->fmt = fmt;
    return 1;
}

static void upload_rgba8(C3dTpage *t, const unsigned char *rgba, int w, int h)
{
    if (w * h > STAGE_PIXELS) return;
    const uint32_t *src = (const uint32_t *)rgba;
    for (int i = 0; i < w * h; i++) {
        uint32_t c = src[i];   /* bytes R,G,B,A -> LE u32 0xAABBGGRR; PICA wants 0xRRGGBBAA */
        s_stage[i] = ((c & 0xFF) << 24) | (((c >> 8) & 0xFF) << 16) | (((c >> 16) & 0xFF) << 8) | (c >> 24);
    }
    if (!tp_realloc(t, w, h, GPU_RGBA8)) return;
    c3d_swizzle32(s_swz, s_stage, w, h, RC3D_TEX_FLIP);
    /* A VRAM texture is filled by the GX copy engine straight from s_swz:
     * push the CPU's cached lines out first or it copies stale texels. */
    GSPGPU_FlushDataCache(s_swz, (u32)w * (u32)h * 4);
    C3D_TexUpload(&t->tex, s_swz);
}

void RC3D_TexUpload(int tpage)
{
    if (tpage < 0 || tpage >= RC3D_TPAGES) return;
    RC3D_TexInit();
    g_rc3dUploadCount++;
    C3dTpage *t = &s_tp[tpage];
    int w, h;
    tp_dims(tpage, &w, &h);

    if (t->rgba8) {
        upload_rgba8(t, t->rgba8, w, h);
        t->dirty = 0;
        return;
    }
    if (t->pendingRGBA) {
        upload_rgba8(t, t->pendingRGBA, t->pendingW, t->pendingH);
        free(t->pendingRGBA);
        t->pendingRGBA = NULL;
        t->dirty = 0;
        return;
    }
    const unsigned short *pixels = (const unsigned short *)g_tpagePixelBuf[tpage];
    if (pixels == NULL) return;
    if (w * h > STAGE_PIXELS) return;

    uint16_t *out = (uint16_t *)s_stage;
    int count = w * h;
    if (t->noColorKey) {
        int green6 = t->green6;
        for (int i = 0; i < count; i++) {
            unsigned short p = pixels[i];
            unsigned r5 = (p >> 11) & 0x1F, b5 = p & 0x1F, g6;
            if (green6) {
                g6 = (p >> 5) & 0x3F;
            } else {
                unsigned g5 = (p >> 6) & 0x1F;
                g6 = (g5 << 1) | (g5 >> 4);
            }
            out[i] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
        if (!tp_realloc(t, w, h, GPU_RGB565)) return;
    } else {
        for (int i = 0; i < count; i++) {
            unsigned short p = pixels[i];
            unsigned r5 = (p >> 11) & 0x1F, g5 = (p >> 6) & 0x1F, b5 = p & 0x1F;
            unsigned a1 = IS_COLOR_KEY_RGB5(r5, g5, b5) ? 0 : 1;
            out[i] = (uint16_t)((r5 << 11) | (g5 << 6) | (b5 << 1) | a1);
        }
        if (!tp_realloc(t, w, h, GPU_RGBA5551)) return;
    }
    c3d_swizzle16((uint16_t *)s_swz, out, w, h, RC3D_TEX_FLIP);
    GSPGPU_FlushDataCache(s_swz, (u32)w * (u32)h * 2);   /* see upload_rgba8 */
    C3D_TexUpload(&t->tex, s_swz);
    t->dirty = 0;
}

void RC3D_TexEnsureUploaded(int tpage)
{
    if (tpage < 0 || tpage >= RC3D_TPAGES) return;
    if (s_tp[tpage].dirty) {
        RC3D_TexUpload(tpage);
    }
}

C3D_Tex *RC3D_TexBindable(int tpage)
{
    if (tpage < 0 || tpage >= RC3D_TPAGES) return NULL;
    C3dTpage *t = &s_tp[tpage];
    if (t->dirty) {
        RC3D_TexUpload(tpage);
    }
    return t->inited ? &t->tex : NULL;
}

void RC3D_TexMarkDirty(int tpage)
{
    if (tpage >= 0 && tpage < RC3D_TPAGES) s_tp[tpage].dirty = 1;
}

void RC3D_TexMarkAllDirty(void)
{
    for (int i = 0; i < RC3D_TPAGES; i++) {
        if (g_tpagePixelBuf[i] != NULL || s_tp[i].rgba8 || s_tp[i].pendingRGBA) {
            s_tp[i].dirty = 1;
        }
    }
}

void RC3D_TexClearDirty(int tpage)
{
    if (tpage >= 0 && tpage < RC3D_TPAGES) s_tp[tpage].dirty = 0;
}

void RC3D_TexFreeSlot(int tpage)
{
    if (tpage < 0 || tpage >= RC3D_TPAGES) return;
    C3dTpage *t = &s_tp[tpage];
    if (t->inited) {
        defer_delete(&t->tex);
        t->inited = 0;
    }
    if (t->pendingRGBA) {
        free(t->pendingRGBA);
        t->pendingRGBA = NULL;
    }
    t->dirty = 1;
}

void RC3D_TexUploadRGBA(int tpage, unsigned char *rgba, int w, int h)
{
    if (tpage < 0 || tpage >= RC3D_TPAGES) return;
    RC3D_TexInit();
    upload_rgba8(&s_tp[tpage], rgba, w, h);
    s_tp[tpage].dirty = 0;
}

/* Sub-rect patches (minimap and env-map tiles into the frozen ICON01 page).
 *
 * Only the rect may change: the CPU buffer also carries content that was
 * deliberately never uploaded (the weather tiles LoadTextureSubRect writes
 * over rows 224+, on top of the HUD faces), so a whole-page re-upload is
 * wrong. Copy the resident tiled image into a fresh buffer, re-tile just the
 * rect's texels from the CPU buffer, and swap; the old buffer is freed once
 * the GPU is done with it. */
void RC3D_TexSubRect(int tpage, int x, int y, int w, int h)
{
    if (tpage < 0 || tpage >= RC3D_TPAGES) return;
    C3dTpage *t = &s_tp[tpage];
    if (t->rgba8 || t->pendingRGBA) return;
    const unsigned short *pixels = (const unsigned short *)g_tpagePixelBuf[tpage];
    if (pixels == NULL) return;
    if (!t->inited || (t->fmt != GPU_RGBA5551 && t->fmt != GPU_RGB565)) {
        RC3D_TexUpload(tpage);
        return;
    }
    int tw = t->w, th = t->h;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > tw) w = tw - x;
    if (y + h > th) h = th - y;
    if (w <= 0 || h <= 0) return;

    C3D_Tex old = t->tex;
    t->inited = 0;
    memset(&t->tex, 0, sizeof(t->tex));
    /* Linear on purpose: the CPU patches this copy in place. */
    if (!C3D_TexInit(&t->tex, (u16)tw, (u16)th, t->fmt)) {
        t->tex = old;
        t->inited = 1;
        return;
    }
    C3D_TexSetWrap(&t->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetFilter(&t->tex, GPU_NEAREST, GPU_NEAREST);
    memcpy(t->tex.data, old.data, old.size);
    t->inited = 1;
    defer_delete(&old);

    uint16_t *dst = (uint16_t *)t->tex.data;
    const int tilesX = tw >> 3;
    const int keyed = (t->fmt == GPU_RGBA5551);
    const int green6 = t->green6;
    for (int yy = y; yy < y + h; yy++) {
        int yd = RC3D_TEX_FLIP ? (th - 1 - yy) : yy;
        uint16_t *tileRow = dst + (size_t)(yd >> 3) * tilesX * 64;
        unsigned my = ((yd & 1) << 1) | ((yd & 2) << 2) | ((yd & 4) << 3);
        for (int xx = x; xx < x + w; xx++) {
            unsigned short p = pixels[yy * tw + xx];
            unsigned r5 = (p >> 11) & 0x1F, b5 = p & 0x1F;
            uint16_t o;
            if (keyed) {
                unsigned g5 = (p >> 6) & 0x1F;
                o = (uint16_t)((r5 << 11) | (g5 << 6) | (b5 << 1) | (IS_COLOR_KEY_RGB5(r5, g5, b5) ? 0 : 1));
            } else {
                unsigned g6 = green6 ? ((p >> 5) & 0x3F) : ((((p >> 6) & 0x1F) << 1) | (((p >> 6) & 0x1F) >> 4));
                o = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
            }
            tileRow[(xx >> 3) * 64 + (my | (xx & 1) | ((xx & 2) << 1) | ((xx & 4) << 2))] = o;
        }
    }
    /* C3D_TexUpload is a memcpy + flush; we wrote in place, so just flush. */
    GSPGPU_FlushDataCache(t->tex.data, t->tex.size);
    t->dirty = 0;
}

void RC3D_TexSetPendingRGBA(int tpage, unsigned char *rgba, int w, int h)
{
    if (tpage < 0 || tpage >= RC3D_TPAGES) { free(rgba); return; }
    C3dTpage *t = &s_tp[tpage];
    if (t->pendingRGBA) free(t->pendingRGBA);
    t->pendingRGBA = rgba;
    t->pendingW = w;
    t->pendingH = h;
    t->dirty = 1;
}

void RC3D_TexSetRGBA8(int tpage, unsigned char *rgba)
{
    if (tpage < 0 || tpage >= RC3D_TPAGES) return;
    C3dTpage *t = &s_tp[tpage];
    if (t->rgba8 && t->rgba8 != rgba) free(t->rgba8);
    t->rgba8 = rgba;
    t->dirty = 1;
}

void RC3D_TexSetNoColorKey(int tpage, int on)
{
    if (tpage < 0 || tpage >= RC3D_TPAGES) return;
    s_tp[tpage].noColorKey = on ? 1 : 0;
    s_tp[tpage].dirty = 1;
}

void RC3D_TexSetGreen6(int tpage, int on)
{
    if (tpage < 0 || tpage >= RC3D_TPAGES) return;
    s_tp[tpage].green6 = on ? 1 : 0;
}

void RC3D_TexKeepPixels(int tpage)
{
    if (tpage < 0 || tpage >= RC3D_TPAGES) return;
    s_tp[tpage].keepPixels = 1;
}
