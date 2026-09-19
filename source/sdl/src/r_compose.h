/**
 * r_compose.h — Stereo eye framebuffers and compose pass (GL backend side).
 *
 * Split from stereo.h so the settings module stays free of GL: stereo.c is
 * plain C with no renderer dependency, and this is the part only the GL
 * backend implements.
 */
#ifndef R_COMPOSE_H
#define R_COMPOSE_H

/* Bring up GLEW, the compose program and the eye framebuffers. Safe to call
 * repeatedly; no-ops once ready or once it has failed. Needs a current GL
 * context. */
void R_StereoInit(void);

/* Release GL objects. MUST run while the GL context is still alive — the SR
 * runtime holds GL resources keyed to it. */
void R_StereoShutdown(void);

/* Nonzero once the compose program and eye FBOs exist. */
int  R_StereoBackendReady(void);

/* Start-of-frame: (re)create targets if the window changed, refresh
 * g_s3dActive, and arm the draw recorder. */
void R_StereoBeginFrame(void);

/* End-of-frame: replay the recording per eye and compose to the backbuffer.
 * Returns 0 when stereo is not running, in which case the caller presents the
 * backbuffer as drawn. */
int  R_StereoComposeFrame(void);

#endif /* R_COMPOSE_H */
