//-----------------------------------------------------------------------------
// leiasr_shim — MSVC shim around the LeiaSR (Simulated Reality) OpenGL weaver.
//
// Why this DLL exists
// -------------------
// Sonic R builds with MinGW64 and cannot link the SR SDK directly: the GL
// weaver is exposed as SR::IGLWeaver1, a C++ class using *virtual inheritance*,
// and MSVC lays v-pointers / vbtables / adjuster fields out differently from
// the Itanium ABI MinGW follows. Symptoms range from link errors to silent
// vtable misalignment at runtime. No amount of extern "C" or .def files fixes
// that — the mismatch is in the layout the caller must walk to dispatch.
//
// So this is the smallest possible MSVC translation unit that owns the C++
// side. It links SR-lib, holds the SRInterfaceOGL, and exports a flat C ABI
// over types both ABIs agree on (int, void*, GLuint). The game loads it with
// LoadLibrary at runtime; no SR symbol ever reaches the game's import table,
// so a machine without the SR runtime — or without this DLL — just falls back
// to plain Side-by-Side.
//
// What is deliberately NOT hand-rolled here
// -----------------------------------------
// SR-lib's SR.cpp wrapper (libs/SR-lib, api_expansion branch) owns the SR
// lifecycle, and it is the shipped, hardware-tested version of what this file
// would otherwise reimplement:
//
//   - SRContext::create -> CreateGLWeaver -> ctx->initialize(), in that order.
//     Getting that order wrong is the single most-misdiagnosed LeiaSR bug:
//     every call still returns success and weave() still runs, but the
//     lenticular interleave uses default no-track eye coordinates, so the panel
//     shows an image that never responds to head movement.
//   - LoadLibraryW probes of both SimulatedRealityCore.dll and the backend
//     weaver DLL before any SR call, so a missing delay-loaded DLL surfaces as
//     a clean failure rather than an SEH that C++ try/catch cannot catch.
//   - create()/deleteSRContext() pairing — the context lives in the SR DLL, so
//     a plain delete frees it on the wrong heap.
//   - ServerNotAvailableException and friends contained behind an HRESULT.
//
// What stays here is only what the wrapper cannot know about: the flat C ABI,
// the SetInputTexture cache, the switchable-lens policy, and teardown-on-failure.
//-----------------------------------------------------------------------------

// Must precede every include. SR.hpp pulls windows.h but deliberately does NOT
// include SDK headers — and it is the SDK's display.h that would otherwise
// define this for us. Without it, windows.h's min/max macros break std::min /
// std::max in this TU with a baffling "C2589: illegal token on right side of ::".
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdio>
#include <exception>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4251 4275 4267)
#endif

#include "SR.hpp"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

namespace
{
    SimulatedReality::SRInterfaceOGL *g_sr      = nullptr;
    unsigned                          g_lastTex = 0;
    bool                              g_lensOn  = false;

    // Apply a lens preference and log the transitions.
    //
    // S_OK means the preference actually CHANGED and was sent to the service;
    // S_FALSE means it was already in that state and nothing was sent. Logging
    // only S_OK is what makes "did the lens ever get released?" answerable from
    // the log — an integration that enables and never releases is invisible in
    // its own window and very visible on everyone else's.
    void apply_lens(bool want)
    {
        try
        {
            HRESULT hr = want ? SimulatedReality::SREnableLensHint()
                              : SimulatedReality::SRDisableLensHint();
            if (hr == S_OK)
            {
                std::fprintf(stderr, "leiasr_shim: lens %s\n", want ? "enabled" : "released");
                std::fflush(stderr);
            }
        }
        catch (...)
        {
            // Fixed-lens panel or no context: E_NOINTERFACE, not an error.
        }
        g_lensOn = want;
    }

    // Release the lens before tearing the context down, then drop the
    // interface. Callable from an exception handler; must not itself throw.
    void teardown()
    {
        try
        {
            // The lens hint is a Sense OWNED BY the SRContext — it dies with
            // the context, so releasing it afterwards is use-after-free. Order
            // matters: disable, then Delete.
            if (g_lensOn)
            {
                apply_lens(false);
            }
            if (g_sr) { g_sr->Delete(); g_sr = nullptr; }
        }
        catch (...)
        {
            g_sr = nullptr;
            g_lensOn = false;
        }
        g_lastTex = 0;
    }

