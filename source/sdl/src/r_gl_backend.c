/**
 * r_gl_backend.c — OpenGL 1.x implementation of the immediate-mode render API.
 *
 * Implements r_state.h, r_draw.h, r_texture.h, and frame lifecycle.
 * Uses a lazy state tracker to minimize redundant GL calls.
 */

/* SOFT=1 builds replace this whole file with r_soft_backend.c. */
#ifndef SONICR_SOFT_RENDER

#ifdef __APPLE__
#include <OpenGL/gl.h>
#elif defined(_WIN32)
#include <GL/glew.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "r_types.h"
#include "r_state.h"
#include "r_state_internal.h"
#include "r_draw.h"
#include "r_capture.h"
#include "stereo.h"
#include "aspect.h"
#include "r_texture.h"
#include "sonicr_types.h"
#include "sonicr_globals.h"
#include <string.h>
#include <stdlib.h>

/* Forward declarations for existing frame functions in render_gl.c */
extern void BeginFrame(void);
extern void EndFrame(void);
extern void FlipD3D(void);
extern void ProcessTpageStates(void);

/* Forward declarations for existing GL_* texture functions in render_gl.c */
extern void GL_UploadTpage(int tpage);
extern void GL_UploadTpageRGBA(int tpage, unsigned char *rgba, int w, int h);
extern void GL_UploadTpageSubRect(int tpage, int destX, int destY, int width, int height);
extern void GL_MarkTpageDirty(int tpage);
extern void GL_ClearTpageDirty(int tpage);
extern void GL_FreezeTpage(int tpage);
extern void GL_SetPendingRGBA(int tpage, unsigned char *rgba, int w, int h);
extern void GL_SetNoColorKey(int tpage);
extern void GL_ClearNoColorKey(int tpage);
extern void GL_SetTpageGreen6(int tpage, int on);
extern void GL_InitTextures(void);
extern void GL_SetTpageRGBA8(int tpage, unsigned char *rgba);

/* =====================================================================
 * Internal state snapshot
 * ===================================================================== */

/* R_StateSnapshot now lives in r_state_internal.h so the stereo capture
 * buffer can store one per recorded draw call. */

static R_StateSnapshot s_desired;
static R_StateSnapshot s_current;

/* State stack for R_PushState / R_PopState */
#define R_STATE_STACK_DEPTH 4
static R_StateSnapshot s_stateStack[R_STATE_STACK_DEPTH];
static int s_stackDepth = 0;

/* Default state values */
static const R_StateSnapshot s_defaults = {
    .textureId      = -1,
    .blendMode      = R_BLEND_ALPHA,
    .depthTest      = 1,
    .depthFunc      = R_DEPTH_LEQUAL,
    .depthWrite     = 1,
    .texEnv         = R_TEXENV_MODULATE,
    .filter         = R_FILTER_NEAREST,
    .cullMode       = R_CULL_NONE,
    .alphaTest      = 1,
    .alphaRef       = 0.01f,
    .scissorEnabled = 0,
    .scissorX       = 0,
    .scissorY       = 0,
    .scissorW       = 640,
    .scissorH       = 480,
};

/* =====================================================================
 * Texture backend state
 * ===================================================================== */

/* GL texture objects and dirty flags — still owned by render_gl.c,
 * accessed here via extern for texture binding during R_FlushState. */
extern GLuint s_glTextures[];       /* render_gl.c */

/* Per-tpage filter override, and the filter actually resident on each GL
 * texture object.
 *
 * GL keeps filter state on the TEXTURE OBJECT, not globally — so a single
 * cached "current filter" is wrong the moment a different texture is bound.
 * It is also wrong after any upload: every GL_UploadTpage* path hard-sets
 * GL_NEAREST (render_gl.c:139, :247), so a filter applied earlier is stomped
 * whenever the tpage is re-uploaded. Together those made the old global
 * R_SetFilter path reach only whichever texture happened to be bound at the
 * moment the value changed, and nothing else.
 *
 * The filter is therefore resolved and applied per tpage at BIND time. Both
 * arrays encode 0 = unset and (mode + 1) otherwise, so the zero initialiser
 * means "no override" / "resident filter unknown". s_glTextureFilter is
 * invalidated by R_MarkTextureDirty, which precedes every upload path. */
