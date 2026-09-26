/**
 * render_c3d.c — game-facing frame functions for the 3DS, the counterpart of
 * sdl/src/render_gl.c and dc/src/render_pvr_glue.c: BeginFrame / EndFrame /
 * FlipD3D, the viewport config copy, the tpage state machine, the background
 * clear and the menu wallpaper.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r_c3d_internal.h"
#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "sonicr_functions.h"
#include "vertex_struct.h"
#include "r_state.h"
#include "r_draw.h"
#include "r_compose.h"
#include "stereo.h"
#include "aspect.h"
#include "platform.h"
#include "net_transport.h"

void ApplyViewportGeometry(void);

extern int g_clipLeftDouble;
extern int g_vpClipLeft10;
extern int g_vpClipRight10;
extern int g_vpClipLeft16;
extern int g_vpClipRight16;
extern int g_vpParam0F;
extern int g_vpParam10;

/* Backing size of one frame: the whole top screen. Scissor rects arrive in
 * these units (hud_full.c) with a GL bottom-left origin. */
int g_glBackingWidth = 400;
int g_glBackingHeight = 240;
int g_glViewportOffsetX = 0;
int g_glViewportOffsetY = 0;

/* True horizontal bounds, before the stereo cull margin widens g_clipLeft /
 * g_clipRight. See render_gl.c. */
int g_clipLeftTrue = 0;
int g_clipRightTrue = 639;

static int s_tpageKeepPixels[RC3D_TPAGES];

/**
 * BeginFrame — port of render_gl.c:436. Runs several times per presented
 * frame; the present (FlipD3D) is the real boundary.
 */
static int s_depthCleared = 0;   /* once per presented frame (FlipD3D resets) */

void BeginFrame(void)
{
    /* render_gl.c zeroes every vertex depth on each BeginFrame, which runs
     * two or three times per presented frame: 2 MB of strided writes each
     * time. Once per present is enough — nothing in between reads a vertex
     * it did not just transform. */
    if (g_vertexArrayBase && !s_depthCleared) {
        SrcVertex *v = g_vertexArrayBase;
        for (int i = 0; i < 32768; i++) {
            v->depth = 0;
            v++;
        }
        s_depthCleared = 1;
    }

    RC3D_TexInit();
    R_StereoBeginFrame();

    int fullW, fullH;
    platform_get_drawable_size(&fullW, &fullH);
    int areaW, areaH;
    stereoEyeViewport(fullW, fullH, &areaW, &areaH);

    AspectUpdate(areaW, areaH);
    const float aspect = AspectEffective();
    if (AspectConsumeChange()) {
        ApplyViewportGeometry();
    }

    int vpW, vpH;
    if ((float)areaW > (float)areaH * aspect) {
        vpH = areaH;
        vpW = (int)((float)areaH * aspect + 0.5f);
    } else {
        vpW = areaW;
        vpH = (int)((float)areaW / aspect + 0.5f);
    }
    g_glViewportOffsetX = (areaW - vpW) / 2;
    g_glViewportOffsetY = (areaH - vpH) / 2;
    g_glBackingWidth = vpW;
    g_glBackingHeight = vpH;

    R_ResetState();
}

void EndFrame(void)
{
}

/* The single present point (see render_gl.c). */
void FlipD3D(void)
{
    R_StereoComposeFrame();
    s_depthCleared = 0;
}

/* Copy of render_gl.c:609 — keeps the stereo cull margin and the true bounds. */
void SetViewportFromConfig(int *config)
{
    if (config == NULL) {
        return;
    }
    int clipMargin = 0;
    {
        float maxShift = stereoMaxShiftNdc();
        if (maxShift > 0.0f) {
            clipMargin = (int)(maxShift * (float)g_screenWidth * 0.5f) + 1;
        }
    }

    g_clipLeftTrue  = config[0];
    g_clipRightTrue = config[2];

    g_clipLeft = config[0] - clipMargin;
    g_clipLeftDouble = g_clipLeft * 2;
    g_clipTop = config[1];
    g_clipRight = config[2] + clipMargin;
    g_clipBottom = config[3];
    g_projScaleX = config[4];
    g_projScaleXCurrent = config[5];
    g_projScaleY = config[6];
    g_screenCenterX = config[7];
    g_screenCenterY = config[8];
    g_screenWidthFull = config[9];
    g_screenHeightFull = config[0xa];
    g_vpClipLeft10 = config[0xb];
    g_vpClipRight10 = config[0xc];
    g_vpClipLeft16 = config[0xd];
    g_vpClipRight16 = config[0xe];
    g_vpParam0F = config[0xf];
    g_vpParam10 = config[0x10];
}

