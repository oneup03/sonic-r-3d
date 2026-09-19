# leiasr_shim

MSVC-built bridge DLL that lets the MinGW-built Sonic R drive a **LeiaSR**
(Simulated Reality) autostereoscopic display.

## Why it exists

The SR SDK is MSVC-compiled, and its OpenGL weaver is exposed as
`SR::IGLWeaver1` — a C++ class using **virtual inheritance**. MSVC lays
v-pointers, vbtables and adjuster fields out differently from the Itanium ABI
MinGW follows, so the game cannot call into the SR libraries directly. That is
not something `extern "C"` or a `.def` file can paper over: the mismatch is in
the layout the caller has to walk to dispatch a virtual call.

So this DLL is the smallest possible MSVC translation unit that owns the C++
side, exposing a flat C ABI over types both ABIs agree on:

```c
int  srk_init(void *hwnd);                          // 1 = ready, 0 = unavailable
void srk_weave(unsigned tex_id, int w, int h);      // SbS texture -> panel
int  srk_available(void);                           // post-init liveness
void srk_set_lens(int enable);                      // switchable-lens hint
void srk_shutdown(void);
```

The game resolves these with `LoadLibrary` + `GetProcAddress` at runtime
(`source/sdl/src/stereo_leiasr.c`). **No SR symbol ever reaches the game's
import table**, so a machine without the SR runtime — or without this DLL —
simply falls back to plain Side-by-Side.

## Prerequisites

- **Visual Studio 2022** with the "Desktop development with C++" workload
  (Community edition is fine), and **CMake**.
- The **SR-lib submodule**, which carries the SDK:

  ```sh
  git submodule update --init --recursive
  ```

  `bo3b/SR-lib` is a public repo — this needs no credentials. The `api_expansion`
  branch is pinned because it ships the CMake package and the `SRInterfaceOGL`
  wrapper that `master` does not.

- To actually *see* 3D you also need the **LeiaSR Platform runtime** installed
  and an SR display connected. Neither is needed to build.

## Building

From VSCode: run the **Build + Deploy LeiaSR shim** task.

By hand:

```sh
cmake -S leiasr_shim -B leiasr_shim/build -A x64
cmake --build leiasr_shim/build --config Release
cp leiasr_shim/build/Release/leiasr_shim.dll /c/Apps/SonicR/
```

The DLL must sit **next to `SONICR.EXE`** — it is found by `LoadLibrary` at
runtime, not linked, so the deploy directory is the only location that counts.

## What the build does that matters

- **Links `SRLib::SR`**, not just the import libs. That is SR-lib's own `SR.cpp`
  wrapper, which owns the SR lifecycle — including the
  `SRContext::create` → `CreateGLWeaver` → `initialize()` ordering. Getting that
  order wrong is the single most-misdiagnosed LeiaSR bug: every call still
  returns success and `weave()` still runs, but the lenticular interleave uses
  default no-track eye coordinates, so the panel shows an image that never
  responds to head movement.
- **`srlib_apply_delayload()`** puts every SR DLL behind `/DELAYLOAD`. Without
  it, Windows resolves the imports at load time and a machine without the SR
  runtime gets a loader popup — usually naming `opencv_world343.dll`, which the
  SR import libs drag in through inlined `cv::String` destructors even though no
  OpenCV code is written anywhere.
- **Static CRT (`/MT`)**, so the DLL does not add a "please install the VC++
  Redistributable" failure mode of its own.
- **`/EHa`**, so `catch (...)` also catches SEH. The usual advice is to avoid
  this ABI-wide because it widens what `catch (...)` means for every function in
  the translation unit — but this TU is nothing *but* the SR shim, so there is no
  non-SR code in scope for it to affect.

Verify the result with:

```powershell
dumpbin /imports leiasr_shim.dll
```

Every SR DLL must appear under **delay load imports**, and the top-level import
section should contain only `OPENGL32.dll` and `KERNEL32.dll`.

## Verifying on hardware

1. **No SR runtime installed** — the game logs
   `LoadLibrary(leiasr_shim.dll) failed` or `srk_init returned 0`, and LeiaSR
   mode presents plain SbS. No popup, no crash.
2. **SR runtime but no SR display** — `srk_init` fails cleanly, same fallback.
3. **SR runtime + SR display** — glasses-free 3D, and the depth **responds to
   head movement**. A correct-looking but *static* image means the init ordering
   is wrong.
4. **Unplug the display mid-session** — `weave()` throws, the shim tears down,
   `srk_available()` goes to 0, and the game drops back to SbS without crashing.
5. **Switchable-lens panels** — switch away from LeiaSR, then quit. The lens must
   go *off* at each transition. The game drives this from whether it actually
   wove, not from which mode is selected, so a LeiaSR frame that fell back to SbS
   releases the lens too.

## Not shipping the SR runtime

The SR runtime DLLs (`DimencoWeaving.dll`, `SimulatedRealityCore.dll`,
`SimulatedRealityOpenGL.dll`, `opencv_world343.dll`, …) live in the system-wide
LeiaSR Platform install and are on `PATH`. They are shared between every
SR-using application on the machine and are deliberately **not** redistributed
here — shipping private copies invites version conflicts.
