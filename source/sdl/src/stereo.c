/**
 * stereo.c — Stereoscopic 3D settings, defaults and persistence.
 *
 * See stereo.h for the clip-space separation/convergence parameterization and
 * why it collapses the geometry work to one line in R_EmitVertex.
 */

#include <stdio.h>
#include <string.h>
#if !defined(SONICR_DC) && !defined(SONICR_3DS)
#include <SDL.h>
#endif
#include "stereo.h"
#include "aspect.h"

int   g_s3dMode          = S3D_OFF;

/* Objects at infinity sit this fraction of the screen width apart, so 0.050 is
 * 5%. Comfortably inside the fusion ceiling, which lands near IPD/screen_width
 * (~0.105 on a 27" 16:9) — past that the eyes are asked to diverge. */
float g_s3dSeparation    = S3D_SEPARATION_DEF;

/* Camera-space Z of the zero-disparity (screen) plane.
 *
 * The scale here is Sonic R's own: 2D/HUD quads sit at z = 10 (HUD_DEPTH_LARGE)
 * and the far plane lands at g_farClipFloat = g_farClipDepth << 3, which is
 * 8184 at full detail (g_clipFar 0x3FF, g_clipNear 0xFF — see
 * track_init.c InitFarClipAndFog).
 *
 * Measured with --stereo-debug-depth, a race spans w = 2 .. 10005 with the near
 * track around 200-500, and the menus span w = 340 .. 3000. The default sits
 * just below the near track, so most of the scene recedes behind the screen and
 * only the closest geometry comes forward — the comfortable arrangement for a
 * chase-camera racer.
 *
 * Still a starting point rather than a per-track calibration — tune it live
 * with the convergence hotkeys, and use --stereo-debug-depth to see the range
 * a given track actually spans. */
float g_s3dConvergence   = S3D_CONVERGENCE_DEF;

int   g_s3dSwapEyes      = 0;
/* Slightly back from the screen plane rather than on it: the HUD then sits
 * behind the nearest track geometry instead of intersecting it, which reads as
 * a layer over the scene rather than one fighting it. */
float g_s3dHudDepth      = S3D_HUD_DEPTH_DEF;
float g_s3dGhostContrast = 1.0f;   /* off */
float g_s3dGhostLift     = 0.0f;   /* off */

int   g_s3dActive        = 0;
int   g_s3dDebugDepth    = 0;

/* Set by the GL side once eye FBOs and the compose program exist. */
extern int R_StereoBackendReady(void);

static const char *s_modeNames[S3D_MODE_COUNT] = {
    "off", "sbs", "tab", "row", "col", "checker", "anaglyph", "leiasr"
};

const char *stereoModeName(int mode)
{
    if (mode < 0 || mode >= S3D_MODE_COUNT) {
        return "?";
    }
    return s_modeNames[mode];
}

int stereoModeFromName(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (int i = 0; i < S3D_MODE_COUNT; i++) {
        if (strcmp(name, s_modeNames[i]) == 0) {
            return i;
        }
    }
    /* Friendly aliases for the names people actually type. */
    if (strcmp(name, "side-by-side") == 0 || strcmp(name, "sidebyside") == 0) return S3D_SBS;
    if (strcmp(name, "top-and-bottom") == 0 || strcmp(name, "ou") == 0)       return S3D_TAB;
    if (strcmp(name, "interlaced") == 0)                                       return S3D_ROW;
    if (strcmp(name, "checkerboard") == 0)                                     return S3D_CHECKER;
    if (strcmp(name, "redcyan") == 0 || strcmp(name, "red-cyan") == 0)         return S3D_ANAGLYPH;
    if (strcmp(name, "leia") == 0 || strcmp(name, "sr") == 0)                  return S3D_LEIASR;
    return -1;
}

