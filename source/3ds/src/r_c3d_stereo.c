/**
 * r_c3d_stereo.c — render targets, the hardware 3D slider and the compose
 * pass. Implements r_compose.h for the 3DS.
 *
 * The desktop build composes two eye textures with a GLSL pass; here each
 * eye is its own framebuffer, so "compose" is just replaying the recorded
 * frame into the left target, the right target (when the slider is up) and
 * the bottom screen (race HUD + touch panel).
 */

#include <stdio.h>
#include <string.h>

#include "r_c3d_internal.h"
#include "r_compose.h"
#include "stereo.h"
#include "bottom_panel.h"
#include "sonicr_globals.h"

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

/* Full slider = this many times the "3D DEPTH MAX" setting (g_s3dSeparation,
 * whose desktop default of 0.05 reads as shallow on the 3DS panel). */
#define S3D_3DS_SLIDER_GAIN 2.0f

float g_rc3dSlider = 0.0f;
int   g_rc3dFps = 0;          /* presented frames over the last second */
int   g_rc3dStatGame = 0, g_rc3dStatWait = 0, g_rc3dStatReplay = 0, g_rc3dStatSleep = 0;
int   g_rc3dUploadsPerSec = 0;
u64   g_rc3dSleepTicks = 0;   /* accumulated by platform_sleep_ms since the last present */
extern int g_rc3dUploadCount;  /* r_c3d_texture.c */

static C3D_RenderTarget *s_top[2] = { NULL, NULL };
static C3D_RenderTarget *s_bottom = NULL;
static C3D_Mtx s_projTop, s_projBottom;
static int s_ready = 0;
static int s_3dOn = 1;
static int s_frame = 0;

