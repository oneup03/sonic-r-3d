/**
 * r_c3d_backend.c — citro3d implementation of the immediate-mode render API
 * (r_state.h, r_draw.h, r_texture.h) for the Nintendo 3DS.
 *
 * Nothing is drawn when the game calls R_DrawTriFan. Every primitive is
 * converted ONCE into the PICA vertex layout and appended to this frame's
 * vertex buffer together with the render state it was issued under (the same
 * command-list idea as sdl/src/r_capture.c). At present time
 * (r_c3d_stereo.c) the list is replayed into the left eye, the right eye and
 * the bottom screen; consecutive commands with identical state collapse into
 * one C3D_DrawArrays.
 *
 * Stereo: the clip-space shear from r_gl_backend.c's R_EmitVertex,
 *
 *     x += dir * shiftNdc * w * (W/2),   shiftNdc = separation * f(layer, w)
 *
 * is linear in the separation, so f(layer, w) * w * W/2 is stored per vertex
 * (`shift`) and the vertex shader adds `shear * shift` with
 * shear = dir * separation * slider set per target. One buffer, three draws.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r_c3d_internal.h"
#include "r_state.h"
#include "r_draw.h"
#include "r_texture.h"
#include "stereo.h"
#include "aspect.h"
#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "vshader_shbin.h"

extern void BeginFrame(void);
extern void EndFrame(void);
extern void FlipD3D(void);
extern void ProcessTpageStates(void);

/* ---------------------------------------------------------------------------
 * Vertex / command storage
 * ------------------------------------------------------------------------- */

typedef struct {
    float x, y, z, w;     /* clip space: (sx*w, sy*w, sz*w, w) */
    float u, v;
    float shift;          /* stereo disparity per unit separation (see header) */
    u8    rgba[4];
} C3dVertex;
_Static_assert(sizeof(C3dVertex) == 32, "C3dVertex must be 32 bytes");

enum { RCMD_DRAW = 0, RCMD_CLEAR_COLOR, RCMD_CLEAR_DEPTH };

typedef struct {
    int             type;
    R_StateSnapshot state;
    int             layer;
    int             first, count;
    float           clear[4];
} RCmd;

#define VBO_VERTS 65536                 /* 2 MB per buffer, two buffers */
static C3dVertex *s_vbo[2];
static int        s_vboIdx = 0;
static int        s_vtxCount = 0;

static RCmd *s_cmds = NULL;
static int   s_cmdCount = 0, s_cmdCap = 0;
static int   s_dropped = 0;
static int   s_peakVerts = 0, s_peakCmds = 0;

/* shader */
static DVLB_s          *s_dvlb = NULL;
static shaderProgram_s  s_prog;
static int              s_uProj = -1, s_uShear = -1;

/* What the GPU currently has, so a run only re-emits what changed. */
static R_StateSnapshot s_applied;
static C3D_Tex *s_appliedTex = NULL;
static unsigned char s_appliedFilter = 0;
static int s_appliedValid = 0;
static int s_appliedDepthOnly = 0;

/* per-target replay context */
static float s_shear = 0.0f;
static int   s_isBottom = 0;
static int   s_targetW = 400, s_targetH = 240;

/* ---------------------------------------------------------------------------
 * Render state tracker (same shape as r_gl_backend.c)
 * ------------------------------------------------------------------------- */

static R_StateSnapshot s_desired;

#define R_STATE_STACK_DEPTH 4
static R_StateSnapshot s_stateStack[R_STATE_STACK_DEPTH];
static int s_stackDepth = 0;

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

static unsigned char s_tpageFilter[RC3D_TPAGES];   /* 0 = none, else mode+1 */

/* scopes */
static int s_in2D = 0,        s_in2DDepth = 0;
static int s_inOverlay = 0,   s_inOverlayDepth = 0;
static int s_inPillarbox = 0, s_inPillarboxDepth = 0;
static int s_inRaceHud = 0,   s_inRaceHudDepth = 0;

/* ---------------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------------- */

static void bind_vbo(void)
{
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, s_vbo[s_vboIdx], sizeof(C3dVertex), 4, 0x3210);
}