void stereoClampSettings(void)
{
    if (g_s3dMode < 0 || g_s3dMode >= S3D_MODE_COUNT) {
        g_s3dMode = S3D_OFF;
    }
    if (g_s3dSeparation < 0.0f)                  g_s3dSeparation = 0.0f;
    if (g_s3dSeparation > S3D_SEPARATION_MAX)    g_s3dSeparation = S3D_SEPARATION_MAX;
    /* Convergence has to stay inside the scene's own depth range — see
     * S3D_CONVERGENCE_MIN. Below it the whole world flattens to a constant
     * offset and only the HUD shows any depth, which reads as "3D is broken"
     * rather than "convergence is low". */
    if (g_s3dConvergence < S3D_CONVERGENCE_MIN)  g_s3dConvergence = S3D_CONVERGENCE_MIN;
    if (g_s3dConvergence > S3D_CONVERGENCE_MAX)  g_s3dConvergence = S3D_CONVERGENCE_MAX;
    if (g_s3dHudDepth < -1.0f)                   g_s3dHudDepth = -1.0f;
    if (g_s3dHudDepth > 1.0f)                    g_s3dHudDepth = 1.0f;
    if (g_s3dGhostContrast < 0.5f)               g_s3dGhostContrast = 0.5f;
    if (g_s3dGhostContrast > 1.0f)               g_s3dGhostContrast = 1.0f;
    if (g_s3dGhostLift < 0.0f)                   g_s3dGhostLift = 0.0f;
    if (g_s3dGhostLift > 0.2f)                   g_s3dGhostLift = 0.2f;
    g_s3dSwapEyes = (g_s3dSwapEyes != 0);
}

void stereoRefreshActive(void)
{
    int want = (g_s3dMode != S3D_OFF) && R_StereoBackendReady();
    if (want != g_s3dActive) {
        fprintf(stderr, "stereo: %s (mode=%s sep=%.3f conv=%.1f)\n",
                want ? "ON" : "OFF", stereoModeName(g_s3dMode),
                (double)g_s3dSeparation, (double)g_s3dConvergence);
    }
    g_s3dActive = want;
}

void stereoInit(void)
{
    stereoClampSettings();
    stereoRefreshActive();
}

void stereoShutdown(void)
{
    g_s3dActive = 0;
}

int stereoNumEyes(void)
{
    return g_s3dActive ? 2 : 1;
}

float stereoShearDir(int eye)
{
    /* Left eye shears -x, right eye +x.
     *
     * Derivation, because getting this backwards produces an image that still
     * looks "3D" and is merely wrong — every depth cue inverted, distant
     * scenery floating in front of near geometry:
     *
     *   shift(eye) = dir(eye) * separation * (1 - convergence/w)
     *
     *   w -> infinity: the bracket tends to +1. Two eyes looking at something
     *   infinitely far away have parallel lines of sight, so on screen the LEFT
     *   eye's copy must sit to the LEFT of the right eye's, separated by the
     *   eye baseline (uncrossed disparity). That requires dir(left) < 0.
     *
     *   w == convergence: the bracket is 0, both eyes agree, and the geometry
     *   lands exactly on the screen plane. Correct under either sign.
     *
     *   w < convergence: the bracket goes negative, so with dir(left) < 0 the
     *   left copy moves RIGHT of the right copy — crossed disparity, i.e. in
     *   front of the screen. Which is what "nearer than the screen plane"
     *   should look like.
     *
     * SwapEyes is applied at compose time only — see the header. */
    return (eye == 0) ? -1.0f : 1.0f;
}

int stereoEyeViewport(int drawableW, int drawableH, int *eyeW, int *eyeH)
{
    int w = drawableW;
    int h = drawableH;
    int split = 0;

    /* Keyed off the SELECTED mode, not g_s3dActive: the eye buffers are sized
     * during stereo init, before the active flag has been raised, so gating on
     * the flag would size the first set of buffers for the wrong layout. */
    if (g_s3dMode == S3D_SBS && drawableW > 0 && drawableH > 0) {
        const float panel = (float)drawableW / (float)drawableH;
        if (panel >= 2.0f * ASPECT_MIN_RATIO) {
            w = drawableW / 2;
            split = 1;
        }
    }

    if (eyeW) *eyeW = (w > 0) ? w : 1;
    if (eyeH) *eyeH = (h > 0) ? h : 1;
    return split;
}

float stereoMaxShiftNdc(void)
{
    if (!g_s3dActive) {
        return 0.0f;
    }
    /* The far side settles at exactly separation; the near side is bounded by
     * the pop-out clamp, which is the larger of the two. Backdrops sit at
     * S3D_BACKDROP_DEPTH * separation, inside that. */
    float pop = g_s3dSeparation * S3D_MAX_POPOUT;
    return (pop > g_s3dSeparation) ? pop : g_s3dSeparation;
}

/* ---------------------------------------------------------------------------
 * Live tuning hotkeys
 *
 * Separation and convergence cannot sensibly be chosen from a table: the right
 * values depend on the panel, its size, and how far away the player sits, and
 * the only way to find them is to change them while looking at the 3D image.
 * So they get hotkeys, and the options menu follows later.
 *
 * Every change logs the resulting value, so a setting arrived at by feel can be
 * written down and passed on the command line or kept via SONICR.INF.
 * ------------------------------------------------------------------------- */