static unsigned char s_tpageFilter[52];
static unsigned char s_glTextureFilter[52];

/* Apply the effective filter to tpage `tp`, which must be bound already. */
static void GL_ApplyTpageFilter(int tp)
{
    unsigned char want = s_tpageFilter[tp];
    if (want == 0) {
        want = (unsigned char)(s_desired.filter + 1);
    }
    if (s_glTextureFilter[tp] != want) {
        GLenum f = (want == (unsigned char)(R_FILTER_LINEAR + 1)) ? GL_LINEAR
                                                                  : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        s_glTextureFilter[tp] = want;
    }
}

/* =====================================================================
 * R_FlushState — apply only the GL calls for state that changed
 * ===================================================================== */

void R_FlushState(void)
{
    /* Texture binding — skip if tpage unchanged.
     * Dirty-texture uploads still happen even on a cache hit (the tpage
     * data changed, not the binding). */
    if (s_desired.textureId != s_current.textureId) {
        if (s_desired.textureId < 0) {
            glDisable(GL_TEXTURE_2D);
        } else {
            int tp = s_desired.textureId;
            if (tp < 52) {
                if (g_tpagePixelBuf[tp] != NULL) {
                    if (s_glTextureDirty[tp]) {
                        R_CaptureNoteUpload(tp);
                        GL_UploadTpage(tp);
                    }
                    glEnable(GL_TEXTURE_2D);
                    glBindTexture(GL_TEXTURE_2D, s_glTextures[tp]);
                }
                else {
                    glDisable(GL_TEXTURE_2D);
                }
            }
        }
        s_current.textureId = s_desired.textureId;
    }
    else if (s_desired.textureId >= 0 && s_desired.textureId < 52) {
        /* Same tpage, but check if pixel data was updated */
        if (s_glTextureDirty[s_desired.textureId]) {
            R_CaptureNoteUpload(s_desired.textureId);
            GL_UploadTpage(s_desired.textureId);
        }
    }

    /* Filter, resolved per tpage against the texture actually bound. Placed
     * after the whole binding block so it covers all three ways the resident
     * filter can go stale: a fresh bind, an in-place re-upload of the same
     * tpage, and a global R_SetFilter while the same tpage stays bound. */
    if (s_desired.textureId >= 0 && s_desired.textureId < 52 &&
        g_tpagePixelBuf[s_desired.textureId] != NULL) {
        GL_ApplyTpageFilter(s_desired.textureId);
    }

    /* Depth test */
    if (s_desired.depthTest != s_current.depthTest) {
        if (s_desired.depthTest) {
            glEnable(GL_DEPTH_TEST);
        }
        else {
            glDisable(GL_DEPTH_TEST);
        }
        s_current.depthTest = s_desired.depthTest;
    }

    /* Depth function */
    if (s_desired.depthFunc != s_current.depthFunc) {
        switch (s_desired.depthFunc) {
            case R_DEPTH_LEQUAL:
                glDepthFunc(GL_LEQUAL);
                break;
            case R_DEPTH_LESS:
                glDepthFunc(GL_LESS);
                break;
            case R_DEPTH_ALWAYS:
                glDepthFunc(GL_ALWAYS);
                break;
        }
        s_current.depthFunc = s_desired.depthFunc;
    }

    /* Depth write */
    if (s_desired.depthWrite != s_current.depthWrite) {
        glDepthMask(s_desired.depthWrite ? GL_TRUE : GL_FALSE);
        s_current.depthWrite = s_desired.depthWrite;
    }

    /* Texture environment (overbright) */
    if (s_desired.texEnv != s_current.texEnv) {
#ifdef __EMSCRIPTEN__
        /* WebGL legacy GL emulation doesn't support GL_COMBINE/GL_ADD_SIGNED.
         * Fall back to plain GL_MODULATE for all modes. */
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
#else
        if (s_desired.texEnv == R_TEXENV_ADD_SIGNED) {
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_ADD_SIGNED);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PREVIOUS);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
        }
        else {
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0f);
        }
