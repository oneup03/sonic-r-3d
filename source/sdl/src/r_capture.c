/**
 * r_capture.c — Per-frame draw-command capture and per-eye replay.
 *
 * See r_capture.h for why the draw stream is replayed rather than the game.
 */

#ifndef SONICR_SOFT_RENDER

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r_types.h"
#include "r_state.h"
#include "r_state_internal.h"
#include "r_draw.h"
#include "r_capture.h"
#include "stereo.h"

int g_rCaptureActive    = 0;
int g_rCaptureReplaying = 0;

typedef enum {
    RCMD_DRAW = 0,
    RCMD_CLEAR_COLOR,
    RCMD_CLEAR_DEPTH
} RCmdType;

typedef struct {
    int             type;
    R_StateSnapshot state;      /* state in force when the call was made */
    int             vtxOffset;  /* index into s_verts (RCMD_DRAW) */
    int             vtxCount;
    int             layer;      /* R_Layer — see r_types.h */
    float           clear[4];   /* RGBA (RCMD_CLEAR_COLOR) */
} RCmd;

/* Growable arenas. Sized from a 1998-era frame: a few thousand verts. They
 * only ever grow, so after the first busy frame there is no allocation on the
 * hot path at all. */
static RCmd         *s_cmds     = NULL;
static int           s_cmdCount = 0;
static int           s_cmdCap   = 0;

static RenderVertex *s_verts    = NULL;
static int           s_vtxCount = 0;
static int           s_vtxCap   = 0;

/* Per-frame diagnostics (see R_CaptureDepthStats). */
static float s_minW = 0.0f;
static float s_maxW = 0.0f;
static int   s_haveW = 0;
static int   s_verts2D   = 0;   /* flagged via the R_DrawQuad2D* helpers */
static int   s_vertsFlat = 0;   /* rhw == 1: already-projected screen space */
static int   s_vertsPersp= 0;   /* rhw = 1/camZ: real world geometry */
/* Per-tpage upload count for the current frame.
 *
 * The hazard is NOT "an upload happened late". A tpage is uploaded lazily the
 * first time it is bound, which can be deep into the frame and is harmless:
 * its contents do not change again before replay, so replaying with them is
 * correct. The hazard is the SAME tpage being uploaded TWICE in one frame with
 * different contents, because then draws recorded before the second upload
 * would sample the later texels on replay. */
#define RC_MAX_TPAGE 52
static unsigned char s_tpageUploads[RC_MAX_TPAGE];
static int   s_uploadWarned = 0;

/* Provided by r_gl_backend.c. */
extern void R_StateCapture(R_StateSnapshot *dst);
extern void R_StateRestore(const R_StateSnapshot *src);
extern void R_FlushState(void);
extern void R_ReplayPrimitive(const RenderVertex *v, int count, int layer);
extern void R_ReplayClearColor(const float rgba[4]);
extern void R_ReplayClearDepth(void);
extern void R_SetEyeShear(float dir);

static int ensureCmd(void)
{
    if (s_cmdCount < s_cmdCap) {
        return 1;
    }
    int cap = s_cmdCap ? s_cmdCap * 2 : 2048;
    RCmd *p = (RCmd *)realloc(s_cmds, (size_t)cap * sizeof(RCmd));
    if (p == NULL) {
        return 0;
    }
    s_cmds = p;
    s_cmdCap = cap;
    return 1;
}

static int ensureVerts(int extra)
{
    if (s_vtxCount + extra <= s_vtxCap) {
        return 1;
    }
    int cap = s_vtxCap ? s_vtxCap : 32768;
    while (cap < s_vtxCount + extra) {
        cap *= 2;
    }
    RenderVertex *p = (RenderVertex *)realloc(s_verts, (size_t)cap * sizeof(RenderVertex));
    if (p == NULL) {
        return 0;
    }
    s_verts = p;
    s_vtxCap = cap;
    return 1;
}

void R_CaptureBegin(void)
{
    s_cmdCount = 0;
    s_vtxCount = 0;
    s_haveW = 0;
    s_minW = 0.0f;
    s_maxW = 0.0f;
    s_verts2D = s_vertsFlat = s_vertsPersp = 0;
    memset(s_tpageUploads, 0, sizeof(s_tpageUploads));
}

void R_CaptureNoteUpload(int tpage)
{
    if (g_rCaptureReplaying || !g_rCaptureActive) {
        return;
    }
    if (tpage < 0 || tpage >= RC_MAX_TPAGE) {
        return;
    }
    if (s_tpageUploads[tpage] < 255) {
        s_tpageUploads[tpage]++;
    }
    /* Second upload of the same tpage inside one recorded frame breaks the
     * assumption documented in r_capture.h. Warn once rather than rendering
     * something that merely looks plausible. */
    /* Only a hazard if geometry was ALREADY recorded when the second upload
     * lands — two uploads back-to-back before any draw (which happens during
     * screen setup) are both superseded by the final contents and replay
     * correctly. */
    if (s_tpageUploads[tpage] == 2 && s_cmdCount > 0 && !s_uploadWarned) {
        s_uploadWarned = 1;
        fprintf(stderr,
                "r_capture: WARNING tpage %d uploaded twice in one frame after "
                "%d recorded draw(s) — replayed eyes may sample post-upload "
                "texels. See r_capture.h.\n", tpage, s_cmdCount);
    }
}