#if !defined(SONICR_DC) && !defined(SONICR_3DS)
/* Does this scancode belong to us? Used to swallow rate-limited repeats without
 * letting them reach the game's key mapping. Must stay in step with the switch
 * in stereoHandleHotkey. */
int stereoIsHotkey(int scancode)
{
    switch (scancode) {
        case SDL_SCANCODE_F1:  case SDL_SCANCODE_F2:
        case SDL_SCANCODE_F3:  case SDL_SCANCODE_F4:
        case SDL_SCANCODE_F5:  case SDL_SCANCODE_F6:
            return 1;
        default:
            return 0;
    }
}

static void logSettings(const char *what)
{
    fprintf(stderr, "stereo: %-11s mode=%s sep=%.3f conv=%.1f swap=%d hud=%+.2f "
                    "ghost=%.2f/%.2f\n",
            what, stereoModeName(g_s3dMode), (double)g_s3dSeparation,
            (double)g_s3dConvergence, g_s3dSwapEyes, (double)g_s3dHudDepth,
            (double)g_s3dGhostContrast, (double)g_s3dGhostLift);
}

/* Minimum gap between accepted auto-repeats, in ms.
 *
 * Holding a key has to ramp, not teleport. Passing OS auto-repeat straight
 * through sweeps separation across its whole range in a fraction of a second
 * (and convergence, being multiplicative, several thousand-fold) — unusable for
 * dialling a value in. Rate-limiting here rather than relying on the key-repeat
 * delay also makes the feel independent of the user's Windows repeat settings.
 *
 * ~14 steps/sec: separation crosses its 0..0.15 range in about 5s, convergence
 * moves ~2.7x/sec. Fast enough not to feel like mashing, slow enough to stop
 * where you meant to. */
#define S3D_REPEAT_INTERVAL_MS 70

int stereoHandleHotkey(int scancode, int isRepeat)
{
    if (isRepeat) {
        /* Every remaining hotkey is a continuous value, so they all ramp; the
         * discrete actions that had to refuse repeats (mode, swap eyes) now
         * live on the options page instead. */
        static Uint32 s_lastRepeat = 0;
        Uint32 now = SDL_GetTicks();
        /* Consume the event either way — returning 0 would leak the key
         * through to the game's own binding for it. */
        if (now - s_lastRepeat < S3D_REPEAT_INTERVAL_MS) {
            return stereoIsHotkey(scancode);
        }
        s_lastRepeat = now;
    }

    switch (scancode) {
        /* HUD depth: -1 pops the HUD fully forward, 0 pins it on the screen
         * plane, +1 puts it at the background disparity (as far away as the
         * sky). Worth having on the front keys — it is the setting you retune
         * whenever separation or convergence moves, because where the HUD
         * wants to sit depends on where the rest of the scene ended up. */
        case SDL_SCANCODE_F1:
            g_s3dHudDepth -= S3D_HUD_DEPTH_STEP;
            stereoClampSettings();
            logSettings("hud-");
            return 1;
        case SDL_SCANCODE_F2:
            g_s3dHudDepth += S3D_HUD_DEPTH_STEP;
            stereoClampSettings();
            logSettings("hud+");
            return 1;

        case SDL_SCANCODE_F3:
            g_s3dSeparation -= S3D_SEPARATION_STEP;
            stereoClampSettings();
            logSettings("sep-");
            return 1;
        case SDL_SCANCODE_F4:
            g_s3dSeparation += S3D_SEPARATION_STEP;
            stereoClampSettings();
            logSettings("sep+");
            return 1;

        /* Convergence steps multiplicatively: perceived depth goes with the
         * ratio, so a fixed increment is far too coarse near the camera and
         * far too fine out at the background. */
        case SDL_SCANCODE_F5:
            g_s3dConvergence /= S3D_CONVERGENCE_STEP;
            stereoClampSettings();
            logSettings("conv-");
            return 1;
        case SDL_SCANCODE_F6:
            g_s3dConvergence *= S3D_CONVERGENCE_STEP;
            stereoClampSettings();
            logSettings("conv+");
            return 1;

        /* F7-F12 deliberately unbound. Output mode, swap eyes and ghost
         * reduction moved to the Graphics options page (stereo_menu.c): they
         * are chosen once for a display rather than dialled in per scene, so
         * they do not need to be reachable mid-race. F11/F12 would be poor
         * choices anyway — Windows and attached debuggers intercept them often
         * enough that a binding there reads as broken rather than absent. */
        default:
            return 0;
    }
}