#endif
        s_current.texEnv = s_desired.texEnv;
    }

    /* Blend mode */
    if (s_desired.blendMode != s_current.blendMode) {
        switch (s_desired.blendMode) {
            case R_BLEND_NONE:
                glDisable(GL_BLEND);
                break;
            case R_BLEND_ALPHA:
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                break;
            case R_BLEND_ADDITIVE:
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE);
                break;
        }
        s_current.blendMode = s_desired.blendMode;
    }

    /* Filter is applied per tpage at bind time above — see GL_ApplyTpageFilter.
     * GL keeps it on the texture object, so it cannot be driven from here. The
     * snapshot field is still synced so the two fields never disagree, which
     * would trip up any whole-struct compare added later (the GLES2 backend
     * already has one). */
    s_current.filter = s_desired.filter;

    glDisable(GL_CULL_FACE);

    /* Alpha test */
    if (s_desired.alphaTest != s_current.alphaTest) {
        if (s_desired.alphaTest) {
            glEnable(GL_ALPHA_TEST);
        }
        else {
            glDisable(GL_ALPHA_TEST);
        }
        s_current.alphaTest = s_desired.alphaTest;
    }

    /* Alpha ref */
    if (s_desired.alphaRef != s_current.alphaRef) {
        glAlphaFunc(GL_GREATER, s_desired.alphaRef);
        s_current.alphaRef = s_desired.alphaRef;
    }

    /* Scissor enable/disable */
    if (s_desired.scissorEnabled != s_current.scissorEnabled) {
        if (s_desired.scissorEnabled) {
            glEnable(GL_SCISSOR_TEST);
        }
        else {
            glDisable(GL_SCISSOR_TEST);
        }
        s_current.scissorEnabled = s_desired.scissorEnabled;
    }

    /* Scissor rect */
    if (s_desired.scissorEnabled &&
        (s_desired.scissorX != s_current.scissorX ||
         s_desired.scissorY != s_current.scissorY ||
         s_desired.scissorW != s_current.scissorW ||
         s_desired.scissorH != s_current.scissorH))
    {
        glScissor(s_desired.scissorX, s_desired.scissorY,
                  s_desired.scissorW, s_desired.scissorH);
        s_current.scissorX = s_desired.scissorX;
        s_current.scissorY = s_desired.scissorY;
        s_current.scissorW = s_desired.scissorW;
        s_current.scissorH = s_desired.scissorH;
    }
}

/* =====================================================================
 * Render state API implementation
 * ===================================================================== */

void R_SetTexture(int tpageIndex)
{
    s_desired.textureId = tpageIndex;
}

void R_SetBlendMode(R_BlendMode mode)
{
    s_desired.blendMode = mode;
}

void R_SetDepthTest(int enable)
{
    s_desired.depthTest = enable;
}

void R_SetDepthFunc(R_DepthFunc func)
{
    s_desired.depthFunc = func;
}

void R_SetDepthWrite(int enable) 
{
    s_desired.depthWrite = enable;
}

void R_SetTexEnv(R_TexEnvMode mode)
{
    s_desired.texEnv = mode;
}

void R_SetFilter(R_FilterMode mode)
{
    s_desired.filter = mode;
}

/* Per-tpage filter pin. Takes priority over the global R_SetFilter value;
 * applied to the texture object at bind time by GL_ApplyTpageFilter. */
void R_SetTpageFilter(int tpage, R_FilterMode mode)
{
    if (tpage >= 0 && tpage < 52) {
        s_tpageFilter[tpage] = (unsigned char)(mode + 1);
    }
}

void R_ClearTpageFilter(int tpage)
{
    if (tpage >= 0 && tpage < 52) {
        s_tpageFilter[tpage] = 0;
    }
}

/* Chroma gain is a DC-only upload-time transform — GL uploads RGBA directly
 * and the SDL path renders true Add Signed, so there is nothing to correct. */
void R_SetTpageSatBoost(int tpage, int k256)
{
    (void)tpage;
    (void)k256;
}

