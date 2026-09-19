/**
 * aspect.c — Render aspect ratio. See aspect.h for the derivation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aspect.h"

#define ASPECT_4_3 ASPECT_MIN_RATIO

/* Never go NARROWER than the original design. A window taller than 4:3 would
 * otherwise crop the sides off a game whose framing assumes them, so a narrow
 * window letterboxes to 4:3 instead — same as before this module existed.
 *
 * The upper bound is a comfort limit rather than a technical one: past about
 * 32:9 the horizontal field of view is wide enough that the edges of the screen
 * are well outside where the original ever expected geometry to be drawn, and
 * culling/LOD artefacts at the far sides start to show. */
#define ASPECT_MIN ASPECT_4_3
#define ASPECT_MAX 3.7f

float g_renderAspect = 0.0f;                  /* 0 = auto */
static float s_effective = ASPECT_4_3;
static int   s_changed   = 0;

void AspectUpdate(int areaW, int areaH)
{
    float a = g_renderAspect;

    if (a <= 0.0f) {
        a = (areaH > 0) ? ((float)areaW / (float)areaH) : ASPECT_4_3;
    }
    if (a < ASPECT_MIN) a = ASPECT_MIN;
    if (a > ASPECT_MAX) a = ASPECT_MAX;

    if (a != s_effective) {
        s_effective = a;
        s_changed = 1;
    }
}

int AspectConsumeChange(void)
{
    int c = s_changed;
    s_changed = 0;
    return c;
}

float AspectEffective(void)
{
    return s_effective;
}

int AspectProjScaleXDivisor(void)
{
    /* 0x140 (320) is the 4:3 value, and 320 = 240 * (4/3) — the vertical
     * half-height times the aspect. Generalising to 240 * aspect keeps the
     * vertical field of view fixed and opens the horizontal one. */
    int d = (int)(240.0f * s_effective + 0.5f);
    return (d < 1) ? 1 : d;
}

float Aspect2DScale(void)
{
    return ASPECT_4_3 / s_effective;
}

float AspectParse(const char *s)
{
    if (s == NULL || *s == '\0') {
        return -1.0f;
    }
    if (strcmp(s, "auto") == 0) {
        return 0.0f;
    }

    /* "W:H" or "W/H" */
    const char *sep = strpbrk(s, ":/");
    if (sep != NULL) {
        double w = atof(s);
        double h = atof(sep + 1);
        if (w > 0.0 && h > 0.0) {
            return (float)(w / h);
        }
        return -1.0f;
    }

    double v = atof(s);
    return (v > 0.0) ? (float)v : -1.0f;
}
