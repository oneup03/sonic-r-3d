/**
 * r_stereo_shaders.h — GLSL for the stereo compose pass.
 *
 * One vertex shader (a vertexless fullscreen triangle from gl_VertexID, no VBO
 * and no IA state) and one fragment shader carrying every output mode behind a
 * uniform branch. The branch is uniform-coherent across the whole draw, so it
 * costs nothing measurable and keeps the mode table in one place.
 *
 * Ported from perfect_dark_3D (port/fast3d/gfx_opengl_stereo_shaders.h), with
 * two deliberate changes:
 *
 *  - That implementation flips V in the vertex shader so vUV.y == 0 is the top
 *    of the screen. We do not flip: our eye FBOs are rendered with the same
 *    matrices as the backbuffer, so texture V == 0 is the BOTTOM, matching
 *    GL's gl_FragCoord origin. The top/bottom test in TAB is inverted to suit.
 *
 *  - Row/checkerboard parity is computed from the TOP-DOWN display row
 *    (uOutSize.y - 1 - gl_FragCoord.y) rather than gl_FragCoord.y directly.
 *    GL numbers rows from the bottom and physical panels from the top, and for
 *    an even framebuffer height those two parities are opposites — so reading
 *    gl_FragCoord.y raw silently inverts the eyes on every even-height
 *    display, which is every real one.
 *
 * Ghost / crosstalk reduction (ghostReduce) is applied LAST, after the mode's
 * own colour maths including the anaglyph matrix, so what gets compressed is
 * what actually reaches the display. It runs in LINEAR light (plain pow(2.2),
 * not the piecewise sRGB curve) because that is the space a cancelling display
 * performs its own correction in, and the two have to agree.
 */
#ifndef R_STEREO_SHADERS_H
#define R_STEREO_SHADERS_H

/* Mode values must match S3DMode in stereo.h (minus S3D_OFF, which never
 * reaches the compose pass). LEIASR composes as SBS into the weaver's input
 * texture, so it shares the SBS branch. */
#define STEREO_VS_SRC \
    "#version 130\n" \
    "out vec2 vUV;\n" \
    "void main(void) {\n" \
    "  vec2 p = vec2((gl_VertexID == 2) ? 3.0 : -1.0,\n" \
    "                (gl_VertexID == 1) ? 3.0 : -1.0);\n" \
    "  vUV = (p + 1.0) * 0.5;\n" \
    "  gl_Position = vec4(p, 0.0, 1.0);\n" \
    "}\n"

#define STEREO_FS_SRC \
    "#version 130\n" \
    "uniform sampler2D uTexL;\n" \
    "uniform sampler2D uTexR;\n" \
    "uniform int   uMode;\n" \
    "uniform vec2  uOutSize;\n" \
    "uniform float uGhostContrast;\n" \
    "uniform float uGhostLift;\n" \
    "in  vec2 vUV;\n" \
    "out vec4 fragColor;\n" \
    "\n" \
    "vec3 ghostReduce(vec3 c) {\n" \
    "  if (uGhostContrast == 1.0 && uGhostLift == 0.0) return c;\n" \
    "  vec3 lin = pow(clamp(c, 0.0, 1.0), vec3(2.2));\n" \
    "  lin = (lin - 0.5) * uGhostContrast + 0.5;\n" \
    "  lin = lin * (1.0 - uGhostLift) + uGhostLift;\n" \
    "  return pow(clamp(lin, 0.0, 1.0), vec3(1.0 / 2.2));\n" \
    "}\n" \
    "\n" \
    "/* Sampling both eye textures at the OUTPUT uv is what upscales the eye\n" \
    "   buffers to display resolution, in every mode, before any pattern is\n" \
    "   selected. Pattern-then-upscale would make the line pitch drift\n" \
    "   whenever render resolution != output resolution. */\n" \
    "vec4 eyeL(vec2 uv) { return texture(uTexL, uv); }\n" \
    "vec4 eyeR(vec2 uv) { return texture(uTexR, uv); }\n" \
    "\n" \
    "vec4 pick(int useRight, vec2 uv) {\n" \
    "  return (useRight != 0) ? eyeR(uv) : eyeL(uv);\n" \
    "}\n" \
    "\n" \
    "void main(void) {\n" \
    "  vec4 c;\n" \
    "  /* Top-down display row/column — see the header comment. */\n" \
    "  int row = int(uOutSize.y - 1.0 - gl_FragCoord.y);\n" \
    "  int col = int(gl_FragCoord.x);\n" \
    "\n" \
    "  if (uMode == 1 || uMode == 7) {          /* SBS, and LEIASR's input */\n" \
    "    if (vUV.x < 0.5) c = eyeL(vec2(vUV.x * 2.0, vUV.y));\n" \
    "    else             c = eyeR(vec2((vUV.x - 0.5) * 2.0, vUV.y));\n" \
    "  } else if (uMode == 2) {                 /* TAB: screen-top = left eye */\n" \
    "    if (vUV.y >= 0.5) c = eyeL(vec2(vUV.x, (vUV.y - 0.5) * 2.0));\n" \
    "    else              c = eyeR(vec2(vUV.x, vUV.y * 2.0));\n" \
    "  } else if (uMode == 3) {                 /* row interlaced */\n" \
    "    c = pick(row & 1, vUV);\n" \
    "  } else if (uMode == 4) {                 /* column interlaced */\n" \
    "    c = pick(col & 1, vUV);\n" \
    "  } else if (uMode == 5) {                 /* checkerboard */\n" \
    "    c = pick((row + col) & 1, vUV);\n" \
    "  } else if (uMode == 6) {                 /* anaglyph, Dubois red/cyan */\n" \
    "    vec3 L = eyeL(vUV).rgb;\n" \
    "    vec3 R = eyeR(vUV).rgb;\n" \
    "    float r = clamp( 0.437*L.r + 0.449*L.g + 0.164*L.b\n" \
    "                    -0.011*R.r - 0.032*R.g - 0.007*R.b, 0.0, 1.0);\n" \
    "    float g = clamp(-0.062*L.r - 0.062*L.g - 0.024*L.b\n" \
    "                    +0.377*R.r + 0.761*R.g + 0.009*R.b, 0.0, 1.0);\n" \
    "    float b = clamp(-0.048*L.r - 0.050*L.g - 0.017*L.b\n" \
    "                    -0.026*R.r - 0.093*R.g + 1.234*R.b, 0.0, 1.0);\n" \
    "    c = vec4(r, g, b, 1.0);\n" \
    "  } else {\n" \
    "    c = eyeL(vUV);\n" \
    "  }\n" \
    "\n" \
    "  fragColor = vec4(ghostReduce(c.rgb), 1.0);\n" \
    "}\n"

#endif /* R_STEREO_SHADERS_H */