void RC3D_Init(void)
{
    s_dvlb = DVLB_ParseFile((u32 *)vshader_shbin, vshader_shbin_size);
    shaderProgramInit(&s_prog);
    shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
    C3D_BindProgram(&s_prog);
    s_uProj  = shaderInstanceGetUniformLocation(s_prog.vertexShader, "projection");
    s_uShear = shaderInstanceGetUniformLocation(s_prog.vertexShader, "shear");

    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);          /* v0 position */
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);          /* v1 texcoord */
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 1);          /* v2 shift */
    AttrInfo_AddLoader(ai, 3, GPU_UNSIGNED_BYTE, 4);  /* v3 colour */

    for (int i = 0; i < 2; i++) {
        s_vbo[i] = (C3dVertex *)linearAlloc(VBO_VERTS * sizeof(C3dVertex));
        if (s_vbo[i] == NULL) {
            fprintf(stderr, "c3d: vertex buffer alloc failed\n");
        }
    }
    s_cmdCap = 4096;
    s_cmds = (RCmd *)malloc(sizeof(RCmd) * s_cmdCap);
    s_vboIdx = 0;
    s_vtxCount = 0;
    s_cmdCount = 0;
    bind_vbo();

    /* The game culls on the CPU and the tilt rotation would confuse winding. */
    C3D_CullFace(GPU_CULL_NONE);
    /* Clip z is (sz-1): near -> -1, far -> 0. Default map gives depth = 1-sz,
     * so "nearer wins" is GPU_GREATER/GEQUAL and the clear value is 0. */
    C3D_DepthMap(true, -1.0f, 0.0f);

    s_desired = s_defaults;
    memset(s_tpageFilter, 0, sizeof(s_tpageFilter));
}