void R_StereoInit(void)
{
    if (s_ready) {
        return;
    }
    s_top[0] = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    s_top[1] = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    s_bottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!s_top[0] || !s_top[1] || !s_bottom) {
        fprintf(stderr, "c3d: render target creation failed\n");
        return;
    }
    C3D_RenderTargetSetOutput(s_top[0], GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    C3D_RenderTargetSetOutput(s_top[1], GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);
    C3D_RenderTargetSetOutput(s_bottom, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    /* The game's 640x480 virtual space fills each screen; y grows downward;
     * sz in [0,1] lands in the PICA's [-1,0] clip range. */
    Mtx_OrthoTilt(&s_projTop, 0.0f, 640.0f, 480.0f, 0.0f, 0.0f, 1.0f, true);
    Mtx_OrthoTilt(&s_projBottom, 0.0f, 640.0f, 480.0f, 0.0f, 0.0f, 1.0f, true);

    s_ready = 1;
    g_s3dMode = S3D_SBS;      /* the only mode that exists here */
    stereoRefreshActive();
}

void R_StereoShutdown(void)
{
    if (!s_ready) {
        return;
    }
    C3D_RenderTargetDelete(s_top[0]);
    C3D_RenderTargetDelete(s_top[1]);
    C3D_RenderTargetDelete(s_bottom);
    s_top[0] = s_top[1] = s_bottom = NULL;
    s_ready = 0;
}

int R_StereoBackendReady(void)
{
    return s_ready;
}

void R_StereoBeginFrame(void)
{
    if (!s_ready) {
        R_StereoInit();
    }
    g_rc3dSlider = osGet3DSliderState();
    g_s3dActive = (s_ready && g_rc3dSlider > 0.0f) ? 1 : 0;
}

int RC3D_PresentReady(void)
{
    return s_ready;
}

/* The rewind happens after C3D_FrameBegin, which has waited for the GPU to
 * finish the previous frame, so a prompt frame never overwrites vertices the
 * GPU is still reading. The last prompt frame's vertices are left in place
 * for the same reason: the game's next recorded draws go after them. */
void RC3D_PresentBottomOnly(void (*draw)(void), int mark)
{
    if (!s_ready) {
        return;
    }
    C3D_FrameBegin(0);
    RC3D_ImmRewind(mark);
    C3D_RenderTargetClear(s_bottom, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(s_bottom);
    RC3D_BeginTarget(&s_projBottom, 0.0f, 1);
    draw();
    RC3D_ImmFlush();
    C3D_FrameEnd(0);
}

int R_StereoComposeFrame(void)
{
    if (!s_ready) {
        return 0;
    }
    const float slider = g_rc3dSlider;
    const int stereo = slider > 0.001f;
    if (stereo != s_3dOn) {
        gfxSet3D(stereo ? true : false);
        s_3dOn = stereo;
    }
    const float sep = g_s3dSeparation * slider * S3D_3DS_SLIDER_GAIN;
    const int l = g_s3dSwapEyes ? 1 : 0;
    const int r = 1 - l;

    /* No SYNCDRAW: that would align every frame start to a 60 Hz vblank and
     * snap a 35 ms frame to 50. The 30 fps cap is WaitForFrameCap's job; the
     * display transfer still lands on a vblank. This only blocks while the
     * GPU is still on the previous frame. */
    const u64 t0 = svcGetSystemTick();
    C3D_FrameBegin(0);
    const u64 t1 = svcGetSystemTick();

    C3D_RenderTargetClear(s_top[0], C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(s_top[0]);
    RC3D_BeginTarget(&s_projTop, stereo ? stereoShearDir(l) * sep : 0.0f, 0);
    RC3D_Replay(0);

    if (stereo) {
        C3D_RenderTargetClear(s_top[1], C3D_CLEAR_ALL, 0x000000FF, 0);
        C3D_FrameDrawOn(s_top[1]);
        RC3D_BeginTarget(&s_projTop, stereoShearDir(r) * sep, 0);
        RC3D_Replay(0);
    }

    C3D_RenderTargetClear(s_bottom, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(s_bottom);
    RC3D_BeginTarget(&s_projBottom, 0.0f, 1);
    RC3D_Replay(1);
    BottomPanel_Draw();
    RC3D_ImmFlush();

    C3D_FrameEnd(0);
    const u64 t2 = svcGetSystemTick();

    RC3D_RecorderReset();
    RC3D_TexEndFrame();

    /* Presented-frame rate and where the frame went, refreshed once a second.
     * Ticks run at SYSCLOCK_ARM11 whatever the CPU clock. game = everything
     * outside this function that is not sleeping in WaitForFrameCap. */
    {
        static u64 winStart = 0, prevFlip = 0;
        static u64 sumFrame = 0, sumWait = 0, sumReplay = 0, sumSleep = 0;
        static int winFrames = 0;
        if (prevFlip != 0) {
            sumFrame  += t0 - prevFlip;
            sumWait   += t1 - t0;
            sumReplay += t2 - t1;
            sumSleep  += g_rc3dSleepTicks;
            winFrames++;
        }
        prevFlip = t0;
        g_rc3dSleepTicks = 0;
        u64 now = osGetTime();
        if (winStart == 0) winStart = now;
        if (now - winStart >= 1000 && winFrames > 0) {
            const double toMs = 1000.0 / (double)SYSCLOCK_ARM11 / (double)winFrames;
            int frameMs  = (int)(sumFrame * toMs + 0.5);
            g_rc3dStatWait   = (int)(sumWait * toMs + 0.5);
            g_rc3dStatReplay = (int)(sumReplay * toMs + 0.5);
            g_rc3dStatSleep  = (int)(sumSleep * toMs + 0.5);
            g_rc3dStatGame   = frameMs - g_rc3dStatWait - g_rc3dStatReplay - g_rc3dStatSleep;
            if (g_rc3dStatGame < 0) g_rc3dStatGame = 0;
            g_rc3dFps = (int)((u64)winFrames * 1000 / (now - winStart));
            g_rc3dUploadsPerSec = g_rc3dUploadCount;
            g_rc3dUploadCount = 0;
            winStart = now;
            winFrames = 0;
            sumFrame = sumWait = sumReplay = sumSleep = 0;
        }
    }

    if ((++s_frame % 300) == 0) {
        int cmds = 0, verts = 0, dropped = 0;
        RC3D_Stats(&cmds, &verts, &dropped);
        fprintf(stderr, "c3d: frame %d %d fps: game %d wait %d replay %d sleep %d ms, %d uploads/s, peak %d cmds %d verts, dropped %d, slider %.2f\n",
                s_frame, g_rc3dFps, g_rc3dStatGame, g_rc3dStatWait, g_rc3dStatReplay, g_rc3dStatSleep,
                g_rc3dUploadsPerSec, cmds, verts, dropped, (double)slider);
    }
    return 1;
}