void R_SetCullMode(R_CullMode mode)
{
    s_desired.cullMode = mode;
}

void R_SetAlphaTest(int enable)
{
    s_desired.alphaTest = enable;
}

void R_SetAlphaRef(float ref)
{
    s_desired.alphaRef = ref;
}

void R_SetScissor(int x, int y, int w, int h)
{
    s_desired.scissorEnabled = 1;
    s_desired.scissorX = x;
    s_desired.scissorY = y;
    s_desired.scissorW = w;
    s_desired.scissorH = h;
}

void R_DisableScissor(void)
{
    s_desired.scissorEnabled = 0;
}

void R_PushState(void)
{
    if (s_stackDepth < R_STATE_STACK_DEPTH) {
        s_stateStack[s_stackDepth++] = s_desired;
    }
}

void R_PopState(void)
{
    if (s_stackDepth > 0) {
        s_desired = s_stateStack[--s_stackDepth];
    }
}

void R_DebugGetState(int *depthTest, int *depthWrite, int *depthFunc,
                     int *blendMode, int *alphaTest, float *alphaRef)
{
    if (depthTest)  *depthTest  = s_desired.depthTest;
    if (depthWrite) *depthWrite = s_desired.depthWrite;
    if (depthFunc)  *depthFunc  = (int)s_desired.depthFunc;
    if (blendMode)  *blendMode  = (int)s_desired.blendMode;
    if (alphaTest)  *alphaTest  = s_desired.alphaTest;
    if (alphaRef)   *alphaRef   = s_desired.alphaRef;
}

void R_ResetState(void)
{
    s_desired = s_defaults;
    /* Force full re-sync on next R_FlushState by invalidating current.
     * Can't use plain memset(0xFF) because 0xFFFFFFFF == -1, which matches
     * s_defaults.textureId — so textureId would appear "unchanged" and
     * R_FlushState would skip the glDisable(GL_TEXTURE_2D) call. */
    memset(&s_current, 0xFF, sizeof(s_current));
    s_current.textureId = -99;   /* won't match any valid id or -1 */
    s_current.alphaRef = -1.0f;  /* ensure float comparison triggers */
    /* Resident texture-object filters are not part of this snapshot; drop
     * what we believe about them so the next bind re-applies. */
    memset(s_glTextureFilter, 0, sizeof(s_glTextureFilter));
    s_stackDepth = 0;
}

/* =====================================================================
 * Geometry submission — immediate draw
 * ===================================================================== */

/* ---------------------------------------------------------------------------
 * Stereo shear state.
 *
 * s_eyeShearDir is +1 for the left eye, -1 for the right, and 0 for mono. At 0
 * both shear expressions below evaluate to +0.0f, so the mono path is
 * arithmetically identical to the pre-stereo code — not merely close.
 *
 * s_emit2D selects which of the two shear formulas applies; it is set per
 * captured command by R_ReplayPrimitive.
 * ------------------------------------------------------------------------- */
static float s_eyeShearDir = 0.0f;
static int   s_emitLayer   = R_LAYER_WORLD;

/* Full-screen overlay scope — stricter than the 2D/HUD one.
 *
 * Distinct from the HUD because these are not content you place, they are
 * surfaces that must lie exactly ON the display:
 *
 *   - Pinned to ZERO disparity, not HUD depth. A split-screen separator is a
 *     divider between two viewports; giving it any parallax makes it hover in
 *     front of or sink behind the very views it is dividing, and it has no
 *     business moving when the player retunes the HUD.
 *   - Exempt from the widescreen 2D compression. Their job is to COVER the
 *     screen — a separator that stops short of the edges leaves the split
 *     unmarked, and a fade iris that does not reach the corners leaves the
 *     scene showing through during a transition.
 *
 * Read by R_EmitVertex, so it has to be declared before it. */
static int s_inOverlay = 0;
static int s_inOverlayDepth = 0;

/* Pillarbox scope — a modifier on whatever depth bucket applies, not a bucket
 * of its own. See R_LAYER_PILLARBOX. */
static int s_inPillarbox = 0;
static int s_inPillarboxDepth = 0;

