/**
 * r_draw.h — Geometry submission API for the immediate-mode renderer.
 *
 * All functions emit geometry immediately using the current render state.
 * No batching — the backend draws to the GPU right away.
 */
#ifndef R_DRAW_H
#define R_DRAW_H

#include "r_types.h"
#include "vertex_struct.h"

/* Pre-clipped primitives (caller guarantees all verts are visible). */
void R_DrawTri(const RenderVertex v[3]);
void R_DrawQuad(const RenderVertex v[4]);
void R_DrawTriFan(const RenderVertex *v, int count);

/* 2D textured quad (HUD, menus, sprites).
 * Position and UV in screen/texel coordinates. */
void R_DrawQuad2D(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1,
                  float z, uint32_t color);

/* Untextured 2D quad (iris, sky fill, borders).
 * Temporarily disables texturing for this primitive. */
void R_DrawQuad2DSolid(float x0, float y0, float x1, float y1,
                       float z, uint32_t color);

/* Declare that the enclosed draws are screen-space HUD/overlay, not world
 * geometry. Nestable.
 *
 * Only meaningful for stereo: HUD elements are drawn as real quads at a shallow
 * camera depth, so without this they take the world depth shear and shoot out
 * in front of the screen. Inside this scope they instead take the flat HUD
 * offset, which the HUD-depth setting controls independently of convergence.
 * A no-op when stereo is off. */
void R_Begin2D(void);
void R_End2D(void);

/* Full-screen overlay scope: pinned to zero disparity (exactly the screen
 * plane, independent of the HUD depth setting) and exempt from the widescreen
 * 2D compression so it still covers the whole screen. For surfaces drawn ON the
 * display rather than content placed in it — the split-screen separator and the
 * fade/transition iris. Nestable. */
void R_BeginOverlay(void);
void R_EndOverlay(void);

/* Clear the colour buffer, honouring the current scissor. Routed through the
 * stereo recorder so each eye gets its own clear on replay. */
void R_ClearColor(float r, float g, float b, float a);

/* Frame lifecycle */
void R_EndFrame(void);
void R_Flip(void);
void R_ClearAndReset(void);

#endif /* R_DRAW_H */
