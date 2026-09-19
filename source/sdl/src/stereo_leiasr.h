/**
 * stereo_leiasr.h — LeiaSR autostereoscopic 3D: runtime loader for the MSVC shim.
 *
 * The LeiaSR ("Simulated Reality") SDK ships as MSVC import libraries that
 * cannot link into this MinGW build, and its high-level GLWeaver is a C++ class
 * with virtual inheritance, so a link-time C wrapper is not an option either.
 * Everything SR-facing therefore lives in a small MSVC-built shim DLL
 * (leiasr_shim.dll, built from leiasr_shim/) exposing a flat C ABI; this is the
 * pure-C runtime loader for it. The game binary never links the SR SDK.
 *
 * If the shim DLL is absent, fails to load, or the SR runtime/hardware is not
 * present, stereoLeiaSRAvailable() returns 0 and S3D_LEIASR degrades to plain
 * Side-by-Side in r_compose_gl.c.
 *
 * Ported from perfect_dark_3D (port/src/stereo_leiasr.c).
 */
#ifndef STEREO_LEIASR_H
#define STEREO_LEIASR_H

/* Load leiasr_shim.dll and resolve its exports. Does NOT start the SR runtime:
 * srk_init is deferred to the first weave, because bringing the runtime up
 * does intrusive things to the host window (display detection, eye trackers,
 * GL hooks) that a non-LeiaSR session should not pay for. */
void stereoLeiaSRInit(void);

/* 1 once the weaver has actually initialized. Can flip back to 0 mid-session
 * when the SR display is unplugged or the service dies. */
int  stereoLeiaSRAvailable(void);

/* 1 if the shim DLL loaded and its exports resolved, regardless of whether the
 * SR runtime has been brought up yet. */
int  stereoLeiaSRShimLoaded(void);

/* 1 if the LeiaSR present path is worth taking this frame — i.e. the shim is
 * loaded AND either the runtime has not been tried yet, or it was tried and
 * works.
 *
 * This is the predicate the renderer must gate on, NOT stereoLeiaSRAvailable().
 * The SR runtime comes up lazily on the first weave (bringing it up early
 * disturbs the host window, see stereoLeiaSRInit), so Available() is still 0
 * before that first weave — gating on it means the weave never happens and the
 * runtime never initializes. Once init has been attempted and failed, this goes
 * to 0 and stays there, so a failure costs exactly one wasted compose rather
 * than one per frame. */
int  stereoLeiaSRUsable(void);

/* Hand a side-by-side packed texture to the weaver, which composites the
 * autostereo result into the currently-bound framebuffer at the current
 * viewport. Triggers the lazy runtime init on first call. No-op when
 * unavailable. */
void stereoLeiaSRWeave(unsigned int texId, int width, int height);

/* Switchable-lens panels: lens off is an ordinary sharp 2D monitor, lens on is
 * autostereoscopic. The hint is a PREFERENCE that the SR service ORs across
 * every connected application, so the lens stays on while any app wants it on.
 * That makes releasing it a correctness requirement, not politeness — an
 * integration that only ever enables leaves the panel lenticular over other
 * applications and over the desktop.
 *
 * Call once per present with "did we actually weave", not "is LeiaSR
 * selected": a frame that picked LeiaSR but fell back to SbS must release too.
 * Safe and cheap every frame — an unchanged preference never reaches the SDK,
 * and a fixed-lens panel costs one failed create rather than one per frame. */
void stereoLeiaSRSetLens(int enable);

/* Tear down the weaver and free the shim. Must run while the GL context is
 * still alive. */
void stereoLeiaSRShutdown(void);

#endif /* STEREO_LEIASR_H */
