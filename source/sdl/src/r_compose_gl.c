/**
 * r_compose_gl.c — Stereo eye framebuffers and the compose pass.
 *
 * Owns everything between "the frame has been recorded" and "the window has
 * been presented":
 *
 *   R_StereoBeginFrame()  arm the recorder for this frame
 *   R_StereoPresent()     replay the recording once per eye into the two eye
 *                         FBOs, then compose them onto the backbuffer
 *
 * The eye FBOs are the full drawable size, NOT the 4:3 content area. The
 * game's existing letterbox/pillarbox runs inside each eye buffer exactly as
 * it does today, which is also the layout a half-SbS display expects: it
 * stretches each half back to full width, so the bars survive the round trip.
 */

#ifndef SONICR_SOFT_RENDER

#include <stdio.h>
#include <string.h>

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/glew.h>
#include <GL/gl.h>
#endif

#include "platform.h"
#include "stereo.h"
#include "r_capture.h"
#include "r_stereo_shaders.h"
#include "stereo_leiasr.h"

/* The 4:3 letterbox rect BeginFrame installs, owned by render_gl.c. The eye
 * passes must replay under exactly this viewport. */
extern int g_glViewportOffsetX;
extern int g_glViewportOffsetY;
extern int g_glBackingWidth;
extern int g_glBackingHeight;

/* ---- GL objects ---- */
static GLuint s_eyeTex[2]   = { 0, 0 };
static GLuint s_eyeFbo[2]   = { 0, 0 };
static GLuint s_eyeDepth[2] = { 0, 0 };

/* LeiaSR wants a single SbS-packed texture rather than a composed backbuffer,
 * so that mode composes into this intermediate and hands it to the weaver. */
static GLuint s_sbsTex = 0;
static GLuint s_sbsFbo = 0;

static GLuint s_prog = 0;
static GLuint s_vao  = 0;          /* core profiles require one even for a
                                    * vertexless draw; harmless in compat */
static GLint  s_uTexL = -1, s_uTexR = -1, s_uMode = -1,
              s_uOutSize = -1, s_uGhostContrast = -1, s_uGhostLift = -1;

static int s_fbW = 0, s_fbH = 0;
static int s_ready = 0;
static int s_glewOk = 0;
static int s_failed = 0;          /* sticky: don't retry a broken setup */

int R_StereoBackendReady(void)
{
    return s_ready;
}

/* ---------------------------------------------------------------------------
 * Shader plumbing
 * ------------------------------------------------------------------------- */

