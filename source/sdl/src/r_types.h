/**
 * r_types.h — Render vertex type for the immediate-mode render API.
 *
 * RenderVertex is the GPU-facing pre-transformed vertex, identical in
 * layout to the existing D3DTLVERTEX (8 ints / 32 bytes).
 */
#ifndef R_TYPES_H
#define R_TYPES_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    float    sx, sy;     /* screen-space position */
    float    sz;         /* normalized depth (0..1) */
    float    rhw;        /* 1/Z for perspective-correct interpolation */
    uint32_t color;      /* packed ARGB: (A<<24 | R<<16 | G<<8 | B) */
    uint32_t specular;   /* unused, reserved (always 0) */
    float    u, v;       /* texture coordinates */
} RenderVertex;

_Static_assert(sizeof(RenderVertex) == 32, "RenderVertex must be 32 bytes");
_Static_assert(offsetof(RenderVertex, sx)       == 0,  "");
_Static_assert(offsetof(RenderVertex, sy)       == 4,  "");
_Static_assert(offsetof(RenderVertex, sz)       == 8,  "");
_Static_assert(offsetof(RenderVertex, rhw)      == 12, "");
_Static_assert(offsetof(RenderVertex, color)    == 16, "");
_Static_assert(offsetof(RenderVertex, specular) == 20, "");
_Static_assert(offsetof(RenderVertex, u)        == 24, "");
_Static_assert(offsetof(RenderVertex, v)        == 28, "");

/* Which depth bucket a primitive belongs to, for stereo.
 *
 * This has to travel WITH the recorded draw call, not be read from a global at
 * replay time: the stereo path records a whole frame and replays it once per
 * eye at present time, by which point every R_Begin2D/R_BeginOverlay scope the
 * game opened has long since closed. Anything the classification depends on
 * must either live in the vertex data (as rhw does) or be captured here. */
typedef enum {
    R_LAYER_WORLD   = 0,  /* real geometry: depth from its own w */
    R_LAYER_HUD     = 1,  /* flat at the user's HUD depth */
    R_LAYER_OVERLAY = 2,  /* flat at zero disparity, uncompressed */
    R_LAYER_MASK    = 0x0F,

    /* Modifier, not a bucket: "this is authored in the 640x480 4:3 space, so
     * compress it horizontally in a wider viewport" — WITHOUT changing which
     * depth bucket it belongs to.
     *
     * The two concerns look linked because HUD content wants both, but they are
     * independent. The full-screen logo/title backdrops are the case that
     * separates them: they are drawn in fixed virtual screen coordinates and so
     * must be pillarboxed, yet they carry a genuine depth and are meant to be
     * seen at it. Folding them into a 2D bucket would fix the aspect by
     * flattening them, which is not the same thing. */
    R_LAYER_PILLARBOX = 0x10
} R_Layer;

#endif /* R_TYPES_H */