void RC3D_Shutdown(void)
{
    for (int i = 0; i < 2; i++) {
        if (s_vbo[i]) { linearFree(s_vbo[i]); s_vbo[i] = NULL; }
    }
    free(s_cmds); s_cmds = NULL; s_cmdCap = s_cmdCount = 0;
    if (s_dvlb) {
        shaderProgramFree(&s_prog);
        DVLB_Free(s_dvlb);
        s_dvlb = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * Recording
 * ------------------------------------------------------------------------- */

static int ensure_cmd(void)
{
    if (s_cmdCount < s_cmdCap) return 1;
    int cap = s_cmdCap * 2;
    RCmd *p = (RCmd *)realloc(s_cmds, sizeof(RCmd) * cap);
    if (p == NULL) return 0;
    s_cmds = p;
    s_cmdCap = cap;
    return 1;
}

/* Per-frame constants for emit_vertex (see RC3D_RecorderReset). */
static float s_aspect2D = 1.0f;
static float s_halfW = 320.0f;
static float s_cx = 320.0f;

/* Port of R_EmitVertex (r_gl_backend.c): widescreen 2D compression and the
 * stereo disparity, with the separation factored out into `shift`. The
 * vertex is stored as (sx, sy, sz, rhw); the shader takes 1/rhw, so there
 * are no divides here. */
static inline void emit_vertex(C3dVertex *o, const RenderVertex *v, int layer)
{
    const float rhw = (v->rhw > 0.0f) ? v->rhw : 1.0f;
    float sx = v->sx;

    const int screenSpace = (rhw > 0.999999f && rhw < 1.000001f);
    const int depthLayer  = (layer & R_LAYER_MASK);
    const int overlay     = (depthLayer == R_LAYER_OVERLAY);
    const int racehud     = (layer & R_LAYER_RACEHUD) != 0;
    const int is2D = !overlay && ((layer & R_LAYER_PILLARBOX)
                                  || depthLayer == R_LAYER_HUD
                                  || screenSpace);

    /* The bottom screen is 4:3, the authored space: no compression there. */
    if (is2D && !racehud && s_aspect2D != 1.0f) {
        sx = s_cx + (sx - s_cx) * s_aspect2D;
    }

    float shiftUnit;
    if (overlay || racehud) {
        shiftUnit = 0.0f;
    } else if (depthLayer == R_LAYER_HUD) {
        shiftUnit = g_s3dHudDepth;
    } else if (screenSpace) {
        shiftUnit = S3D_BACKDROP_DEPTH;
    } else {
        shiftUnit = 1.0f - g_s3dConvergence * rhw;   /* 1 - conv/w */
        if (shiftUnit < -S3D_MAX_POPOUT) {
            shiftUnit = -S3D_MAX_POPOUT;
        }
    }

    o->x = sx;
    o->y = v->sy;
    o->z = v->sz;
    o->w = rhw;
    o->u = v->u;
    o->v = v->v;
    o->shift = shiftUnit * s_halfW;
    uint32_t c = v->color;
    o->rgba[0] = (u8)((c >> 16) & 0xFF);
    o->rgba[1] = (u8)((c >> 8) & 0xFF);
    o->rgba[2] = (u8)(c & 0xFF);
    o->rgba[3] = (u8)((c >> 24) & 0xFF);
}

void R_DrawTriFan(const RenderVertex *v, int count)
{
    if (count < 3 || v == NULL || s_vbo[s_vboIdx] == NULL) {
        return;
    }
    const int layer = (s_inOverlay ? R_LAYER_OVERLAY
                     : (s_in2D     ? R_LAYER_HUD
                                   : R_LAYER_WORLD))
                    | (s_inPillarbox ? R_LAYER_PILLARBOX : 0)
                    | (s_inRaceHud   ? R_LAYER_RACEHUD   : 0);

    /* Textures are uploaded at record time, so every target samples the same
     * texels (see r_capture.h on why that is safe here). */
    if (s_desired.textureId >= 0) {
        RC3D_TexEnsureUploaded(s_desired.textureId);
    }

    const int need = (count - 2) * 3;
    if (s_vtxCount + need > VBO_VERTS || !ensure_cmd()) {
        s_dropped++;
        return;
    }
    RCmd *c = &s_cmds[s_cmdCount++];
    c->type  = RCMD_DRAW;
    c->state = s_desired;
    c->layer = layer;
    c->first = s_vtxCount;
    c->count = need;

    C3dVertex *o = &s_vbo[s_vboIdx][s_vtxCount];
    for (int i = 1; i < count - 1; i++) {
        emit_vertex(o++, &v[0], layer);
        emit_vertex(o++, &v[i], layer);
        emit_vertex(o++, &v[i + 1], layer);
    }
    s_vtxCount += need;
}

void R_DrawTri(const RenderVertex v[3])
{
    R_DrawTriFan(v, 3);
}

void R_DrawQuad(const RenderVertex v[4])
{
    R_DrawTriFan(v, 4);
}

void R_ClearColor(float r, float g, float b, float a)
{
    if (!ensure_cmd()) return;
    RCmd *c = &s_cmds[s_cmdCount++];
    c->type = RCMD_CLEAR_COLOR;
    c->state = s_desired;
    c->layer = 0;
    c->first = c->count = 0;
    c->clear[0] = r; c->clear[1] = g; c->clear[2] = b; c->clear[3] = a;
}

void R_ClearDepth(void)
{
    if (!ensure_cmd()) return;
    RCmd *c = &s_cmds[s_cmdCount++];
    c->type = RCMD_CLEAR_DEPTH;
    c->state = s_desired;
    c->layer = 0;
    c->first = c->count = 0;
}

void RC3D_RecorderReset(void)
{
    s_aspect2D = Aspect2DScale();
    s_halfW = (float)g_screenWidth * 0.5f;
    s_cx = s_halfW;
    if (s_vtxCount > s_peakVerts) s_peakVerts = s_vtxCount;
    if (s_cmdCount > s_peakCmds)  s_peakCmds = s_cmdCount;
    s_cmdCount = 0;
    s_vtxCount = 0;
    s_vboIdx ^= 1;
    bind_vbo();
}

void RC3D_Stats(int *cmds, int *verts, int *dropped)
{
    if (cmds)    *cmds = s_peakCmds;
    if (verts)   *verts = s_peakVerts;
    if (dropped) *dropped = s_dropped;
    s_peakCmds = s_peakVerts = 0;
}

/* ---------------------------------------------------------------------------
 * Replay
 * ------------------------------------------------------------------------- */

int RC3D_TargetWidth(void)  { return s_targetW; }
int RC3D_TargetHeight(void) { return s_targetH; }

void RC3D_BeginTarget(const C3D_Mtx *projection, float shear, int isBottom)
{
    C3D_BindProgram(&s_prog);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProj, projection);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uShear, shear, 0.0f, 0.0f, 0.0f);
    s_shear = shear;
    s_isBottom = isBottom;
    s_targetW = isBottom ? 320 : 400;
    s_targetH = 240;
    s_appliedValid = 0;
    s_appliedTex = NULL;
    bind_vbo();
}