    // Re-assert per-monitor-v2 awareness on whichever thread drives SR init.
    // The game declares it process-wide before SDL comes up, but the weave only
    // lands 1:1 on physical panel pixels if the thread creating the SR context
    // is aware too — and a silent mismatch shows up as a blurry weave on a
    // scaled display with nothing in the log to point at it.
    void assert_thread_dpi_awareness()
    {
        HMODULE user32 = LoadLibraryW(L"user32.dll");
        if (!user32) {
            return;
        }

        typedef DPI_AWARENESS_CONTEXT (WINAPI *PFN_SetThreadCtx)(DPI_AWARENESS_CONTEXT);
        typedef DPI_AWARENESS_CONTEXT (WINAPI *PFN_GetThreadCtx)(void);
        typedef DPI_AWARENESS         (WINAPI *PFN_FromCtx)(DPI_AWARENESS_CONTEXT);

        PFN_SetThreadCtx pSet = (PFN_SetThreadCtx)(void *)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
        PFN_GetThreadCtx pGet = (PFN_GetThreadCtx)(void *)GetProcAddress(user32, "GetThreadDpiAwarenessContext");
        PFN_FromCtx     pFrom = (PFN_FromCtx)    (void *)GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");

        if (pSet) {
            pSet(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
        if (pGet && pFrom) {
            const char *s = "unknown";
            switch (pFrom(pGet())) {
                case DPI_AWARENESS_UNAWARE:           s = "unaware"; break;
                case DPI_AWARENESS_SYSTEM_AWARE:      s = "system"; break;
                case DPI_AWARENESS_PER_MONITOR_AWARE: s = "per-monitor"; break;
                default:                              s = "invalid"; break;
            }
            std::fprintf(stderr, "leiasr_shim: SR init thread DPI awareness = %s\n", s);
        }
        FreeLibrary(user32);
    }
}

extern "C" __declspec(dllexport)
int srk_init(void *hwnd)
{
    if (g_sr != nullptr)
        return 1; // Already up.

    assert_thread_dpi_awareness();

    // CreateSRInterfaceOGL binds the weaver to whatever GL context is current
    // on this thread, so the caller must have one — the game calls us from the
    // render thread at first weave, which satisfies that.
    //
    // It returns E_NOINTERFACE for every "SR isn't usable here" case: DLLs not
    // installed (its own probe fails), SR service not running
    // (ServerNotAvailable), no weaving-capable display, incompatible GL
    // context. All mean the same thing to us, so we don't distinguish. /EHa
    // plus this catch is the outer belt for anything that still escapes as an
    // SEH — acceptable here precisely because this whole TU is the SR shim, so
    // /EHa's wider catch(...) has no non-SR code in scope to affect.
    HRESULT hr = E_FAIL;
    try
    {
        hr = SimulatedReality::CreateSRInterfaceOGL(static_cast<HWND>(hwnd), &g_sr);
    }
    catch (...)
    {
        std::fprintf(stderr, "leiasr_shim: CreateSRInterfaceOGL raised; LeiaSR unavailable.\n");
        teardown();
        return 0;
    }

    if (FAILED(hr) || g_sr == nullptr)
    {
        std::fprintf(stderr, "leiasr_shim: CreateSRInterfaceOGL failed (hr=0x%08lx); "
                             "LeiaSR unavailable.\n", (unsigned long)hr);
        teardown();
        return 0;
    }

    std::fprintf(stderr, "leiasr_shim: SR weaver initialized.\n");
    std::fflush(stderr);
    return 1;
}

extern "C" __declspec(dllexport)
void srk_weave(unsigned int tex_id, int width, int height)
{
    // width/height are part of the ABI but not forwarded: SR-lib's
    // SetInputTexture queries GL_TEXTURE_WIDTH / _HEIGHT / _INTERNAL_FORMAT off
    // the texture object itself. That removes the classic failure mode of the
    // caller's declared dimensions disagreeing with the texture's real ones
    // (the weaver samples exactly width x height texels and produces garbage
    // when lied to), and picks up the real internal format rather than a
    // hardcoded guess. Kept in the signature so an older game binary still
    // links against a newer shim.
    (void)width;
    (void)height;

    if (g_sr == nullptr || tex_id == 0)
        return;

    try
    {
        // SetInputTexture re-binds the weaver's sampling source and re-queries
        // the texture, which on some SR versions reinitializes internal state.
        // The game hands us the same texture every frame once the SbS
        // intermediate settles, so cache on the id and skip the call.
        if (tex_id != g_lastTex)
        {
            g_sr->SetInputTexture(tex_id);
            g_lastTex = tex_id;
        }
        g_sr->Weave();
    }
    // Weave() throws when the SR service dies or the user unplugs the SR
    // display mid-session. Retrying every frame from here on would throw every
    // frame forever, so tear down instead: srk_available() then reports 0 and
    // the game drops back to the plain SbS compose.
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "leiasr_shim: weave threw: %s — disabling LeiaSR "
                             "for this session.\n", e.what());
        std::fflush(stderr);
        teardown();
    }
    catch (...)
    {
        std::fprintf(stderr, "leiasr_shim: weave threw unknown exception — "
                             "disabling LeiaSR for this session.\n");
        std::fflush(stderr);
        teardown();
    }
}

// Post-init liveness check. Distinct from srk_init's return value: this can
// flip back to 0 mid-session when the SR display is unplugged or the service
// dies (see srk_weave). Optional from the game's point of view — a host built
// against an older shim simply won't find this export.
extern "C" __declspec(dllexport)
int srk_available(void)
{
    return g_sr != nullptr;
}

// Switchable-lens panels: lens off is an ordinary sharp 2D monitor, lens on is
// autostereoscopic. The hint is a *preference* that the SR service ORs across
// every connected application, so the lens stays on while any app wants it on —
// which makes releasing it a correctness requirement, not politeness. An
// integration that only ever enables leaves the panel lensed over every other
// output mode, over other applications, and over the desktop.
//
// The game calls this once per present with "did we actually weave", not "is
// LeiaSR selected": a frame that picked LeiaSR but fell back to SbS must
// release too. Cheap to call every frame — an unchanged preference never
// reaches the SDK, and a fixed-lens panel costs one failed create, not one per
// frame (E_NOINTERFACE, which is normal there and not an error).
extern "C" __declspec(dllexport)
void srk_set_lens(int enable)
{
    const bool want = (enable != 0);

    if (g_sr == nullptr && want)
        return;   // no context to enable a lens against

    // No dedup on our side: SR-lib returns S_FALSE without sending anything
    // when the preference is unchanged, and latches E_NOINTERFACE after the
    // first failed create on a fixed-lens panel. So calling this every frame is
    // intended and costs nothing.
    apply_lens(want);
}

extern "C" __declspec(dllexport)
void srk_shutdown(void)
{
    teardown();
}
