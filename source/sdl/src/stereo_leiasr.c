/**
 * stereo_leiasr.c — LeiaSR shim DLL loader. Pure C, bound at runtime via
 * LoadLibrary. See stereo_leiasr.h for why the shim exists.
 *
 * The shim exports:
 *     int  srk_init(void *hwnd);           // 1 = ready, 0 = unavailable
 *     void srk_weave(unsigned tex, int w, int h);
 *     void srk_shutdown(void);
 *     int  srk_available(void);            // optional; older shims lack it
 *
 * srk_available is optional and reports post-init liveness: it flips to 0 when
 * the SR service dies or the display is unplugged mid-session.
 *
 * Ported from perfect_dark_3D (port/src/stereo_leiasr.c).
 */

#include <stdio.h>
#include "stereo_leiasr.h"
#include "platform.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef int  (*PFN_srk_init)(void *hwnd);
typedef void (*PFN_srk_weave)(unsigned int texId, int width, int height);
typedef void (*PFN_srk_shutdown)(void);
typedef int  (*PFN_srk_available)(void);
typedef void (*PFN_srk_set_lens)(int enable);

enum {
    LEIASR_UNINITIALIZED = 0,  /* shim not loaded yet */
    LEIASR_SHIM_LOADED,        /* DLL loaded, srk_init NOT yet called */
    LEIASR_INIT_DONE,          /* srk_init attempted; see s_available */
    LEIASR_SHUT_DOWN
};

static int s_state     = LEIASR_UNINITIALIZED;
static int s_available = 0;

#ifdef _WIN32
static HMODULE s_shim = NULL;
#else
static void   *s_shim = NULL;
#endif

static PFN_srk_init      p_init      = NULL;
static PFN_srk_weave     p_weave     = NULL;
static PFN_srk_shutdown  p_shutdown  = NULL;
static PFN_srk_available p_available = NULL;
static PFN_srk_set_lens  p_set_lens  = NULL;

static void leiaDisable(void)
{
    s_available = 0;
}

void stereoLeiaSRInit(void)
{
#ifdef _WIN32
    if (s_state != LEIASR_UNINITIALIZED) {
        return;
    }

    s_shim = LoadLibraryA("leiasr_shim.dll");
    if (!s_shim) {
        fprintf(stderr,
                "leiasr: LoadLibrary(leiasr_shim.dll) failed (err=%lu); "
                "LeiaSR mode will fall back to SbS\n",
                (unsigned long)GetLastError());
        s_state = LEIASR_INIT_DONE;
        return;
    }

    p_init     = (PFN_srk_init)     (void *)GetProcAddress(s_shim, "srk_init");
    p_weave    = (PFN_srk_weave)    (void *)GetProcAddress(s_shim, "srk_weave");
    p_shutdown = (PFN_srk_shutdown) (void *)GetProcAddress(s_shim, "srk_shutdown");
    /* Optional — absent in shims built before the liveness export existed.
     * Without it we simply never detect a mid-session display unplug. */
    p_available = (PFN_srk_available)(void *)GetProcAddress(s_shim, "srk_available");
    p_set_lens  = (PFN_srk_set_lens) (void *)GetProcAddress(s_shim, "srk_set_lens");

    if (!p_init || !p_weave || !p_shutdown) {
        fprintf(stderr, "leiasr: shim loaded but missing srk_init / srk_weave / "
                        "srk_shutdown; LeiaSR disabled.\n");
        FreeLibrary(s_shim);
        s_shim = NULL;
        p_init = NULL; p_weave = NULL; p_shutdown = NULL;
        p_available = NULL; p_set_lens = NULL;
        s_state = LEIASR_INIT_DONE;
        return;
    }

    fprintf(stderr, "leiasr: shim loaded (srk_init deferred until first weave).\n");
    s_state = LEIASR_SHIM_LOADED;
#endif
}

/* Lazy, one-shot. Runs srk_init against the game's HWND on the first weave. */
static void leiaTryInitWeaver(void)
{
#ifdef _WIN32
    if (s_state != LEIASR_SHIM_LOADED) {
        return;
    }
    s_state = LEIASR_INIT_DONE;   /* latch — only ever try once */

    void *hwnd = platform_native_window_handle();
    if (!hwnd) {
        fprintf(stderr, "leiasr: no HWND available; LeiaSR disabled.\n");
        leiaDisable();
        return;
    }

    if (p_init(hwnd)) {
        s_available = 1;
        fprintf(stderr, "leiasr: SR weaver initialized.\n");
    } else {
        fprintf(stderr, "leiasr: srk_init returned 0; falling back to SbS.\n");
        leiaDisable();
    }
#endif
}

int stereoLeiaSRAvailable(void)
{
    return s_available;
}

int stereoLeiaSRShimLoaded(void)
{
    return (s_state == LEIASR_SHIM_LOADED || s_state == LEIASR_INIT_DONE)
        && p_weave != NULL;
}

int stereoLeiaSRUsable(void)
{
    if (!stereoLeiaSRShimLoaded()) {
        return 0;
    }
    /* SHIM_LOADED = init not attempted yet, so the first weave is what brings
     * the runtime up. INIT_DONE = attempted, so defer to the outcome. */
    return (s_state == LEIASR_SHIM_LOADED) || s_available;
}

void stereoLeiaSRWeave(unsigned int texId, int width, int height)
{
    if (s_state == LEIASR_SHIM_LOADED) {
        leiaTryInitWeaver();
    }
    if (!s_available || !p_weave) {
        return;
    }
    p_weave(texId, width, height);

    /* The shim tears its weaver down and reports unavailable when weave()
     * throws — SR service crash, or the display unplugged mid-session. Drop
     * back to plain SbS rather than paying for a weave that produces nothing. */
    if (p_available && !p_available()) {
        fprintf(stderr, "leiasr: weaver went away (display unplugged or service "
                        "died); falling back to SbS.\n");
        leiaDisable();
    }
}

void stereoLeiaSRSetLens(int enable)
{
    /* Optional export — a shim built before the lens support simply won't have
     * it, and a fixed-lens panel makes it a no-op inside the shim. */
    if (p_set_lens) {
        p_set_lens(enable);
    }
}

void stereoLeiaSRShutdown(void)
{
#ifdef _WIN32
    if (s_state == LEIASR_SHUT_DOWN || s_state == LEIASR_UNINITIALIZED) {
        s_state = LEIASR_SHUT_DOWN;
        return;
    }
    if (p_shutdown) {
        p_shutdown();
    }
    if (s_shim) {
        FreeLibrary(s_shim);
        s_shim = NULL;
    }
    p_init = NULL; p_weave = NULL; p_shutdown = NULL;
    p_available = NULL; p_set_lens = NULL;
    s_available = 0;
    s_state = LEIASR_SHUT_DOWN;
#endif
}