/* GL-style scissor (x, y from the bottom-left, in backing pixels of a
 * 400x240 top / 320x240 bottom image) -> rotated framebuffer rectangle. The
 * top framebuffer is 240 wide (screen Y, 0 = right... i.e. bottom row of the
 * image = column 0) by 400 tall (screen X). */
static void apply_scissor(const R_StateSnapshot *s)
{
    if (!s->scissorEnabled || s_isBottom) {
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        return;
    }
    int x0 = s->scissorX, y0 = s->scissorY;
    int x1 = x0 + s->scissorW, y1 = y0 + s->scissorH;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > s_targetW) x1 = s_targetW;
    if (y1 > s_targetH) y1 = s_targetH;
    if (x1 <= x0 || y1 <= y0) {
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 0, 0);
        return;
    }
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)y0, (u32)x0, (u32)y1, (u32)x1);
}

static GPU_TESTFUNC depth_func(R_DepthFunc f)
{
    switch (f) {
        case R_DEPTH_LESS:   return GPU_GREATER;
        case R_DEPTH_ALWAYS: return GPU_ALWAYS;
        case R_DEPTH_LEQUAL:
        default:             return GPU_GEQUAL;
    }
}

static void apply_state(const R_StateSnapshot *s, int depthOnly)
{
    const int full = !s_appliedValid;
    const R_StateSnapshot *a = &s_applied;

    C3D_Tex *tex = NULL;
    if (s->textureId >= 0 && s->textureId < RC3D_TPAGES) {
        tex = RC3D_TexBindable(s->textureId);
    }
    unsigned char want = 0;
    if (tex != NULL) {
        want = s_tpageFilter[s->textureId];
        if (want == 0) want = (unsigned char)(s->filter + 1);
    }
    if (full || tex != s_appliedTex || s->texEnv != a->texEnv || (tex && want != s_appliedFilter)) {
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        if (tex != NULL) {
            GPU_TEXTURE_FILTER_PARAM f = (want == (unsigned char)(R_FILTER_LINEAR + 1)) ? GPU_LINEAR : GPU_NEAREST;
            C3D_TexSetFilter(tex, f, f);
            C3D_TexBind(0, tex);
            C3D_TexEnvInit(env);
            C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            if (s->texEnv == R_TEXENV_ADD_SIGNED) {
                C3D_TexEnvFunc(env, C3D_RGB, GPU_ADD_SIGNED);
                C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
            } else {
                C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
            }
        } else {
            C3D_TexEnvInit(env);
            C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        }
        s_appliedTex = tex;
        s_appliedFilter = want;
    }

    if (depthOnly) {
        C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_DEPTH);
    } else if (full || s_appliedDepthOnly || s->depthTest != a->depthTest ||
               s->depthFunc != a->depthFunc || s->depthWrite != a->depthWrite) {
        C3D_DepthTest(s->depthTest ? true : false, depth_func(s->depthFunc),
                      s->depthWrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
    }

    if (full || s->blendMode != a->blendMode) {
        switch (s->blendMode) {
            case R_BLEND_NONE:
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
                break;
            case R_BLEND_ADDITIVE:
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
                break;
            case R_BLEND_ALPHA:
            default:
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
                break;
        }
    }

    if (full || s->alphaTest != a->alphaTest || s->alphaRef != a->alphaRef) {
        int ref = (int)(s->alphaRef * 255.0f + 0.5f);
        if (ref < 0) ref = 0;
        if (ref > 255) ref = 255;
        C3D_AlphaTest(s->alphaTest ? true : false, GPU_GREATER, ref);
    }

    if (full || s->scissorEnabled != a->scissorEnabled ||
        (s->scissorEnabled && (s->scissorX != a->scissorX || s->scissorY != a->scissorY ||
                               s->scissorW != a->scissorW || s->scissorH != a->scissorH))) {
        apply_scissor(s);
    }

    s_applied = *s;
    s_appliedValid = 1;
    s_appliedDepthOnly = depthOnly;
}