static GLuint compileStage(GLenum type, const char *src, const char *label)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n = 0;
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log) - 1, &n, log);
        log[n] = '\0';
        fprintf(stderr, "stereo: %s shader failed to compile:\n%s\n", label, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int buildProgram(void)
{
    GLuint vs = compileStage(GL_VERTEX_SHADER, STEREO_VS_SRC, "vertex");
    if (!vs) {
        return 0;
    }
    GLuint fs = compileStage(GL_FRAGMENT_SHADER, STEREO_FS_SRC, "fragment");
    if (!fs) {
        glDeleteShader(vs);
        return 0;
    }

    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glBindFragDataLocation(s_prog, 0, "fragColor");
    glLinkProgram(s_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n = 0;
        glGetProgramInfoLog(s_prog, (GLsizei)sizeof(log) - 1, &n, log);
        log[n] = '\0';
        fprintf(stderr, "stereo: compose program failed to link:\n%s\n", log);
        glDeleteProgram(s_prog);
        s_prog = 0;
        return 0;
    }

    s_uTexL          = glGetUniformLocation(s_prog, "uTexL");
    s_uTexR          = glGetUniformLocation(s_prog, "uTexR");
    s_uMode          = glGetUniformLocation(s_prog, "uMode");
    s_uOutSize       = glGetUniformLocation(s_prog, "uOutSize");
    s_uGhostContrast = glGetUniformLocation(s_prog, "uGhostContrast");
    s_uGhostLift     = glGetUniformLocation(s_prog, "uGhostLift");
    return 1;
}

/* ---------------------------------------------------------------------------
 * Framebuffers
 * ------------------------------------------------------------------------- */

static void destroyTargets(void)
{
    if (s_eyeFbo[0])   { glDeleteFramebuffers(2, s_eyeFbo);   s_eyeFbo[0] = s_eyeFbo[1] = 0; }
    if (s_eyeTex[0])   { glDeleteTextures(2, s_eyeTex);       s_eyeTex[0] = s_eyeTex[1] = 0; }
    if (s_eyeDepth[0]) { glDeleteRenderbuffers(2, s_eyeDepth); s_eyeDepth[0] = s_eyeDepth[1] = 0; }
    if (s_sbsFbo)      { glDeleteFramebuffers(1, &s_sbsFbo);  s_sbsFbo = 0; }
    if (s_sbsTex)      { glDeleteTextures(1, &s_sbsTex);      s_sbsTex = 0; }
}

static int createTargets(int w, int h)
{
    destroyTargets();

    glGenTextures(2, s_eyeTex);
    glGenRenderbuffers(2, s_eyeDepth);
    glGenFramebuffers(2, s_eyeFbo);

    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, s_eyeTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        /* Linear: the compose pass samples these at output UV, which is also
         * the upscale when the eye buffers are smaller than the output. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindRenderbuffer(GL_RENDERBUFFER, s_eyeDepth[i]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);

        glBindFramebuffer(GL_FRAMEBUFFER, s_eyeFbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s_eyeTex[i], 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, s_eyeDepth[i]);

        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "stereo: eye FBO %d incomplete (0x%04x)\n", i, (unsigned)st);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            destroyTargets();
            return 0;
        }
    }

    /* SbS intermediate for the LeiaSR weaver, at FULL-SbS: 2W x H, so each
     * half is a whole W x H eye.
     *
     * This is deliberately not the W x H ("half-SbS") packing the other output
     * modes use. The weaver's lenticular sampler works at the panel's per-eye
     * column rate; hand it W x H and each eye is only W/2 wide, so it
     * bilinear-upscales internally before the interleave and that softening
     * propagates into the woven image. At 2W x H each eye arrives at full
     * per-eye rate and the weaver resamples nothing.
     *
     * It costs no extra rendering here: the eye buffers are already W x H
     * each, so the compose writes them out 1:1 rather than downscaling them
     * into half-width slots. No depth attachment — this is only ever a compose
     * target. */
    glGenTextures(1, &s_sbsTex);
    glBindTexture(GL_TEXTURE_2D, s_sbsTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * 2, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &s_sbsFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_sbsFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, s_sbsTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "stereo: SbS intermediate FBO incomplete\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        destroyTargets();
        return 0;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    s_fbW = w;
    s_fbH = h;
    fprintf(stderr, "stereo: eye buffers %dx%d\n", w, h);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Init / teardown
 * ------------------------------------------------------------------------- */

void R_StereoInit(void)
{
    if (s_ready || s_failed) {
        return;
    }

    if (!s_glewOk) {
        glewExperimental = GL_TRUE;
        GLenum err = glewInit();
        /* GLEW_ERROR_NO_GLX_DISPLAY and friends are non-fatal on some
         * platforms; what actually matters is whether the entry points we use
         * resolved. */
        if (err != GLEW_OK) {
            fprintf(stderr, "stereo: glewInit warning: %s\n", glewGetErrorString(err));
        }
        if (!glCreateShader || !glGenFramebuffers) {
            fprintf(stderr, "stereo: required GL entry points unavailable; "
                            "stereo disabled.\n");
            s_failed = 1;
            return;
        }
        s_glewOk = 1;
    }

    if (!s_prog && !buildProgram()) {
        s_failed = 1;
        return;
    }

    /* A VAO must be bound for a vertexless draw under a core profile. We ask
     * for compatibility, but drivers differ, and binding one is free. */
    if (!s_vao && glGenVertexArrays) {
        glGenVertexArrays(1, &s_vao);
    }

    int w = 0, h = 0;
    platform_get_drawable_size(&w, &h);
    if (w <= 0 || h <= 0) {
        return;   /* try again next frame */
    }
    if (!createTargets(w, h)) {
        s_failed = 1;
        return;
    }

    s_ready = 1;
    stereoRefreshActive();
}

void R_StereoShutdown(void)
{
    /* Must run while the GL context is alive — the SR runtime holds GL
     * resources keyed to it. */
    stereoLeiaSRShutdown();
    destroyTargets();
    if (s_prog) { glDeleteProgram(s_prog); s_prog = 0; }
    if (s_vao && glDeleteVertexArrays) { glDeleteVertexArrays(1, &s_vao); s_vao = 0; }
    R_CaptureFree();
    s_ready = 0;
}

/* ---------------------------------------------------------------------------
 * Per-frame
 * ------------------------------------------------------------------------- */

void R_StereoBeginFrame(void)
{
    if (g_s3dMode != S3D_OFF && !s_ready && !s_failed) {
        R_StereoInit();
    }
    /* Track window resizes. */
    if (s_ready) {
        int w = 0, h = 0;
        platform_get_drawable_size(&w, &h);
        if (w > 0 && h > 0 && (w != s_fbW || h != s_fbH)) {
            if (!createTargets(w, h)) {
                s_failed = 1;
                s_ready = 0;
            }
        }
    }

    stereoRefreshActive();

    /* NOTE: the recording is NOT reset here.
     *
     * BeginFrame() is not a frame boundary in this codebase — it is called
     * several times per presented frame (RenderHUD, RenderFadeOverlay, and
     * twice in most screen loops). Resetting the command list here threw away
     * everything recorded before the last call, so only a handful of
     * primitives survived to be replayed. The real boundary is the present, so
     * R_CaptureBegin() is called at the end of R_StereoComposeFrame(). */
    g_rCaptureActive = g_s3dActive;
}

/* Draw the fullscreen triangle with the compose program bound. */
static void composeInto(GLuint fbo, int mode, int w, int h)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    glUseProgram(s_prog);

    /* SwapEyes is applied HERE and nowhere else. The shear in R_EmitVertex
     * always runs the raw left/right convention; which physical eye sees which
     * rendered image is decided by this one binding. Applying the swap in both
     * places double-swaps and looks like the control does nothing. */
    int l = g_s3dSwapEyes ? 1 : 0;
    int r = 1 - l;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_eyeTex[l]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_eyeTex[r]);

    glUniform1i(s_uTexL, 0);
    glUniform1i(s_uTexR, 1);
    glUniform1i(s_uMode, mode);
    glUniform2f(s_uOutSize, (float)w, (float)h);
    glUniform1f(s_uGhostContrast, g_s3dGhostContrast);
    glUniform1f(s_uGhostLift, g_s3dGhostLift);

    if (s_vao && glBindVertexArray) {
        glBindVertexArray(s_vao);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (s_vao && glBindVertexArray) {
        glBindVertexArray(0);
    }

    glUseProgram(0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDepthMask(GL_TRUE);
}

/* Replay the recorded frame into both eye buffers and compose the result onto
 * the default framebuffer. Returns 0 if stereo could not run, in which case
 * the caller should present whatever is already in the backbuffer. */
int R_StereoComposeFrame(void)
{
    if (!g_s3dActive || !s_ready) {
        /* Keep the arenas empty so a later switch into stereo does not replay
         * a stale half-frame. */
        R_CaptureBegin();
        return 0;
    }

    int w = s_fbW, h = s_fbH;

    /* Convergence lives in the game's camera-space Z units, which are not
     * documented anywhere — so report the range the frame actually spans.
     * Convergence should sit inside [minW, maxW]: geometry nearer than it pops
     * out of the screen, further recedes. Logged occasionally rather than every
     * frame so it stays readable while tuning with the hotkeys. */
    if (g_s3dDebugDepth) {
        static int s_statFrame = 0;
        if ((s_statFrame++ % 120) == 0) {
            float minW = 0.0f, maxW = 0.0f;
            int draws = 0, verts = 0, flat = 0, persp = 0, tagged = 0;
            R_CaptureDepthStats(&minW, &maxW, &draws, &verts);
            R_CaptureVertexCensus(&flat, &persp, &tagged);
            fprintf(stderr,
                    "stereo: w=[%.1f..%.1f] conv=%.1f | %d draws %d verts | "
                    "flat(rhw=1)=%d persp=%d tagged2D=%d\n",
                    (double)minW, (double)maxW, (double)g_s3dConvergence,
                    draws, verts, flat, persp, tagged);
        }
    }

    /* --- two eye passes ------------------------------------------------- */
    for (int eye = 0; eye < 2; eye++) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_eyeFbo[eye]);

        /* Replay under the SAME viewport the geometry was recorded against —
         * BeginFrame's 4:3 letterbox rect, not the full drawable. glViewport
         * is not part of the captured state (the game sets it once per frame
         * and never mid-frame), so it has to be re-established here. Using
         * (0,0,w,h) instead happens to be identical on a 4:3 window and
         * stretches the image on every other aspect. */
        glViewport(g_glViewportOffsetX, g_glViewportOffsetY,
                   g_glBackingWidth, g_glBackingHeight);

        /* The recording's own clear commands only cover the 4:3 content area
         * (they carry RenderBackground's scissor), so wipe the whole target
         * first or the letterbox bars keep last frame's pixels. */
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        R_CaptureReplay(eye);
    }

    /* --- compose -------------------------------------------------------- */
    int mode = g_s3dMode;

    int wove = 0;

    /* Gate on Usable(), not Available(): the SR runtime is brought up lazily by
     * the first weave, so Available() is still 0 at this point on the very
     * first LeiaSR frame. Gating on it would mean never weaving and therefore
     * never initializing. */
    if (mode == S3D_LEIASR && stereoLeiaSRUsable()) {
        /* Compose full-SbS (2W x H) into the intermediate, then weave it onto
         * the window. Ghost reduction is applied by composeInto, which is the
         * right place: BEFORE the weave, because the residual ghost is the
         * weaver's own anti-crosstalk correction overshooting and getting
         * clamped, so the headroom has to exist by the time it runs. */
        composeInto(s_sbsFbo, S3D_SBS, w * 2, h);

        /* The weave writes into whatever framebuffer and viewport are bound, so
         * both have to be the window before we hand off — composeInto just left
         * the viewport at 2W. Getting this wrong weaves at double width and
         * shows only the left half. */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, w, h);
        stereoLeiaSRWeave(s_sbsTex, w * 2, h);   /* also runs the lazy init */
        wove = stereoLeiaSRAvailable();          /* 0 if init just failed, or
                                                  * the display went away */
    }

    if (!wove) {
        /* LeiaSR selected but no shim / runtime / display, or the weave just
         * stopped working — present plain SbS. On the frame where init fails
         * this repeats the compose once; Usable() latches to 0 afterwards so it
         * does not recur. */
        if (mode == S3D_LEIASR) {
            mode = S3D_SBS;
        }
        composeInto(0, mode, w, h);
    }

    /* Switchable-lens panels: drive the lens from whether we ACTUALLY wove, not
     * from which mode is selected — a frame that picked LeiaSR and fell back to
     * SbS must release it too. The hint is a preference the SR service ORs
     * across every running application, so failing to release leaves the panel
     * lenticular over the desktop and over every other app. Cheap to call every
     * frame: an unchanged preference never reaches the SDK. */
    stereoLeiaSRSetLens(wove);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* Present IS the frame boundary — start the next recording here rather
     * than in BeginFrame, which fires several times per presented frame. */
    R_CaptureBegin();
    return 1;
}

#endif /* !SONICR_SOFT_RENDER */
