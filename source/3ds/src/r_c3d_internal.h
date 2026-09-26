/**
 * r_c3d_internal.h — shared between the citro3d backend files.
 *
 *   r_c3d_backend.c   render state, the frame recorder and per-target replay
 *   r_c3d_texture.c   tpage -> C3D_Tex management (convert, swizzle, upload)
 *   r_c3d_stereo.c    render targets, slider, compose (R_Stereo* hooks)
 *   render_c3d.c      game-facing frame functions (BeginFrame, FlipD3D, ...)
 */
#ifndef R_C3D_INTERNAL_H
#define R_C3D_INTERNAL_H

#include <3ds.h>
#include <citro3d.h>
#include "r_types.h"
#include "r_state_internal.h"

#define RC3D_TPAGES 52

/* Set to 1 if the SEGA logo comes out upside down (see c3d_swizzle.h). */
#define RC3D_TEX_FLIP 1

/* Not in r_draw.h but called by shared code (hud_full.c). */
void R_ClearDepth(void);

/* --- backend (r_c3d_backend.c) --- */
void RC3D_Init(void);            /* after C3D_Init: shader, attributes, VBOs */
void RC3D_Shutdown(void);
void RC3D_BeginTarget(const C3D_Mtx *projection, float shear, int isBottom);
/* Replay the recorded frame: racehud=0 draws everything but the race HUD,
 * racehud=1 draws only the race HUD (no clears, no scissor). */
void RC3D_Replay(int racehud);
void RC3D_RecorderReset(void);   /* at present: next frame records afresh */
/* Untextured screen-space quad drawn immediately into the current target
 * (for the bottom panel). Coordinates in the 640x480 virtual space. */
void RC3D_ImmQuad(float x0, float y0, float x1, float y1, uint32_t argb);
void RC3D_ImmTexQuad(int tpage, float x0, float y0, float x1, float y1,
                     float u0, float v0, float u1, float v1, uint32_t argb);
void RC3D_ImmFlush(void);
int  RC3D_TargetWidth(void);     /* backing size of the target being drawn */
int  RC3D_TargetHeight(void);
void RC3D_Stats(int *cmds, int *verts, int *dropped);

/* --- textures (r_c3d_texture.c) --- */
void     RC3D_TexInit(void);
C3D_Tex *RC3D_TexBindable(int tpage);      /* uploads if dirty; NULL if no pixels */
void     RC3D_TexEnsureUploaded(int tpage);
void     RC3D_TexMarkDirty(int tpage);
void     RC3D_TexMarkAllDirty(void);
void     RC3D_TexClearDirty(int tpage);
void     RC3D_TexFreeSlot(int tpage);       /* tpage state 6 */
void     RC3D_TexUpload(int tpage);
void     RC3D_TexUploadRGBA(int tpage, unsigned char *rgba, int w, int h);
void     RC3D_TexSubRect(int tpage, int x, int y, int w, int h);
void     RC3D_TexSetPendingRGBA(int tpage, unsigned char *rgba, int w, int h);
void     RC3D_TexSetRGBA8(int tpage, unsigned char *rgba);
void     RC3D_TexSetNoColorKey(int tpage, int on);
void     RC3D_TexSetGreen6(int tpage, int on);
void     RC3D_TexKeepPixels(int tpage);
void     RC3D_TexEndFrame(void);            /* drain deferred frees */

/* --- stereo / targets (r_c3d_stereo.c) --- */
extern float g_rc3dSlider;                  /* osGet3DSliderState() this frame */
extern int   g_rc3dFps;                     /* presented frames per second */
extern int   g_rc3dStatGame, g_rc3dStatWait, g_rc3dStatReplay, g_rc3dStatSleep;   /* ms per frame */
extern int   g_rc3dUploadsPerSec;
extern u64   g_rc3dSleepTicks;
extern int   g_rc3dCpuMhz;                  /* platform_3ds.c boot estimate */
extern char  g_rc3dSpeedupInfo[48];
extern int   g_rc3dUploadCount;

#endif
