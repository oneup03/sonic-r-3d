/**
 * stereo_menu.c — Stereoscopic 3D rows on the Graphics options page.
 * See stereo_menu.h for why these particular rows.
 */

#include <stdio.h>
#include "stereo.h"
#include "stereo_menu.h"

/* Pixel-font text, exported from screen_misc.c for exactly this sort of use.
 * Draws through R_DrawQuad2DSolid, which is already tagged as screen-space, so
 * these rows land at HUD depth in stereo without any extra handling. */
extern void DrawDebugOverlayText(const char *s, int x, int y, int pixSz,
                                 unsigned int color);

/* Which options item is which setting.
 *
 * Deliberately NOT items 26 or 30: 26 is the quality/draw-distance row, which
 * still does something, and 30 is Back. */
#define ROW_MODE        23   /* was: DirectDraw resolution   */
#define ROW_SEPARATION  24   /* was: colour depth            */
#define ROW_CONVERGENCE 25   /* was: software interlace      */
#define ROW_HUD_DEPTH   27   /* was: resolution level        */
#define ROW_SWAP_EYES   28   /* was: emerald render flag     */
#define ROW_GHOST       29   /* was: software render option  */

/* Layout, in the menu's 640x480 virtual space.
 *
 * LABEL_X matches where the sprite path puts a 136-wide Graphics label
 * (0x140 - 136*2 = 48), so the stereo rows line up with the sprite rows above
 * and below them rather than looking bolted on. */
#define LABEL_X   48
#define VALUE_X   330
#define PIX_SIZE  2
#define ROW_Y_PAD 8    /* centres 7 text rows inside the 32-tall item slot */

#define COL_LABEL 0xFFC0C0C0u
#define COL_VALUE 0xFFFFFFFFu
#define COL_DIM   0xFF808080u

int StereoMenuIsRow(int itemIndex)
{
    switch (itemIndex) {
        case ROW_MODE:
        case ROW_SEPARATION:
        case ROW_CONVERGENCE:
        case ROW_HUD_DEPTH:
        case ROW_SWAP_EYES:
        case ROW_GHOST:
            return 1;
        default:
            return 0;
    }
}

/* The pixel font has A-Z, 0-9, '.', '/', '-' and '?' — no ':' or '+'. Labels
 * and values below stay inside that set. */
static void rowText(int itemIndex, char *label, int labelSz,
                    char *value, int valueSz, int *dim)
{
    *dim = 0;

    switch (itemIndex) {
        case ROW_MODE:
            snprintf(label, (size_t)labelSz, "3D MODE");
            snprintf(value, (size_t)valueSz, "%s", stereoModeName(g_s3dMode));
            break;

        case ROW_SEPARATION:
            snprintf(label, (size_t)labelSz, "SEPARATION");
            snprintf(value, (size_t)valueSz, "%.3f", (double)g_s3dSeparation);
            break;

        case ROW_CONVERGENCE:
            snprintf(label, (size_t)labelSz, "CONVERGENCE");
            snprintf(value, (size_t)valueSz, "%.0f", (double)g_s3dConvergence);
            break;

        case ROW_HUD_DEPTH:
            snprintf(label, (size_t)labelSz, "HUD DEPTH");
            snprintf(value, (size_t)valueSz, "%.1f", (double)g_s3dHudDepth);
            break;

        case ROW_SWAP_EYES:
            snprintf(label, (size_t)labelSz, "SWAP EYES");
            snprintf(value, (size_t)valueSz, "%s", g_s3dSwapEyes ? "ON" : "OFF");
            break;

        case ROW_GHOST:
            snprintf(label, (size_t)labelSz, "GHOST REDUCE");
            snprintf(value, (size_t)valueSz, "%.2f", (double)g_s3dGhostContrast);
            break;

        default:
            label[0] = '\0';
            value[0] = '\0';
            return;
    }

    /* Everything below the mode row is inert while stereo is off. Dimming says
     * so without hiding the rows, which would make the page jump around as the
     * mode is toggled. */
    if (itemIndex != ROW_MODE && g_s3dMode == S3D_OFF) {
        *dim = 1;
    }
}

void StereoMenuDrawRow(int itemIndex, int rowY)
{
    char label[24];
    char value[24];
    int dim = 0;

    rowText(itemIndex, label, (int)sizeof(label), value, (int)sizeof(value), &dim);
    if (label[0] == '\0') {
        return;
    }

    /* rowY arrives in the menu's half-height units, the same value the sprite
     * path doubles before handing to DrawTexturedQuad. */
    const int y = rowY * 2 + ROW_Y_PAD;

    DrawDebugOverlayText(label, LABEL_X, y, PIX_SIZE,
                         dim ? COL_DIM : COL_LABEL);
    DrawDebugOverlayText(value, VALUE_X, y, PIX_SIZE,
                         dim ? COL_DIM : COL_VALUE);
}

int StereoMenuAdjust(int itemIndex, int direction)
{
    if (!StereoMenuIsRow(itemIndex)) {
        return 0;
    }

    /* Only the mode row responds while stereo is off — matching the dimming in
     * rowText, so what looks inert is inert. */
    if (itemIndex != ROW_MODE && g_s3dMode == S3D_OFF) {
        return -1;   /* claimed, but inert — no click */
    }

    switch (itemIndex) {
        case ROW_MODE: {
            int m = g_s3dMode + direction;
            if (m < 0)                 m = S3D_MODE_COUNT - 1;
            if (m >= S3D_MODE_COUNT)   m = 0;
            g_s3dMode = m;
            break;
        }

        case ROW_SEPARATION:
            g_s3dSeparation += (float)direction * S3D_SEPARATION_STEP;
            break;

        case ROW_CONVERGENCE:
            /* Multiplicative, like the hotkey: perceived depth tracks the
             * ratio, so a fixed increment is far too coarse near the camera and
             * far too fine out at the background. */
            if (direction > 0) {
                g_s3dConvergence *= S3D_CONVERGENCE_STEP;
            } else {
                g_s3dConvergence /= S3D_CONVERGENCE_STEP;
            }
            break;

        case ROW_HUD_DEPTH:
            g_s3dHudDepth += (float)direction * S3D_HUD_DEPTH_STEP;
            break;

        case ROW_SWAP_EYES:
            g_s3dSwapEyes = !g_s3dSwapEyes;
            break;

        case ROW_GHOST:
            g_s3dGhostContrast += (float)direction * 0.01f;
            break;

        default:
            return -1;
    }

    stereoClampSettings();
    /* The mode row can turn stereo on or off, and g_s3dActive gates the whole
     * render path — so it has to be refreshed here, not left until the next
     * frame boundary. */
    stereoRefreshActive();
    return 1;
}