#endif /* hotkeys: desktop only */

/* ---------------------------------------------------------------------------
 * Persistence
 *
 * SONICR.INF is a 40-int array of which the game assigns buf[0..31]; buf[32..39]
 * are already written to disk as zero (save.c clears the tail), so they are
 * free real estate that costs no format change.
 *
 * Floats are stored as scaled ints so the file stays an int array and remains
 * byte-compatible with the original format. Zeroed slots (a pre-stereo INF)
 * must therefore decode to the DEFAULTS, not to zero — hence the sentinel
 * check on slot 32.
 * ------------------------------------------------------------------------- */
#define S3D_INF_BASE   32
/* 'S3D2'. Bumped from 'S3D1' when the shipped defaults changed: an INF written
 * by an older build holds values tuned against the old ones, and silently
 * keeping them would mean the new defaults never reached anyone who had already
 * run the game. A version bump makes the slots read as uninitialized once, so
 * the defaults land and are then re-persisted under the new tag. */
#define S3D_INF_MAGIC  0x53334432

void stereoConfigLoad(const int *buf, int count)
{
    if (buf == NULL || count < S3D_INF_BASE + 8) {
        return;
    }
    if (buf[S3D_INF_BASE + 0] != S3D_INF_MAGIC) {
        /* Pre-stereo INF, or one from a build with different defaults: keep the
         * compiled-in values. */
        return;
    }

    int packed = buf[S3D_INF_BASE + 1];
    g_s3dMode        = packed & 0xFF;
    g_s3dSwapEyes    = (packed >> 8) & 1;

    g_s3dSeparation    = (float)buf[S3D_INF_BASE + 2] / 100000.0f;
    g_s3dConvergence   = (float)buf[S3D_INF_BASE + 3] / 100.0f;
    /* An out-of-range stored convergence means a previous session left it
     * somewhere unusable (older builds allowed values far below the scene's
     * depth range, which flattens the image completely). Snap back to the
     * default rather than to the nearest edge — the edge would still look
     * wrong, and the user would reasonably read that as the fix not working. */
    if (g_s3dConvergence < S3D_CONVERGENCE_MIN
        || g_s3dConvergence > S3D_CONVERGENCE_MAX) {
        fprintf(stderr, "stereo: stored convergence %.1f is outside the usable "
                        "range [%.0f..%.0f]; resetting to %.0f\n",
                (double)g_s3dConvergence, (double)S3D_CONVERGENCE_MIN,
                (double)S3D_CONVERGENCE_MAX, (double)S3D_CONVERGENCE_DEF);
        g_s3dConvergence = S3D_CONVERGENCE_DEF;
    }
    g_s3dHudDepth      = (float)buf[S3D_INF_BASE + 4] / 10000.0f;
    /* HUD depth only became effective once the HUD was actually tagged as
     * overlay; values stored before that were never applied and are not
     * meaningful. +-1.0 in particular parks the HUD at full background
     * disparity, which is jarring. Treat a saturated stored value as
     * uncalibrated and start from the screen plane. */
    if (g_s3dHudDepth <= -0.999f || g_s3dHudDepth >= 0.999f) {
        g_s3dHudDepth = S3D_HUD_DEPTH_DEF;
    }
    g_s3dGhostContrast = (float)buf[S3D_INF_BASE + 5] / 10000.0f;
    g_s3dGhostLift     = (float)buf[S3D_INF_BASE + 6] / 10000.0f;
    /* buf[S3D_INF_BASE + 7] reserved */

    stereoClampSettings();
}

void stereoConfigSave(int *buf, int count)
{
    if (buf == NULL || count < S3D_INF_BASE + 8) {
        return;
    }
    buf[S3D_INF_BASE + 0] = S3D_INF_MAGIC;
    buf[S3D_INF_BASE + 1] = (g_s3dMode & 0xFF) | ((g_s3dSwapEyes & 1) << 8);
    buf[S3D_INF_BASE + 2] = (int)(g_s3dSeparation    * 100000.0f);
    buf[S3D_INF_BASE + 3] = (int)(g_s3dConvergence   * 100.0f);
    buf[S3D_INF_BASE + 4] = (int)(g_s3dHudDepth      * 10000.0f);
    buf[S3D_INF_BASE + 5] = (int)(g_s3dGhostContrast * 10000.0f);
    buf[S3D_INF_BASE + 6] = (int)(g_s3dGhostLift     * 10000.0f);
    buf[S3D_INF_BASE + 7] = 0;
}
