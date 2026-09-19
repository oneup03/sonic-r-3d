/**
 * r_capture.h — Per-frame draw-command capture and per-eye replay.
 *
 * Why this exists
 * ---------------
 * Stereo needs the frame drawn twice, once per eye. In this codebase that
 * cannot be done by calling a "render the frame" function twice: drawing is
 * spread across ~21 separate FlipD3D() present sites (every menu and screen
 * runs its own `while (1) { draw; FlipD3D(); }` loop), and the race loop
 * interleaves drawing with game logic through goto. Re-entering any of that
 * would re-step game state.
 *
 * So instead of replaying the GAME, we replay the DRAW STREAM. The backend's
 * public surface is narrow — all geometry funnels through R_DrawTriFan, and
 * state is already a single snapshot struct — so recording it is cheap and
 * total. At present time the recording is played back twice, once per eye,
 * with a different shear each time.
 *
 * Because capture is bypassed entirely when stereo is off, the non-stereo path
 * stays exactly what it was: immediate-mode GL, no buffering, no behaviour
 * change.
 *
 * What is NOT captured
 * --------------------
 * Texture uploads. Both eyes want identical texture state, so uploads run
 * immediately at capture time and are not replayed. This is only correct while
 * no tpage is re-uploaded with different content partway through a frame — if
 * that ever happens, draws recorded before the re-upload would sample the
 * later content on replay. Sonic R uploads from ProcessTpageStates at frame
 * start, so this holds; R_CaptureNoteUpload() exists to catch a regression.
 */
#ifndef R_CAPTURE_H
#define R_CAPTURE_H

#include "r_types.h"

/* Nonzero while the backend should record instead of drawing. Read on the hot
 * path by R_DrawTriFan, so it is a plain int rather than a function call. */
extern int g_rCaptureActive;

/* Nonzero while R_CaptureReplay is walking the command list. The backend uses
 * this to avoid re-capturing its own replayed draws. */
extern int g_rCaptureReplaying;

/* Start a new frame's recording. Cheap: resets counters, keeps the arenas. */
void R_CaptureBegin(void);

/* Record one primitive / clear. Each returns 1 when the call was consumed by
 * the recorder and the caller should NOT also draw it immediately. */
int  R_CaptureDraw(const RenderVertex *v, int count, int layer);
int  R_CaptureClearColor(float r, float g, float b, float a);
int  R_CaptureClearDepth(void);

/* Replay the recorded frame for one eye (0 = left, 1 = right) into whatever
 * framebuffer is currently bound. */
void R_CaptureReplay(int eye);

/* Diagnostics: the clip-space w (== camera-space Z) range seen across the
 * recorded 3D geometry this frame. This is the range convergence has to sit
 * inside, and is the quickest way to calibrate it for a given track. Returns 0
 * if no 3D geometry was recorded. */
int  R_CaptureDepthStats(float *minW, float *maxW, int *drawCalls, int *verts);

/* Called by the backend when it performs a texture upload, so the "no
 * mid-frame re-uploads" assumption above can be checked rather than trusted. */
/* Vertex census for the recorded frame: how many vertices were already-
 * projected screen space (rhw == 1), how many carried a real perspective W,
 * and how many were tagged 2D by the R_DrawQuad2D* helpers. */
void R_CaptureVertexCensus(int *flat, int *persp, int *tagged2D);

void R_CaptureNoteUpload(int tpage);

void R_CaptureFree(void);

#endif /* R_CAPTURE_H */
