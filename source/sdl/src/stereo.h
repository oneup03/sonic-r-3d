/**
 * stereo.h — Stereoscopic 3D settings and per-eye math.
 *
 * Naming: everything here is prefixed g_s3d* / S3D_*. Do NOT confuse with
 * g_stereoEnabled (SONICR.INF buf[20]), which is the AUDIO mono/stereo toggle
 * and predates this by 27 years.
 *
 * ---------------------------------------------------------------------------
 * The parameterization
 *
 * Clip-space separation/convergence, i.e. the NVIDIA / 3Dmigoto convention:
 *
 *     x_clip += dir * separation * (w_clip - convergence)
 *
 * This renderer hands GL pre-transformed screen-space vertices with rhw = 1/Z,
 * and R_EmitVertex submits glVertex4f(sx*w, sy*w, sz*w, w) with w = Z. So
 * x_clip and w_clip are already exactly what the formula wants and the whole
 * stereo geometry transform is one line in R_EmitVertex — no projection
 * matrix to shear, no camera to move, and no FoV compensation anywhere (the
 * clip-space form is FoV-independent by construction).
 *
 * separation is a FRACTION OF SCREEN WIDTH of background disparity: NDC x
 * spans [-1,+1] across the screen, and the shear settles at +/- separation as
 * w -> infinity, so 0.04 puts objects at infinity 4% of the screen width
 * apart. That also states the comfort ceiling in the same units — uncrossed
 * disparity wider than the viewer's own IPD forces the eyes to diverge and
 * cannot be fused at any setting, which lands near IPD/screen_width (~0.105 on
 * a 27" 16:9). Hence the 0..0.15 range.
 *
 * convergence is in the game's camera-space Z units (the same units as
 * RenderVertex.rhw's reciprocal). Geometry at exactly this depth has zero
 * disparity and sits on the screen plane; nearer geometry pops out.
 * ---------------------------------------------------------------------------
 */
#ifndef STEREO_H
#define STEREO_H

typedef enum {
    S3D_OFF = 0,
    S3D_SBS,           /* side-by-side, half width per eye */
    S3D_TAB,           /* top-and-bottom */
    S3D_ROW,           /* row interlaced */
    S3D_COL,           /* column interlaced */
    S3D_CHECKER,       /* checkerboard (DLP-Link style) */
    S3D_ANAGLYPH,      /* red/cyan, Dubois matrix */
    S3D_LEIASR,        /* autostereo via the SR weaver; falls back to SBS */
    S3D_MODE_COUNT
} S3DMode;

/* ---- User settings (persisted to SONICR.INF buf[32..39]) ---- */
extern int   g_s3dMode;          /* S3DMode */
extern float g_s3dSeparation;    /* 0 .. S3D_SEPARATION_MAX */
extern float g_s3dConvergence;   /* camera-space Z units; > 0 */
extern int   g_s3dSwapEyes;      /* applied at COMPOSE time only */
extern float g_s3dHudDepth;      /* -1..+1 fraction of background disparity */
extern float g_s3dGhostContrast; /* 1.0 = off */
extern float g_s3dGhostLift;     /* 0.0 = off */

#define S3D_SEPARATION_MAX   0.15f
#define S3D_SEPARATION_DEF   0.050f
#define S3D_HUD_DEPTH_DEF    0.2f

/* Maximum crossed (pop-out) disparity, as a multiple of separation. The far
 * side self-limits at exactly separation, but the near side runs to infinity
 * as w -> 0, and this renderer does submit near-plane geometry through the 3D
 * path. See the clamp in R_EmitVertex. */
#define S3D_MAX_POPOUT       3.0f

/* Usable convergence range, in the game's camera-space Z units.
 *
 * Measured with --stereo-debug-depth during a race: world geometry spans
 * w = 2 .. 10000, with the near track around 200-500 and the far distance
 * pinned at 10005. The HUD sits at w = 2..40.
 *
 * The lower bound matters more than it looks. Convergence far below the world's
 * depth range makes sep*(1 - conv/w) ~ sep for EVERY world vertex — a constant
 * offset, i.e. a completely flat image — while the HUD, being the only thing at
 * a comparable depth, becomes the only element with any stereo variation at
 * all. 50 keeps convergence inside the range where it still separates world
 * depths. */
#define S3D_CONVERGENCE_MIN  50.0f
/* Must stay BELOW the sky's depth. render_track_d3d.c submits the backdrop at
 * SKY_RHW = 0.0000999450, i.e. w = 10005 — effectively infinity. Convergence
 * above that makes (1 - conv/w) negative for the sky, so the backdrop acquires
 * crossed disparity and comes forward OUT of the screen, in front of the track
 * it is supposed to sit behind. 5000 keeps the sky comfortably uncrossed at any
 * setting while still allowing convergence far past any real track geometry. */