void R_SetEyeShear(float dir)
{
    s_eyeShearDir = dir;
}

/* Emit a single vertex via GL immediate mode.
 * Uses glVertex4f with W=1/RHW for perspective-correct interpolation.
 *
 * The vertices arriving here are pre-transformed screen-space positions with
 * rhw = 1/Z, so after the multiply below x is already clip-space x and w is
 * already clip-space w. That makes the standard clip-space stereo shear
 *
 *     x_clip += dir * separation * (w_clip - convergence)
 *
 * a direct substitution — no projection matrix to rebuild, no camera to move,
 * and no FoV compensation (the clip-space form is FoV-independent). Geometry
 * at w == convergence is unshifted and lands on the screen plane; nearer
 * geometry gets crossed disparity and pops out.
 *
 * 2D/HUD quads carry a fixed shallow z that has nothing to do with world
 * depth, so feeding them the world formula would fling them far in front of
 * the screen. They instead take a direct NDC offset scaled by separation:
 * hudDepth 0 pins them on the screen plane, +1 puts them at exactly the
 * background disparity (i.e. as far away as the sky). Scaling by separation is
 * what stops the HUD diverging past the background at any separation setting. */
static inline void R_EmitVertex(const RenderVertex *v)
{
    uint32_t argb = v->color;
    float a = (float)((argb >> 24) & 0xFF) / 255.0f;
    float r = (float)((argb >> 16) & 0xFF) / 255.0f;
    float g = (float)((argb >>  8) & 0xFF) / 255.0f;
    float b = (float)((argb      ) & 0xFF) / 255.0f;
    glColor4f(r, g, b, a);
    glTexCoord2f(v->u, v->v);

    float w = (v->rhw > 0.0f) ? (1.0f / v->rhw) : 1.0f;
    float sx = v->sx;

    /* rhw == 1 is the Direct3D "already transformed, no perspective divide
     * needed" convention, which this decompile inherits: the menu wallpaper
     * (RenderWavingMenuBackground) and similar full-screen 2D layers are built
     * that way. Treat them as screen-space regardless of what the caller
     * declared.
     *
     * Without this they are handed w = 1 against a convergence of a few
     * hundred, so (1 - conv/w) is hugely negative and they get pinned at the
     * maximum pop-out — a flat sheet floating well in front of the screen,
     * which reads as inverted depth. The title screen escapes it only because
     * it builds real per-vertex depth (title_render.c: rhw = 1/z_cam).
     *
     * A world vertex can never collide with this: w == 1 is far closer than the
     * near clip, so nothing in the scene is ever submitted at that depth. */
    const int screenSpace = (v->rhw > 0.999999f && v->rhw < 1.000001f);
    const int depthLayer  = (s_emitLayer & R_LAYER_MASK);
    const int overlay     = (depthLayer == R_LAYER_OVERLAY);

    /* Horizontal compression and depth bucket are decided separately — see
     * R_LAYER_PILLARBOX. Overlays are exempt either way: they must span the
     * screen. */
    const int is2D = !overlay && ((s_emitLayer & R_LAYER_PILLARBOX)
                                  || depthLayer == R_LAYER_HUD
                                  || screenSpace);

    /* Widescreen: the world gets the wider frustum, but 2D content does not.
     *
     * HUD, menus and full-screen backdrops are authored in the 640x480 4:3
     * virtual space. Once the viewport stops being 4:3 that space is stretched
     * to fill it, which distorts every sprite and pushes HUD elements out to
     * the corners. Compressing screen-space geometry back toward the centre by
     * the aspect ratio keeps it at its designed proportions, pillarboxed inside
     * the wider frame, while the world keeps the extra field of view.
     *
     * Exactly 1.0 (and skipped) at 4:3, so the original path is untouched. */
    if (is2D) {
        float s2d = Aspect2DScale();
        if (s2d != 1.0f) {
            float cx = (float)g_screenWidth * 0.5f;
            sx = cx + (sx - cx) * s2d;
        }
    }

    float x = sx * w;

    if (s_eyeShearDir != 0.0f) {
        /* Work out the shift in NDC first, clamp it there, then convert.
         *
         * 3D:  shift = sep * (1 - convergence/w). Settles at +sep as
         *      w -> infinity (that IS the background disparity, by
         *      definition) and goes negative — crossed, i.e. popping out of
         *      the screen — for anything nearer than convergence.
         * 2D:  a flat NDC offset; the quad's z is a draw-order artefact, not a
         *      world depth, so the world formula would fling it off-screen.
         */
        float shiftNdc;
        if (overlay) {
            /* Exactly on the screen plane, whatever the HUD is set to. */
            shiftNdc = 0.0f;
        } else if (depthLayer == R_LAYER_HUD) {
            /* Declared overlay — HUD, menus, UI. Sits where the HUD-depth
             * setting puts it, screen plane by default, because it is text and
             * gauges you read rather than scenery you look past. */
            shiftNdc = g_s3dSeparation * g_s3dHudDepth;
        } else if (screenSpace) {
            /* Full-screen 2D layer. Park it at infinity — the same disparity
             * the sky gets — rather than at the HUD plane.
             *
             * It is a backdrop, and a backdrop reads most comfortably when the
             * eyes converge on it exactly as they would on a distant scene:
             * nothing is asked to sit in front of the screen, and anything
             * drawn over it (which lands at HUD depth or nearer) is correctly
             * ordered in front. Putting it at the screen plane instead leaves
             * the eyes working harder for no depth payoff. */
            shiftNdc = g_s3dSeparation * S3D_BACKDROP_DEPTH;
        } else {
            shiftNdc = g_s3dSeparation * (1.0f - g_s3dConvergence / w);

            /* Asymmetric clamp — pop-out is not divergence.
             *
             * The far side needs no clamp: the formula approaches +sep on its
             * own, and sep is already bounded to a fusible value. The near
             * side has no such limit — it runs to -infinity as w -> 0, and
             * this renderer really does submit near-plane geometry (the logo
             * screens draw full-screen quads through the 3D path at w ~ 1.1,
             * which without this clamp shift by over three screen widths and
             * vanish). Bounding crossed disparity to a few times the
             * background keeps genuine pop-out while making that impossible. */
            const float maxPop = g_s3dSeparation * S3D_MAX_POPOUT;
            if (shiftNdc < -maxPop) {
                shiftNdc = -maxPop;
            }
        }

        /* NDC -> pre-modelview units. The modelview applies
         * x_ndc = (2 / g_screenWidth) * x / w, so going the other way costs a
         * factor of w * g_screenWidth/2. Leaving this out makes the shear 320x
         * too small, which presents as a flat image with stereo "on". */
        x += s_eyeShearDir * shiftNdc * w * ((float)g_screenWidth * 0.5f);
    }

    glVertex4f(x, v->sy * w, v->sz * w, w);
}