/* Append an untextured full-screen quad (virtual space) and return its index. */
static int append_quad(float x0, float y0, float x1, float y1, float sz, const u8 rgba[4])
{
    if (s_vtxCount + 6 > VBO_VERTS) return -1;
    C3dVertex *o = &s_vbo[s_vboIdx][s_vtxCount];
    const float xs[6] = { x0, x1, x1, x0, x1, x0 };
    const float ys[6] = { y0, y0, y1, y0, y1, y1 };
    for (int i = 0; i < 6; i++) {
        o[i].x = xs[i]; o[i].y = ys[i]; o[i].z = sz; o[i].w = 1.0f;
        o[i].u = 0.0f; o[i].v = 0.0f; o[i].shift = 0.0f;
        memcpy(o[i].rgba, rgba, 4);
    }
    int first = s_vtxCount;
    s_vtxCount += 6;
    return first;
}

static void replay_clear(const RCmd *c)
{
    R_StateSnapshot st = c->state;
    st.textureId = -1;
    st.blendMode = R_BLEND_NONE;
    st.alphaTest = 0;
    u8 rgba[4] = { 0, 0, 0, 255 };
    float sz = 0.0f;
    if (c->type == RCMD_CLEAR_COLOR) {
        st.depthTest = 0;
        st.depthWrite = 0;
        rgba[0] = (u8)(c->clear[0] * 255.0f + 0.5f);
        rgba[1] = (u8)(c->clear[1] * 255.0f + 0.5f);
        rgba[2] = (u8)(c->clear[2] * 255.0f + 0.5f);
        rgba[3] = (u8)(c->clear[3] * 255.0f + 0.5f);
    } else {
        sz = 1.0f;   /* far plane */
    }
    int first = append_quad(0.0f, 0.0f, (float)g_screenWidth, (float)g_screenHeight, sz, rgba);
    if (first < 0) return;
    apply_state(&st, c->type == RCMD_CLEAR_DEPTH);
    C3D_DrawArrays(GPU_TRIANGLES, first, 6);
}

void RC3D_Replay(int racehud)
{
    int runFirst = 0, runCount = 0;
    const R_StateSnapshot *runState = NULL;

    for (int i = 0; i < s_cmdCount; i++) {
        const RCmd *c = &s_cmds[i];
        if (c->type == RCMD_DRAW) {
            const int isHud = (c->layer & R_LAYER_RACEHUD) != 0;
            if (isHud != racehud) {
                continue;
            }
            if (runCount > 0 && c->first == runFirst + runCount &&
                memcmp(&c->state, runState, sizeof(R_StateSnapshot)) == 0) {
                runCount += c->count;
                continue;
            }
            if (runCount > 0) {
                apply_state(runState, 0);
                C3D_DrawArrays(GPU_TRIANGLES, runFirst, runCount);
            }
            runFirst = c->first;
            runCount = c->count;
            runState = &c->state;
        } else {
            if (racehud) {
                continue;   /* the bottom target is cleared by the compose pass */
            }
            if (runCount > 0) {
                apply_state(runState, 0);
                C3D_DrawArrays(GPU_TRIANGLES, runFirst, runCount);
                runCount = 0;
            }
            replay_clear(c);
        }
    }
    if (runCount > 0) {
        apply_state(runState, 0);
        C3D_DrawArrays(GPU_TRIANGLES, runFirst, runCount);
    }
}

/* Immediate quads for the bottom panel, drawn on top of whatever was replayed
 * there. Untextured quads are batched into one draw so the text overlay costs
 * a couple of draw calls rather than one per glyph pixel. */
static int s_immFirst = -1, s_immCount = 0;

static void imm_state(int tpage)
{
    R_StateSnapshot st = s_defaults;
    st.textureId = tpage;
    st.blendMode = R_BLEND_ALPHA;
    st.depthTest = 0;
    st.depthWrite = 0;
    st.alphaTest = 0;
    st.scissorEnabled = 0;
    apply_state(&st, 0);
}

static void imm_flush_untextured(void)
{
    if (s_immCount > 0) {
        imm_state(-1);
        C3D_DrawArrays(GPU_TRIANGLES, s_immFirst, s_immCount);
    }
    s_immFirst = -1;
    s_immCount = 0;
}