int R_CaptureDraw(const RenderVertex *v, int count, int layer)
{
    if (!g_rCaptureActive || g_rCaptureReplaying) {
        return 0;
    }
    if (count < 3 || v == NULL) {
        return 1;   /* consumed: nothing to draw either way */
    }
    if (!ensureCmd() || !ensureVerts(count)) {
        /* Out of memory: fall back to drawing immediately. The frame will be
         * mono but the game keeps running. */
        return 0;
    }

    RCmd *c = &s_cmds[s_cmdCount++];
    c->type      = RCMD_DRAW;
    c->vtxOffset = s_vtxCount;
    c->vtxCount  = count;
    c->layer     = layer;
    R_StateCapture(&c->state);

    memcpy(&s_verts[s_vtxCount], v, (size_t)count * sizeof(RenderVertex));
    s_vtxCount += count;

    /* Depth census, for calibration and for telling screen-space geometry from
     * world geometry.
     *
     * rhw == 1.0 (w == 1.0) is the D3D convention this decompile inherits for
     * ALREADY-PROJECTED, screen-space vertices — "no perspective divide
     * needed". Real world geometry carries rhw = 1/camZ with camZ far from 1.
     * Counting the two populations separately is what shows whether a frame is
     * mostly flat overlay or mostly scene. */
    if (layer != R_LAYER_WORLD) {
        s_verts2D += count;
    }
    for (int i = 0; i < count; i++) {
        float rhw = v[i].rhw;
        if (rhw <= 0.0f) {
            continue;
        }
        float w = 1.0f / rhw;
        if (w > 0.999f && w < 1.001f) {
            s_vertsFlat++;
            continue;
        }
        s_vertsPersp++;
        if (!s_haveW) {
            s_minW = s_maxW = w;
            s_haveW = 1;
        } else if (w < s_minW) {
            s_minW = w;
        } else if (w > s_maxW) {
            s_maxW = w;
        }
    }
    return 1;
}

int R_CaptureClearColor(float r, float g, float b, float a)
{
    if (!g_rCaptureActive || g_rCaptureReplaying) {
        return 0;
    }
    if (!ensureCmd()) {
        return 0;
    }
    RCmd *c = &s_cmds[s_cmdCount++];
    c->type = RCMD_CLEAR_COLOR;
    c->clear[0] = r; c->clear[1] = g; c->clear[2] = b; c->clear[3] = a;
    /* Clears obey the scissor test, so the state at call time matters just as
     * much as it does for geometry. */
    R_StateCapture(&c->state);
    return 1;
}

int R_CaptureClearDepth(void)
{
    if (!g_rCaptureActive || g_rCaptureReplaying) {
        return 0;
    }
    if (!ensureCmd()) {
        return 0;
    }
    RCmd *c = &s_cmds[s_cmdCount++];
    c->type = RCMD_CLEAR_DEPTH;
    R_StateCapture(&c->state);
    return 1;
}

void R_CaptureReplay(int eye)
{
    g_rCaptureReplaying = 1;
    R_SetEyeShear(stereoShearDir(eye));

    for (int i = 0; i < s_cmdCount; i++) {
        const RCmd *c = &s_cmds[i];
        R_StateRestore(&c->state);
        R_FlushState();

        switch (c->type) {
            case RCMD_CLEAR_COLOR:
                R_ReplayClearColor(c->clear);
                break;
            case RCMD_CLEAR_DEPTH:
                R_ReplayClearDepth();
                break;
            case RCMD_DRAW:
            default:
                R_ReplayPrimitive(&s_verts[c->vtxOffset], c->vtxCount, c->layer);
                break;
        }
    }

    R_SetEyeShear(0.0f);
    g_rCaptureReplaying = 0;
}

void R_CaptureVertexCensus(int *flat, int *persp, int *tagged2D)
{
    if (flat)     *flat     = s_vertsFlat;
    if (persp)    *persp    = s_vertsPersp;
    if (tagged2D) *tagged2D = s_verts2D;
}

int R_CaptureDepthStats(float *minW, float *maxW, int *drawCalls, int *verts)
{
    if (drawCalls) *drawCalls = s_cmdCount;
    if (verts)     *verts     = s_vtxCount;
    if (!s_haveW) {
        return 0;
    }
    if (minW) *minW = s_minW;
    if (maxW) *maxW = s_maxW;
    return 1;
}

void R_CaptureFree(void)
{
    free(s_cmds);  s_cmds  = NULL; s_cmdCap = 0; s_cmdCount = 0;
    free(s_verts); s_verts = NULL; s_vtxCap = 0; s_vtxCount = 0;
}

#endif /* !SONICR_SOFT_RENDER */