/**
 * ProcessTpageStates — port of render_gl.c:709. The GL port clears colour and
 * depth here (the engine relies on these call sites for the depth clear), so
 * record the same two clears for every target.
 */
void ProcessTpageStates(void)
{
    int changed;

    R_DisableScissor();
    R_SetDepthWrite(1);
    R_ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    R_ClearDepth();
    R_SetScissor(g_glViewportOffsetX, g_glViewportOffsetY, g_glBackingWidth, g_glBackingHeight);

    do {
        changed = 0;
        for (int i = 0; i < 0x34; i++) {
            unsigned char state = (unsigned char)g_tpageStateArray[i];
            if (state > 7) {
                continue;
            }
            switch (state) {
                case 6:
                case 7:
                    if (state == 6) {
                        if (g_tpagePixelBuf[i] != NULL) {
                            free(g_tpagePixelBuf[i]);
                            g_tpagePixelBuf[i] = NULL;
                        }
                        RC3D_TexFreeSlot(i);
                    }
                    g_tpageStateArray[i] = 0;
                    break;
                case 1:
                case 2:
                    g_tpageStateArray[i] = 3;
                    changed = 1;
                    break;
                case 3:
                    g_tpageStateArray[i] = 4;
                    changed = 1;
                    break;
                case 5:
                    g_tpageStateArray[i] = 4;
                    break;
                default:
                    break;
            }
        }
    } while (changed);
}

void CleanupD3DTPages(void)
{
    for (int i = 0; i < 0x34; i++) {
        char state = g_tpageStateArray[i];
        if (state == 0 || state == 5 || i == TPAGE_PLATFORM_ICONS) {
            continue;
        }
        g_tpageStateArray[i] = 6;
    }
    ProcessTpageStates();
}

void RenderBackground(void)
{
    R_SetScissor(g_glViewportOffsetX, g_glViewportOffsetY, g_glBackingWidth, g_glBackingHeight);
    R_ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    R_DisableScissor();
    R_FlushState();
}

/* Software twin of the menu wallpaper wave (render_gl.c:1065). */
void RenderWavingMenuBackground(void)
{
    int tpage = g_uiTexPage;
    if (tpage < 0 || tpage >= 52) {
        return;
    }
    if (g_tpageStateArray[tpage] != 4) {
        return;
    }
    if (g_tpagePixelBuf[tpage] == NULL) {
        return;
    }

    struct IntVert { int sx, sy, uvU, uvV; };
    struct IntVert buf[8][8];

    int sinAngle = (g_totalFrames & 0x7F) << 5;
    int yWorld   = 0x483;
    int uvV      = 0x8000;

    for (int row = 0; row < 8; row++) {
        int vertAngle = sinAngle;
        int xWorld    = -0x604;
        int uvU       = 0x8000;
        for (int col = 0; col < 8; col++) {
            int depth = 0x8CA - (g_sinTable[vertAngle] >> 7);
            buf[row][col].sx = g_screenCenterX + (g_projScaleXCurrent * xWorld) / depth;
            buf[row][col].sy = g_screenCenterY - (g_projScaleY * yWorld) / depth;
            buf[row][col].uvU = uvU;
            buf[row][col].uvV = uvV;
            vertAngle = (vertAngle - 0x14D) & 0xFFF;
            xWorld += 0x1B8;
            uvU += 0x246DB6;
        }
        sinAngle = (sinAngle - 0xDE) & 0xFFF;
        yWorld -= 0x14A;
        uvV += 0x246DB6;
    }

    R_PushState();
    R_SetTexture(tpage);
    R_SetTexEnv(R_TEXENV_MODULATE);
    R_SetBlendMode(R_BLEND_ALPHA);
    R_FlushState();

    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 7; col++) {
            const struct IntVert *src[4] = {
                &buf[row][col], &buf[row][col + 1], &buf[row + 1][col + 1], &buf[row + 1][col]
            };
            RenderVertex rv[4];
            for (int k = 0; k < 4; k++) {
                rv[k].sx = (float)src[k]->sx;
                rv[k].sy = (float)src[k]->sy;
                rv[k].sz = 0.5f;
                rv[k].rhw = 1.0f;
                rv[k].color = VERTEX_WHITE;
                rv[k].specular = 0;
                rv[k].u = g_uvLUT256[src[k]->uvU >> 16];
                rv[k].v = g_uvLUT256[src[k]->uvV >> 16];
            }
            R_DrawQuad(rv);
        }
    }
    R_PopState();
}

void GL_KeepPixels(int tpage)
{
    if (tpage >= 0 && tpage < RC3D_TPAGES) {
        s_tpageKeepPixels[tpage] = 1;
        RC3D_TexKeepPixels(tpage);
    }
}

void FinalizeMenuTexturesD3D(void)
{
    RC3D_TexMarkAllDirty();
}