void RC3D_ImmQuad(float x0, float y0, float x1, float y1, uint32_t argb)
{
    u8 rgba[4] = { (u8)(argb >> 16), (u8)(argb >> 8), (u8)argb, (u8)(argb >> 24) };
    int first = append_quad(x0, y0, x1, y1, 0.5f, rgba);
    if (first < 0) return;
    if (s_immCount == 0) s_immFirst = first;
    s_immCount += 6;
}

void RC3D_ImmTexQuad(int tpage, float x0, float y0, float x1, float y1,
                     float u0, float v0, float u1, float v1, uint32_t argb)
{
    imm_flush_untextured();
    u8 rgba[4] = { (u8)(argb >> 16), (u8)(argb >> 8), (u8)argb, (u8)(argb >> 24) };
    int first = append_quad(x0, y0, x1, y1, 0.5f, rgba);
    if (first < 0) return;
    C3dVertex *o = &s_vbo[s_vboIdx][first];
    const float us[6] = { u0, u1, u1, u0, u1, u0 };
    const float vs[6] = { v0, v0, v1, v0, v1, v1 };
    for (int i = 0; i < 6; i++) { o[i].u = us[i]; o[i].v = vs[i]; }
    imm_state(tpage);
    C3D_DrawArrays(GPU_TRIANGLES, first, 6);
}

void RC3D_ImmFlush(void)
{
    imm_flush_untextured();
}

/* ---------------------------------------------------------------------------
 * State API
 * ------------------------------------------------------------------------- */

void R_FlushState(void)
{
    /* State is applied at replay from the recorded snapshot; the only
     * immediate work is making sure a bound texture has its pixels up. */
    if (s_desired.textureId >= 0) {
        RC3D_TexEnsureUploaded(s_desired.textureId);
    }
}

void R_SetTexture(int tpageIndex)      { s_desired.textureId = tpageIndex; }
void R_SetBlendMode(R_BlendMode mode)  { s_desired.blendMode = mode; }
void R_SetDepthTest(int enable)        { s_desired.depthTest = enable; }
void R_SetDepthFunc(R_DepthFunc func)  { s_desired.depthFunc = func; }
void R_SetDepthWrite(int enable)       { s_desired.depthWrite = enable; }
void R_SetTexEnv(R_TexEnvMode mode)    { s_desired.texEnv = mode; }
void R_SetFilter(R_FilterMode mode)    { s_desired.filter = mode; }
void R_SetCullMode(R_CullMode mode)    { s_desired.cullMode = mode; }
void R_SetAlphaTest(int enable)        { s_desired.alphaTest = enable; }
void R_SetAlphaRef(float ref)          { s_desired.alphaRef = ref; }

void R_SetTpageFilter(int tpage, R_FilterMode mode)
{
    if (tpage >= 0 && tpage < RC3D_TPAGES) s_tpageFilter[tpage] = (unsigned char)(mode + 1);
}

void R_ClearTpageFilter(int tpage)
{
    if (tpage >= 0 && tpage < RC3D_TPAGES) s_tpageFilter[tpage] = 0;
}

void R_SetTpageSatBoost(int tpage, int k256)
{
    (void)tpage; (void)k256;   /* ADD_SIGNED is native on the PICA */
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
    if (s_stackDepth < R_STATE_STACK_DEPTH) s_stateStack[s_stackDepth++] = s_desired;
}

void R_PopState(void)
{
    if (s_stackDepth > 0) s_desired = s_stateStack[--s_stackDepth];
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
    s_stackDepth = 0;
}

void R_StateCapture(R_StateSnapshot *dst)       { *dst = s_desired; }
void R_StateRestore(const R_StateSnapshot *src) { s_desired = *src; }

/* ---------------------------------------------------------------------------
 * Scopes
 * ------------------------------------------------------------------------- */

void R_Begin2D(void)        { s_in2DDepth++; s_in2D = 1; }
void R_End2D(void)          { if (s_in2DDepth > 0 && --s_in2DDepth == 0) s_in2D = 0; }
void R_BeginOverlay(void)   { s_inOverlayDepth++; s_inOverlay = 1; }
void R_EndOverlay(void)     { if (s_inOverlayDepth > 0 && --s_inOverlayDepth == 0) s_inOverlay = 0; }
void R_BeginPillarbox(void) { s_inPillarboxDepth++; s_inPillarbox = 1; }
void R_EndPillarbox(void)   { if (s_inPillarboxDepth > 0 && --s_inPillarboxDepth == 0) s_inPillarbox = 0; }
void R_BeginRaceHud(void)   { s_inRaceHudDepth++; s_inRaceHud = 1; }
void R_EndRaceHud(void)     { if (s_inRaceHudDepth > 0 && --s_inRaceHudDepth == 0) s_inRaceHud = 0; }