/* Draw one primitive immediately, bypassing the recorder. Used by the replay
 * loop, which has already restored state and set the eye shear. */
void R_ReplayPrimitive(const RenderVertex *v, int count, int layer)
{
    s_emitLayer = layer;
    glBegin(GL_TRIANGLE_FAN);
    for (int i = 0; i < count; i++) {
        R_EmitVertex(&v[i]);
    }
    glEnd();
    s_emitLayer = R_LAYER_WORLD;
}

void R_ReplayClearColor(const float rgba[4])
{
    glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    glClear(GL_COLOR_BUFFER_BIT);
}

void R_ReplayClearDepth(void)
{
    glClear(GL_DEPTH_BUFFER_BIT);
}

/* Expose the live state snapshot to the capture buffer. */
void R_StateCapture(R_StateSnapshot *dst)
{
    *dst = s_desired;
}

void R_StateRestore(const R_StateSnapshot *src)
{
    s_desired = *src;
}

/* s_in2D tags whichever draw is currently in flight as a 2D/HUD primitive.
 *
 * It cannot be inferred from the vertex data. Sonic R draws its HUD as real
 * geometry at a shallow camera depth (w ~ 2..40) through the same
 * R_DrawQuad/R_DrawTriFan path as the world (w ~ 200..10000) — there is no
 * rhw == 1 "already projected" marker to key off, and the two ranges are close
 * enough that a depth threshold would misclassify near-camera scenery. The
 * R_DrawQuad2D* helpers only cover seven call sites in screen_misc.c (the QR
 * code and matchmaker UI), nowhere near the HUD.
 *
 * So the caller declares it, by bracketing HUD drawing in R_Begin2D/R_End2D.
 * Nested because the HUD entry points call each other. */
