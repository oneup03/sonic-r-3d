/**
 * aspect.h — Render aspect ratio.
 *
 * Sonic R was built for 4:3 and the ratio is baked into the projection rather
 * than derived from the window: the horizontal projection scale divides by
 * 0x140 (320) and the vertical by 0xF0 (240), giving
 *
 *     tan(hfov/2) = (W/2) / projScaleX = 320/512 = 0.625
 *     tan(vfov/2) = (H/2) / projScaleY = 240/512 = 0.46875
 *
 * whose ratio is exactly 4:3 for ANY g_screenWidth/Height. Changing the virtual
 * resolution therefore does nothing to the field of view — it only changes how
 * many virtual units the same 4:3 frustum is quantised into. The ratio lives in
 * that 320, and this module owns it.
 *
 * Widening is "Hor+": the vertical field of view is held fixed and the
 * horizontal one opens up, so a wider display shows more at the sides rather
 * than cropping the top and bottom. That is the behaviour players expect from a
 * widescreen patch, and it keeps the camera framing the original was tuned
 * around.
 *
 * Two things follow from widening that are handled elsewhere but belong in this
 * explanation:
 *
 *  - The GL viewport must stop being the largest 4:3 box and become the largest
 *    box of the chosen aspect, or the wider frustum just gets squeezed back into
 *    a 4:3 rect and everything stretches. See BeginFrame in render_gl.c.
 *  - 2D content — HUD, menus, the full-screen backdrops — is authored in a
 *    640x480 4:3 space. Stretching that to fill a wider viewport distorts it and
 *    flings HUD elements into the corners, so screen-space geometry is
 *    compressed back toward the centre by Aspect2DScale(). The world gets the
 *    wider frustum; the 2D layer keeps its designed proportions, pillarboxed.
 */
#ifndef ASPECT_H
#define ASPECT_H

/* User setting. 0 = auto (track the window/display aspect). Otherwise an
 * explicit ratio such as 4.0f/3.0f or 16.0f/9.0f. Set from --aspect. */
extern float g_renderAspect;

/* Recompute the effective aspect from the setting and the current drawable.
 * Call once per frame before the viewport is set. */
void  AspectUpdate(int drawableW, int drawableH);

/* The aspect actually in use, after auto-resolution and clamping. */
float AspectEffective(void);

/* Divisor for the horizontal projection scale, replacing the hardcoded 0x140.
 * 320 at 4:3; 240 * aspect in general. */
int   AspectProjScaleXDivisor(void);

/* Horizontal scale to apply to screen-space (2D) geometry about the screen
 * centre so it keeps 4:3 proportions inside a wider viewport. 1.0 at 4:3. */
float Aspect2DScale(void);

/* Parse an --aspect argument: "auto", "4:3", "16:9", "16:10", or a decimal.
 * Returns the ratio, 0 for auto, or -1 if unparseable. */
float AspectParse(const char *s);

#endif /* ASPECT_H */