/* ---------------------------------------------------------------------------
 * 2D quad helpers
 * ------------------------------------------------------------------------- */

void R_DrawQuad2D(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1,
                  float z, uint32_t color)
{
    float rhw = (z > 0.0f) ? (1.0f / z) : 1.0f;
    float farSafe = (g_farClipFloat > 0.0f) ? g_farClipFloat : 1.0f;
    float normZ = z / farSafe;
    RenderVertex v[4] = {
        { x0, y0, normZ, rhw, color, 0, u0, v0 },
        { x1, y0, normZ, rhw, color, 0, u1, v0 },
        { x1, y1, normZ, rhw, color, 0, u1, v1 },
        { x0, y1, normZ, rhw, color, 0, u0, v1 },
    };
    int saved = s_in2D;
    s_in2D = 1;
    R_DrawQuad(v);
    s_in2D = saved;
}

void R_DrawQuad2DSolid(float x0, float y0, float x1, float y1,
                       float z, uint32_t color)
{
    int savedTex = s_desired.textureId;
    s_desired.textureId = -1;
    float rhw = (z > 0.0f) ? (1.0f / z) : 1.0f;
    float farSafe = (g_farClipFloat > 0.0f) ? g_farClipFloat : 1.0f;
    float normZ = z / farSafe;
    RenderVertex v[4] = {
        { x0, y0, normZ, rhw, color, 0, 0.0f, 0.0f },
        { x1, y0, normZ, rhw, color, 0, 0.0f, 0.0f },
        { x1, y1, normZ, rhw, color, 0, 0.0f, 0.0f },
        { x0, y1, normZ, rhw, color, 0, 0.0f, 0.0f },
    };
    int saved = s_in2D;
    s_in2D = 1;
    R_DrawQuad(v);
    s_in2D = saved;
    s_desired.textureId = savedTex;
}

/* ---------------------------------------------------------------------------
 * Texture API (forward to r_c3d_texture.c)
 * ------------------------------------------------------------------------- */

void R_InitTextures(void)                                  { RC3D_TexInit(); }
void R_UploadTexture(int tpage)                            { RC3D_TexUpload(tpage); }
void R_MarkTextureDirty(int tpage)                         { RC3D_TexMarkDirty(tpage); }
void R_ClearTextureDirty(int tpage)                        { RC3D_TexClearDirty(tpage); }
void R_FreezeTexture(int tpage)                            { if (g_tpagePixelBuf[tpage]) RC3D_TexUpload(tpage); }
void R_ThawTexture(int tpage)                              { (void)tpage; }
void R_SetNoColorKey(int tpage)                            { RC3D_TexSetNoColorKey(tpage, 1); }
void R_ClearNoColorKey(int tpage)                          { RC3D_TexSetNoColorKey(tpage, 0); }
void R_SetTpageGreen6(int tpage, int on)                   { RC3D_TexSetGreen6(tpage, on); }
void R_SetTpageRGBA8(int tpage, unsigned char *rgba)       { RC3D_TexSetRGBA8(tpage, rgba); }
void R_UploadTextureRGBA(int tpage, unsigned char *rgba, int w, int h) { RC3D_TexUploadRGBA(tpage, rgba, w, h); }
void R_UploadTextureSubRect(int tpage, int x, int y, int w, int h)     { RC3D_TexSubRect(tpage, x, y, w, h); }
void R_SetPendingRGBA(int tpage, unsigned char *rgba, int w, int h)    { RC3D_TexSetPendingRGBA(tpage, rgba, w, h); }

/* ---------------------------------------------------------------------------
 * Frame lifecycle
 * ------------------------------------------------------------------------- */

void R_EndFrame(void)      { EndFrame(); }
void R_Flip(void)          { FlipD3D(); }
void R_ClearAndReset(void) { ProcessTpageStates(); }