static int s_in2D = 0;
static int s_in2DDepth = 0;

void R_Begin2D(void)
{
    s_in2DDepth++;
    s_in2D = 1;
}

void R_End2D(void)
{
    if (s_in2DDepth > 0 && --s_in2DDepth == 0) {
        s_in2D = 0;
    }
}

void R_BeginOverlay(void)
{
    s_inOverlayDepth++;
    s_inOverlay = 1;
}

void R_BeginPillarbox(void)
{
    s_inPillarboxDepth++;
    s_inPillarbox = 1;
}

void R_EndPillarbox(void)
{
    if (s_inPillarboxDepth > 0 && --s_inPillarboxDepth == 0) {
        s_inPillarbox = 0;
    }
}

void R_EndOverlay(void)
{
    if (s_inOverlayDepth > 0 && --s_inOverlayDepth == 0) {
        s_inOverlay = 0;
    }
}

/* Race-HUD scope: a tag only on GL (R_EmitVertex masks it off). */
static int s_inRaceHud = 0;
static int s_inRaceHudDepth = 0;

void R_BeginRaceHud(void)
{
    s_inRaceHudDepth++;
    s_inRaceHud = 1;
}

void R_EndRaceHud(void)
{
    if (s_inRaceHudDepth > 0 && --s_inRaceHudDepth == 0) {
        s_inRaceHud = 0;
    }
}

void R_DrawTriFan(const RenderVertex *v, int count)
{
    if (count < 3) {
        return;
    }

    /* Resolve the layer HERE, while the scopes are still open, and carry it
     * with the primitive. At replay time they are all closed. */
    const int layer = (s_inOverlay ? R_LAYER_OVERLAY
                     : (s_in2D     ? R_LAYER_HUD
                                   : R_LAYER_WORLD))
                    | (s_inPillarbox ? R_LAYER_PILLARBOX : 0)
                    | (s_inRaceHud   ? R_LAYER_RACEHUD   : 0);

    /* Stereo: hand the primitive to the recorder instead of drawing it. The
     * whole frame is replayed once per eye at present time. */
    if (g_rCaptureActive && !g_rCaptureReplaying) {
        if (R_CaptureDraw(v, count, layer)) {
            return;
        }
        /* Recorder refused (allocation failure) — fall through and draw. */
    }

    R_FlushState();

    s_emitLayer = layer;
    glBegin(GL_TRIANGLE_FAN);
    for (int i = 0; i < count; i++) {
        R_EmitVertex(&v[i]);
    }
    glEnd();
    s_emitLayer = R_LAYER_WORLD;
}

void R_DrawTri(const RenderVertex v[3])
{
    R_DrawTriFan(v, 3);
}

void R_DrawQuad(const RenderVertex v[4])
{
    R_DrawTriFan(v, 4);
}

/* =====================================================================
 * 2D quad helpers
 * ===================================================================== */

void R_DrawQuad2D(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1,
                  float z, uint32_t color)
{
    float rhw = (z > 0.0f) ? (1.0f / z) : 1.0f;
    float farSafe = (g_farClipFloat > 0.0f) ? g_farClipFloat : 1.0f;
    float normZ = z / farSafe;

    RenderVertex v[4];
    v[0] = (RenderVertex){ x0, y0, normZ, rhw, color, 0, u0, v0 };
    v[1] = (RenderVertex){ x1, y0, normZ, rhw, color, 0, u1, v0 };
    v[2] = (RenderVertex){ x1, y1, normZ, rhw, color, 0, u1, v1 };
    v[3] = (RenderVertex){ x0, y1, normZ, rhw, color, 0, u0, v1 };
    s_in2D = 1;                 /* screen-space: takes the HUD stereo shear */
    R_DrawQuad(v);
    s_in2D = 0;
}

