/**
 * r_state_internal.h — Render-state snapshot shared between the GL backend
 * and the stereo capture/replay buffer.
 *
 * Not part of the public r_*.h API surface: game code sets state through
 * r_state.h's setters and never sees this struct. It lives in its own header
 * only because r_capture.c has to store one per recorded draw call.
 */
#ifndef R_STATE_INTERNAL_H
#define R_STATE_INTERNAL_H

#include "r_state.h"

typedef struct {
    int          textureId;    /* tpage index, or -1 for untextured */
    R_BlendMode  blendMode;
    int          depthTest;
    R_DepthFunc  depthFunc;
    int          depthWrite;
    R_TexEnvMode texEnv;
    R_FilterMode filter;
    R_CullMode   cullMode;
    int          alphaTest;
    float        alphaRef;
    int          scissorEnabled;
    int          scissorX, scissorY, scissorW, scissorH;
} R_StateSnapshot;

/* Implemented by the backend; used by the replay loop to re-establish the
 * state a captured draw was issued under. */
void R_StateCapture(R_StateSnapshot *dst);
void R_StateRestore(const R_StateSnapshot *src);

#endif /* R_STATE_INTERNAL_H */
