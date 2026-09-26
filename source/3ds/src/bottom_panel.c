/**
 * bottom_panel.c — touch slider on the bottom screen for the one stereo
 * setting that has no right answer in the abstract: convergence (which depth
 * sits on the screen plane). Separation comes from the hardware 3D slider,
 * and with the race HUD on this screen there is no HUD depth to tune.
 *
 * Drawn after the race HUD has been replayed onto the bottom target, in the
 * HUD's own 640x480 virtual space (the bottom screen is 4:3). The race HUD
 * keeps to the edges (timer and ring count along the top, lap counter and
 * minimap along the bottom), so the FPS readout, the label and the slider
 * sit as one group in the empty middle of the screen.
 * Persistence is the existing SONICR.INF stereo block (stereoConfigSave).
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "bottom_panel.h"
#include "r_c3d_internal.h"
#include "stereo.h"

/* Layout, virtual 640x480 units (2x the physical 320x240). The group is
 * FPS line / CONV label / bar, stacked and centred on the screen's middle. */
#define BAR_X0      208.0f
#define BAR_X1      432.0f
#define BAR_H       14.0f
#define GLYPH_PX    4.0f       /* one font pixel, virtual units */
#define GLYPH_H     (7.0f * GLYPH_PX)
#define GLYPH_ADV   (6.0f * GLYPH_PX)
#define GAP_FPS     16.0f      /* below the FPS line */
#define GAP_LABEL   6.0f       /* between the label and the bar */
#define GROUP_H     (GLYPH_H + GAP_FPS + GLYPH_H + GAP_LABEL + BAR_H)
#define FPS_Y       (240.0f - GROUP_H * 0.5f)
#define LABEL_Y     (FPS_Y + GLYPH_H + GAP_FPS)
#define BAR1_Y      (LABEL_Y + GLYPH_H + GAP_LABEL)   /* convergence bar */

#define COL_TRACK   0xB0303030u
#define COL_FILL_C  0xE04080E0u
#define COL_KNOB    0xFFFFFFFFu
#define COL_TEXT    0xFFE0E0E0u

static int   s_touchHeld = 0;
static int   s_touchX = 0, s_touchY = 0;
static int   s_dragBar = 0;      /* 0 none, 1 convergence */

/* 5x7 glyphs for the labels, one byte per row, MSB = leftmost column. */
typedef struct { char ch; unsigned char rows[7]; } Glyph;
static const Glyph s_font[] = {
    { 'C', { 0x0E,0x11,0x10,0x10,0x10,0x11,0x0E } },
    { 'O', { 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E } },
    { 'N', { 0x11,0x19,0x15,0x13,0x11,0x11,0x11 } },
    { 'V', { 0x11,0x11,0x11,0x11,0x11,0x0A,0x04 } },
    { 'H', { 0x11,0x11,0x11,0x1F,0x11,0x11,0x11 } },
    { 'U', { 0x11,0x11,0x11,0x11,0x11,0x11,0x0E } },
    { 'D', { 0x1E,0x11,0x11,0x11,0x11,0x11,0x1E } },
    { 'F', { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10 } },
    { 'P', { 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10 } },
    { 'S', { 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E } },
    { '0', { 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E } },
    { '1', { 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E } },
    { '2', { 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F } },
    { '3', { 0x1F,0x02,0x04,0x02,0x01,0x11,0x0E } },
    { '4', { 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 } },
    { '5', { 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E } },
    { '6', { 0x06,0x08,0x10,0x1E,0x11,0x11,0x0E } },
    { '7', { 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 } },
    { '8', { 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E } },
    { '9', { 0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C } },
    { 'M', { 0x11,0x1B,0x15,0x15,0x11,0x11,0x11 } },
    { 'Z', { 0x1F,0x01,0x02,0x04,0x08,0x10,0x1F } },
    { ' ', { 0,0,0,0,0,0,0 } },
};

static void draw_text(const char *s, float x, float y, uint32_t col)
{
    for (; *s; s++) {
        const Glyph *g = NULL;
        for (unsigned i = 0; i < sizeof(s_font) / sizeof(s_font[0]); i++) {
            if (s_font[i].ch == *s) { g = &s_font[i]; break; }
        }
        if (g) {
            for (int r = 0; r < 7; r++) {
                for (int c = 0; c < 5; c++) {
                    if (g->rows[r] & (0x10 >> c)) {
                        float px = x + c * GLYPH_PX, py = y + r * GLYPH_PX;
                        RC3D_ImmQuad(px, py, px + GLYPH_PX, py + GLYPH_PX, col);
                    }
                }
            }
        }
        x += GLYPH_ADV;
    }
}

/* Horizontally centred text: n glyphs advance n*ADV, the last has no gap. */
static void draw_text_centred(const char *s, float y, uint32_t col)
{
    float w = (float)strlen(s) * GLYPH_ADV - (GLYPH_ADV - 5.0f * GLYPH_PX);
    draw_text(s, 320.0f - w * 0.5f, y, col);
}

/* convergence is perceived logarithmically: map [MIN, MAX] to [0, 1] in log space */
static float conv_to_t(float c)
{
    float t = logf(c / S3D_CONVERGENCE_MIN) / logf(S3D_CONVERGENCE_MAX / S3D_CONVERGENCE_MIN);
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

static float t_to_conv(float t)
{
    return S3D_CONVERGENCE_MIN * powf(S3D_CONVERGENCE_MAX / S3D_CONVERGENCE_MIN, t);
}

static void draw_bar(float y, float t, uint32_t fill)
{
    RC3D_ImmQuad(BAR_X0, y, BAR_X1, y + BAR_H, COL_TRACK);
    RC3D_ImmQuad(BAR_X0, y, BAR_X0 + (BAR_X1 - BAR_X0) * t, y + BAR_H, fill);
    float kx = BAR_X0 + (BAR_X1 - BAR_X0) * t;
    RC3D_ImmQuad(kx - 3.0f, y - 3.0f, kx + 3.0f, y + BAR_H + 3.0f, COL_KNOB);
}

void BottomPanel_Touch(int px, int py, int held)
{
    /* physical 320x240 -> virtual 640x480 */
    int vx = px * 2, vy = py * 2;
    if (held && !s_touchHeld) {
        /* pen down: pick a bar by its band (generous vertical hit area) */
        s_dragBar = (vy >= BAR1_Y - 10 && vy < BAR1_Y + BAR_H + 10) ? 1 : 0;
    }
    if (!held) {
        s_dragBar = 0;
    }
    s_touchHeld = held;
    s_touchX = vx;
    s_touchY = vy;
    if (!held || s_dragBar == 0) {
        return;
    }
    float t = ((float)vx - BAR_X0) / (BAR_X1 - BAR_X0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    g_s3dConvergence = t_to_conv(t);
    stereoClampSettings();
}

void BottomPanel_Draw(void)
{
    /* "30 FPS 804MHZ": the clock confirms the CIA got the New 3DS speed-up.
     * The frame-budget and speed-up diagnostics still go to the log every
     * 300 frames (r_c3d_stereo.c); they no longer clutter the screen. */
    char line[32];
    snprintf(line, sizeof(line), "%d FPS %dMHZ", g_rc3dFps, g_rc3dCpuMhz);
    draw_text_centred(line, FPS_Y, COL_TEXT);

    draw_text_centred("CONV", LABEL_Y, COL_TEXT);
    draw_bar(BAR1_Y, conv_to_t(g_s3dConvergence), COL_FILL_C);
}