void R_DrawQuad2DSolid(float x0, float y0, float x1, float y1,
                       float z, uint32_t color)
{
    /* Temporarily force untextured rendering */
    int savedTex = s_desired.textureId;
    s_desired.textureId = -1;

    float rhw = (z > 0.0f) ? (1.0f / z) : 1.0f;
    float farSafe = (g_farClipFloat > 0.0f) ? g_farClipFloat : 1.0f;
    float normZ = z / farSafe;

    RenderVertex v[4];
    v[0] = (RenderVertex){ x0, y0, normZ, rhw, color, 0, 0.0f, 0.0f };
    v[1] = (RenderVertex){ x1, y0, normZ, rhw, color, 0, 0.0f, 0.0f };
    v[2] = (RenderVertex){ x1, y1, normZ, rhw, color, 0, 0.0f, 0.0f };
    v[3] = (RenderVertex){ x0, y1, normZ, rhw, color, 0, 0.0f, 0.0f };
    s_in2D = 1;                 /* screen-space: takes the HUD stereo shear */
    R_DrawQuad(v);
    s_in2D = 0;

    /* Restore previous texture state */
    s_desired.textureId = savedTex;
}

/* =====================================================================
 * Texture API (forward to render_gl.c)
 * ===================================================================== */

void R_InitTextures(void)
{
    GL_InitTextures();
}

void R_UploadTexture(int tpage)
{
    GL_UploadTpage(tpage);
}

void R_MarkTextureDirty(int tpage)
{
    /* The upload this schedules will hard-set GL_NEAREST on the texture
     * object (render_gl.c:139, :247), so forget the resident filter and let
     * the next bind re-apply it. */
    if (tpage >= 0 && tpage < 52) {
        s_glTextureFilter[tpage] = 0;
    }
    GL_MarkTpageDirty(tpage);
}

void R_ClearTextureDirty(int tpage)
{
    GL_ClearTpageDirty(tpage);
}

void R_FreezeTexture(int tpage)
{
    GL_FreezeTpage(tpage);
}

void R_ThawTexture(int tpage)
{
    (void)tpage; /* GL has no persistent freeze flag */
}

void R_SetNoColorKey(int tpage)
{
    GL_SetNoColorKey(tpage);
}

void R_ClearNoColorKey(int tpage)
{
    GL_ClearNoColorKey(tpage);
}

void R_SetTpageGreen6(int tpage, int on)
{
    GL_SetTpageGreen6(tpage, on);
}

void R_SetTpageRGBA8(int tpage, unsigned char *rgba) {
    GL_SetTpageRGBA8(tpage, rgba);
}

void R_UploadTextureRGBA(int tpage, unsigned char *rgba, int w, int h)
{
    GL_UploadTpageRGBA(tpage, rgba, w, h);
}

void R_UploadTextureSubRect(int tpage, int x, int y, int w, int h)
{
    GL_UploadTpageSubRect(tpage, x, y, w, h);
}

void R_SetPendingRGBA(int tpage, unsigned char *rgba, int w, int h)
{
    GL_SetPendingRGBA(tpage, rgba, w, h);
}

void R_EndFrame(void)
{
    EndFrame();
    /* EndFrame is just glFlush — no GL state changes, tracker stays valid. */
}

void R_Flip(void)
{
    FlipD3D();
}

void R_ClearAndReset(void)
{
    ProcessTpageStates();
}

void R_ClearDepth(void)
{
    if (R_CaptureClearDepth()) {
        return;
    }
    R_FlushState();
    glClear(GL_DEPTH_BUFFER_BIT);
}

/* Colour clear, routed through the recorder so it is replayed per eye.
 * RenderBackground used to call glClearColor/glClear directly; going through
 * here is what lets each eye's framebuffer get its own clear. */
void R_ClearColor(float r, float g, float b, float a)
{
    if (R_CaptureClearColor(r, g, b, a)) {
        return;
    }
    R_FlushState();
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}

#endif /* !SONICR_SOFT_RENDER */