#define S3D_CONVERGENCE_MAX  5000.0f
#define S3D_CONVERGENCE_DEF  252.0f
#define S3D_SEPARATION_STEP  0.002f
#define S3D_CONVERGENCE_STEP 1.08f   /* multiplicative: depth is perceived log-ish */
#define S3D_HUD_DEPTH_STEP   0.1f    /* 20 steps across the -1..+1 range */

/* Where full-screen 2D layers sit, as a fraction of background disparity.
 * 1.0 = infinity, i.e. exactly the disparity the sky gets.
 *
 * These are backdrops, so they read most comfortably at the depth the eyes
 * would converge on for a distant scene: nothing is asked to sit in front of
 * the screen, and the smaller 3D objects and HUD elements drawn over them
 * (which land at HUD depth or nearer) come out correctly ordered in front.
 * Distinct from HUD depth on purpose — the HUD is read, a backdrop is looked
 * past. */
#define S3D_BACKDROP_DEPTH   1.0f

/* Nonzero once the GL side is up and a stereo mode is selected. Everything
 * hot-path checks this single flag; when it is 0 the renderer takes the exact
 * pre-stereo code path. */
extern int g_s3dActive;

/* Log the per-frame camera-space depth range (--stereo-debug-depth). The units
 * convergence is expressed in are the game's own and undocumented, so this is
 * how you find the range it has to sit inside for a given track. */
extern int g_s3dDebugDepth;

void  stereoInit(void);
void  stereoShutdown(void);

/* Recompute g_s3dActive from the mode and GL readiness. Call after any change
 * to g_s3dMode. */
void  stereoRefreshActive(void);

/* Number of passes this frame: 2 when active, else 1. */
int   stereoNumEyes(void);

/* Shear direction for eye 0 (left) / 1 (right).
 *
 * ONE convention for all stereo math: left = +1, right = -1. SwapEyes is
 * deliberately NOT honoured here — it is applied once, at compose time, when
 * choosing which eye texture goes where. Applying it in both places
 * double-swaps and looks like it does nothing. */
float stereoShearDir(int eye);

/* Largest magnitude, in NDC, that the shear can move a vertex horizontally at
 * the current separation. 0 when stereo is off.
 *
 * The game culls and generates geometry against screen-space clip bounds BEFORE
 * the shear is applied at submit time, so those bounds have to be widened by
 * this much (converted to virtual pixels) or the shear reveals a strip at the
 * screen edge that nothing was ever drawn into. */
float stereoMaxShiftNdc(void);

/* Live tuning hotkeys, handled before the game's key mapping.
 * Takes an SDL scancode; returns 1 if the key was consumed.
 *
 *   F1/F2  HUD depth -/+
 *   F3/F4  separation -/+
 *   F5/F6  convergence -/+ (multiplicative)
 *
 * These three are the ones with no right answer in the abstract: separation and
 * convergence depend on the panel and the viewing distance, and HUD depth
 * depends on where those two put the rest of the scene. They have to be
 * adjustable while looking at the 3D image, so they get keys; each change logs
 * the resulting values so a setting found by feel can be written down.
 *
 * All three are continuous and ramp when held, rate-limited internally so the
 * feel does not depend on the user's Windows key-repeat settings.
 *
 * Everything else lives on the Graphics options page (stereo_menu.c). Output
 * mode, swap eyes and ghost reduction are chosen once and left alone, so they
 * do not earn a key each. F7-F12 are deliberately unbound — and F11/F12 would
 * be poor choices regardless, since Windows and attached debuggers intercept
 * them often enough that a binding there reads as broken rather than absent. */
int   stereoHandleHotkey(int scancode, int isRepeat);

/* 1 if this scancode is one of ours. Lets the caller swallow rate-limited
 * auto-repeats instead of leaking them into the game's key mapping. */
int   stereoIsHotkey(int scancode);

/* Clamp/normalize after a hotkey or config load. */
void  stereoClampSettings(void);

/* Persistence — called from save.c with the SONICR.INF int array. */
void  stereoConfigLoad(const int *buf, int count);
void  stereoConfigSave(int *buf, int count);

/* Mode name for logging / menus. */
const char *stereoModeName(int mode);

/* Parse a --stereo=<name> argument. Returns -1 if unrecognized. */
int   stereoModeFromName(const char *name);

#endif /* STEREO_H */
