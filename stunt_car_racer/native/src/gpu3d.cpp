// gpu3d.cpp — voir gpu3d.hpp
#include "gpu3d.hpp"

#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace scr {

namespace {
// fonctions OpenGL au-delà de la version 1.1, chargées à l'exécution
#define GLF(ret, name, args) typedef ret(APIENTRY *name##_t) args; name##_t p_##name = nullptr;
GLF(GLuint, glCreateShader, (GLenum))
GLF(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))
GLF(void, glCompileShader, (GLuint))
GLF(void, glGetShaderiv, (GLuint, GLenum, GLint *))
GLF(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))
GLF(GLuint, glCreateProgram, ())
GLF(void, glAttachShader, (GLuint, GLuint))
GLF(void, glLinkProgram, (GLuint))
GLF(void, glGetProgramiv, (GLuint, GLenum, GLint *))
GLF(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))
GLF(void, glDeleteShader, (GLuint))
GLF(void, glUseProgram, (GLuint))
GLF(GLint, glGetUniformLocation, (GLuint, const GLchar *))
GLF(void, glUniform1i, (GLint, GLint))
GLF(void, glUniform1f, (GLint, GLfloat))
GLF(void, glUniform2f, (GLint, GLfloat, GLfloat))
GLF(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat))
GLF(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))
GLF(void, glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat *))
GLF(void, glGenVertexArrays, (GLsizei, GLuint *))
GLF(void, glBindVertexArray, (GLuint))
GLF(void, glGenBuffers, (GLsizei, GLuint *))
GLF(void, glBindBuffer, (GLenum, GLuint))
GLF(void, glBufferData, (GLenum, ptrdiff_t, const void *, GLenum))
GLF(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))
GLF(void, glEnableVertexAttribArray, (GLuint))
GLF(void, glGenFramebuffers, (GLsizei, GLuint *))
GLF(void, glBindFramebuffer, (GLenum, GLuint))
GLF(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
GLF(void, glGenRenderbuffers, (GLsizei, GLuint *))
GLF(void, glBindRenderbuffer, (GLenum, GLuint))
GLF(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))
GLF(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))
GLF(GLenum, glCheckFramebufferStatus, (GLenum))
GLF(void, glActiveTexture, (GLenum))
#undef GLF

bool loadGl() {
  bool ok = true;
#define L(name) ok = ok && (p_##name = reinterpret_cast<name##_t>(SDL_GL_GetProcAddress(#name))) != nullptr;
  L(glCreateShader) L(glShaderSource) L(glCompileShader) L(glGetShaderiv) L(glGetShaderInfoLog) L(glCreateProgram)
  L(glAttachShader) L(glLinkProgram) L(glGetProgramiv) L(glGetProgramInfoLog) L(glDeleteShader) L(glUseProgram)
  L(glGetUniformLocation) L(glUniform1i) L(glUniform1f) L(glUniform2f) L(glUniform3f) L(glUniform4f) L(glUniformMatrix3fv)
  L(glGenVertexArrays) L(glBindVertexArray) L(glGenBuffers) L(glBindBuffer) L(glBufferData) L(glVertexAttribPointer)
  L(glEnableVertexAttribArray) L(glGenFramebuffers) L(glBindFramebuffer) L(glFramebufferTexture2D) L(glGenRenderbuffers)
  L(glBindRenderbuffer) L(glRenderbufferStorage) L(glFramebufferRenderbuffer) L(glCheckFramebufferStatus) L(glActiveTexture)
#undef L
  return ok;
}

constexpr GLenum K_ARRAY = 0x8892, K_ELEMENT = 0x8893, K_STREAM = 0x88E0, K_FRAG = 0x8B30, K_VERT = 0x8B31,
                 K_COMPILE = 0x8B81, K_LINK = 0x8B82, K_FB = 0x8D40, K_RB = 0x8D41, K_COLOR0 = 0x8CE0, K_DEPTH_ATT = 0x8D00,
                 K_DEPTH24 = 0x81A6, K_FB_OK = 0x8CD5, K_TEX0 = 0x84C0, K_R32F = 0x822E, K_RED = 0x1903, K_RGBA8 = 0x8058,
                 K_BGRA = 0x80E1, K_REV = 0x8367, K_LOD_MAX = 0x813B, K_LOD_BASE = 0x813C, K_LEVEL_MAX = 0x813D;

// ------------------------------------------------------------------ programmes
const char *VS_POLY = R"(#version 330 core
layout(location = 0) in vec3 aCam;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec3 aMisc;     // profondeur fixée (-1 : non), alpha, texture (-1 : aucune)
layout(location = 3) in vec3 aO;
layout(location = 4) in vec3 aU;
layout(location = 5) in vec3 aV;
layout(location = 6) in vec4 aSW;       // s0 s1 w0 w1
layout(location = 7) in vec3 aR;        // rayon, flou, netteté
layout(location = 8) in vec3 aN;        // normale du plan (monde)
uniform vec4 uProj;   // focale x, focale y, centre x, centre y (pixels)
uniform vec2 uSize;
uniform mat3 uMt;     // repère caméra -> monde (rotation)
out vec3 vRel;        // point du monde relatif à la caméra
flat out vec4 vColor; flat out vec3 vMisc, vO, vU, vV, vR, vN; flat out vec4 vSW;
void main() {
  float z = aCam.z;
  vec2 s = vec2(uProj.z + uProj.x * aCam.x / z, uProj.w - uProj.y * aCam.y / z);
  gl_Position = vec4((s / uSize * 2.0 - 1.0) * z, 0.0, z);
  vRel = uMt * aCam;
  vColor = aColor; vMisc = aMisc; vO = aO; vU = aU; vV = aV; vSW = aSW; vR = aR; vN = aN;
}
)";

const char *FS_POLY = R"(#version 330 core
in vec3 vRel;
flat in vec4 vColor; flat in vec3 vMisc, vO, vU, vV, vR, vN; flat in vec4 vSW;
uniform vec3 uCamPos;
uniform float uFocal;
uniform float uTrans;      // 1 : passe des surfaces semi-transparentes
uniform sampler2D uTex[4]; // sol (plaques), sol (détail), route, murs
out vec4 oColor;

uint hash2(int x, int y, uint seed) {
  uint h = uint(x) * 374761393u + uint(y) * 668265263u + seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
float vnoise(float x, float y, uint seed) {
  float fx = floor(x), fy = floor(y);
  int ix = int(fx), iy = int(fy);
  float tx = x - fx, ty = y - fy;
  tx = tx * tx * (3.0 - 2.0 * tx); ty = ty * ty * (3.0 - 2.0 * ty);
  float v00 = float(hash2(ix, iy, seed) & 0xffffu) / 32767.5 - 1.0, v10 = float(hash2(ix + 1, iy, seed) & 0xffffu) / 32767.5 - 1.0;
  float v01 = float(hash2(ix, iy + 1, seed) & 0xffffu) / 32767.5 - 1.0, v11 = float(hash2(ix + 1, iy + 1, seed) & 0xffffu) / 32767.5 - 1.0;
  float a = v00 + (v10 - v00) * tx, b = v01 + (v11 - v01) * tx;
  return a + (b - a) * ty;
}
float oct(float x, float y, float scale, float amp, float foot, uint seed) {
  float k = clamp(1.5 - foot / scale * 2.0, 0.0, 1.0);
  return k > 0.0 ? vnoise(x / scale, y / scale, seed) * amp * k : 0.0;
}
float mip(int t, float u, float v, float foot) {
  float l = foot > 1.0 ? ceil(log2(foot)) : 0.0;
  l = min(l, 8.0);
  vec2 uv = vec2(u, v) / 256.0;
  if (t == 0) return textureLod(uTex[0], uv, l).r;
  if (t == 1) return textureLod(uTex[1], uv, l).r;
  if (t == 2) return textureLod(uTex[2], uv, l).r;
  return textureLod(uTex[3], uv, l).r;
}
vec3 rgb(uint c) { return vec3(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u)) / 255.0; }
float blurSin(float a, float n, float d) { float nd = n * d; return abs(nd) < 1e-4 ? sin(n * a) : (cos(n * (a - d)) - cos(n * a)) / nd; }
float blurCos(float a, float n, float d) { float nd = n * d; return abs(nd) < 1e-4 ? cos(n * a) : (sin(n * a) - sin(n * (a - d))) / nd; }

vec3 shade(int kind, vec3 color, vec3 p, float foot) {
  float k = 1.0;
  vec3 d = p - vO;
  float s0 = vSW.x, s1 = vSW.y, w0 = vSW.z, w1 = vSW.w;
  if (kind == 0) {          // sol
    k += mip(0, p.x / 16.0, p.z / 16.0, foot / 16.0) + mip(1, p.x * 4.0, p.z * 4.0, foot * 4.0);
  } else if (kind == 1) {   // route
    k += mip(2, p.x * 4.0, p.z * 4.0, foot * 4.0) + mip(0, p.z / 8.0, p.x / 8.0, foot / 8.0) * 0.5;
    float s = dot(d, vU);
    float ss = (s - s0) / max(1e-6, s1 - s0);
    float wtr = max(0.035, foot / max(1e-6, s1 - s0));
    float tr = exp(-pow((ss - 0.3) / (wtr + 0.05), 2.0)) + exp(-pow((ss - 0.7) / (wtr + 0.05), 2.0));
    k -= 0.09 * tr * min(1.0, 0.06 / (wtr + 0.025));
  } else if (kind == 2) {   // murs
    float sy = p.y / 24.0, band = abs(sy - floor(sy) - 0.5);
    float kb = clamp(1.5 - foot / 24.0 * 3.0, 0.0, 1.0);
    k += (band > 0.45 ? -0.12 : 0.0) * kb + mip(3, (p.x + p.z) * 2.0, p.y * 2.0, foot * 2.0);
  } else if (kind == 3) {   // carrosserie
    float s = dot(d, vU), w = dot(d, vV);
    float ws = clamp((w - w0) / max(1e-6, w1 - w0), 0.0, 1.0), ss = clamp((s - s0) / max(1e-6, s1 - s0), 0.0, 1.0);
    k = 0.86 + 0.24 * ws + 0.18 * exp(-pow((ws - 0.72) * 9.0, 2.0));
    float e = min(min(s - s0, s1 - s), min(w - w0, w1 - w));
    float seam = max(1.2, foot * 1.5);
    if (e < seam) k *= 0.62;
    else if (abs(ss - 0.5) * (s1 - s0) < seam * 0.6 && s1 - s0 > 30.0) k *= 0.8;
    k += oct(s, w, 3.0, 0.025, foot, 31u);
  } else if (kind == 4) {   // bande de roulement
    float a = atan(dot(d, vV), dot(d, vU));
    float g = blurSin(a, 14.0, vR.y);
    float kb = clamp(1.5 - foot / (vR.x * 0.35) * 2.0, 0.0, 1.0) * vR.z;
    return clamp(rgb(0x262626u) * (1.0 + 0.55 * clamp(0.5 + 1.2 * g, 0.0, 1.0) * kb), 0.0, 1.0);
  } else if (kind == 6) {   // public des tribunes
    float s = dot(d, vU), w = p.y - vO.y;
    float far = clamp((foot - 3.0) / 8.0, 0.0, 1.0);
    vec3 seat = rgb(0x5c5c60u), avg = rgb(0x8a7c74u);
    if (far >= 1.0) return avg;
    float cs = s / 26.0;
    int cell = int(floor(cs));
    float fx = cs - float(cell);
    uint hh = hash2(cell, int(vR.x), 77u);
    if (hh % 9u == 0u) return far > 0.0 ? rgb(0x7a7270u) : seat;
    uint SH[8] = uint[8](0xd02828u, 0x2850c8u, 0xe8c020u, 0xeeeeeeu, 0x30a040u, 0xe87818u, 0x8040a0u, 0x30b0c0u);
    uint SK[3] = uint[3](0xf0c8a0u, 0xc89870u, 0x8a5a3au);
    float bob = float((hh >> 8) % 7u) * 0.8;
    vec3 c = seat;
    if (w < 36.0 + bob && abs(fx - 0.5) < 0.36) c = rgb(SH[(hh >> 4) % 8u]);
    else if (w > 39.0 + bob && w < 53.0 + bob && abs(fx - 0.5) < 0.19) c = rgb(SK[(hh >> 12) % 3u]);
    return mix(c, avg, far);
  } else if (kind == 7) {   // panneau publicitaire
    float s = dot(d, vU), w = dot(d, vV);
    float L = s1, H = w1, b = H * 0.07;
    if (s < b || s > L - b || w < b || w > H - b) return rgb(0xf2f2f2u);
    float ty0 = H * 0.27, ty1 = H * 0.73, lh = ty1 - ty0, lw = lh * 0.62, gap = lh * 0.18;
    int nl = int((L * 0.8) / (lw + gap));
    float sx0 = (L - float(nl) * (lw + gap) + gap) / 2.0;
    if (w < ty0 || w > ty1 || s < sx0 || foot > lh * 0.35) return foot > lh * 0.35 && w > ty0 && w < ty1 ? clamp(color * 1.25, 0.0, 1.0) : color;
    float ls = (s - sx0) / (lw + gap);
    int li = int(ls);
    float lx = (ls - float(li)) * (lw + gap);
    if (li >= nl || lx > lw) return color;
    int px = clamp(int(lx / lw * 3.0), 0, 2), py = clamp(int((ty1 - w) / lh * 5.0), 0, 4);
    uint glyph = hash2(li, int(vR.x), 5u) & 0x7fffu;
    bool on = ((glyph >> uint(py * 3 + px)) & 1u) != 0u || px == 0;
    return on ? rgb(0xfafafau) : color;
  } else if (kind == 5) {   // flanc du pneu, jante, moyeu
    float s = dot(d, vU), w = dot(d, vV);
    float r = sqrt(s * s + w * w) / vR.x, a = atan(w, s);
    float sharp = vR.z * clamp(1.4 - abs(vR.y) * 3.0, 0.0, 1.0);
    if (r > 0.97) return rgb(0x181818u);
    if (r > 0.66) {
      float base = 1.0 + 0.25 * (r - 0.66) / 0.31;
      if (r > 0.74 && r < 0.9) {
        float am = mod(a + 2.0 * 3.14159265, 3.14159265);
        if (am > 0.35 && am < 1.25) {
          float letters = sin(am * 45.0) > -0.2 && abs(r - 0.82) < 0.06 ? 1.0 : 0.0;
          return clamp(rgb(0x2a2a2au) * (base + 3.5 * letters * sharp + 0.6 * letters * (1.0 - sharp) * 0.3), 0.0, 1.0);
        }
      }
      return clamp(rgb(0x2a2a2au) * base, 0.0, 1.0);
    }
    if (r > 0.6) return rgb(0x505050u);
    if (r > 0.24 && r < 0.56) {
      float hole = clamp((-blurCos(a, 5.0, vR.y) - 0.15) * 3.0, 0.0, 1.0);
      float avg = clamp(0.5 - 0.15 * 3.0, 0.0, 1.0);
      hole = avg + (hole - avg) * vR.z;
      return clamp(rgb(0xc0c0c0u) * (1.0 - 0.75 * hole), 0.0, 1.0);
    }
    if (r < 0.2 && r > 0.11) {
      float nut = cos(5.0 * (a - 3.14159265 / 5.0));
      if (nut > 0.8 && sharp > 0.2) return clamp(rgb(0x404040u) * (1.0 + (1.0 - sharp)), 0.0, 1.0);
    }
    return r < 0.24 ? rgb(0xd8d8d8u) : rgb(0xb0b0b0u);
  } else return color;
  return clamp(color * k, 0.0, 1.0);
}

void main() {
  float invz = vMisc.x >= 0.0 ? vMisc.x : gl_FragCoord.w;
  float depth = clamp(-log2(max(invz, 1e-30)) / 48.0, 0.0, 1.0);
  if (uTrans > 0.5) depth -= 1.6e-5;   // ombres et fumée : tolérance de profondeur du rendu logiciel
  gl_FragDepth = depth;
  vec3 c = vColor.rgb;
  int kind = int(vMisc.z + 0.5) - 1;
  if (kind >= 0) {
    float dist = max(length(vRel), 1e-3);
    float cosI = abs(dot(vN, vRel)) / dist;
    float foot = dist / uFocal / max(0.08, cosI);
    c = shade(kind, c, uCamPos + vRel, foot);
  }
  oColor = vec4(c, vMisc.y);
}
)";

const char *VS_FULL = R"(#version 330 core
out vec2 vUv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  vUv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char *FS_SKY = R"(#version 330 core
uniform vec4 uProj;
uniform mat3 uMt;
uniform vec3 uCamPos;
uniform vec3 uBase;     // couleur du ciel de la palette (0..255)
uniform float uT;       // temps (s)
uniform sampler2D uCloud;
out vec4 oColor;
float mipc(float u, float v, float foot) {
  float l = foot > 1.0 ? min(ceil(log2(foot)), 8.0) : 0.0;
  return textureLod(uCloud, vec2(u, v) / 256.0, l).r;
}
void main() {
  vec3 hz = uBase + (vec3(232.0, 240.0, 255.0) - uBase) * 0.62;
  vec3 zn = uBase * vec3(0.62, 0.72, 0.92);
  vec3 sun = normalize(vec3(0.62, 0.14, 0.72));
  float dxc = gl_FragCoord.x - uProj.z, dy = -(gl_FragCoord.y - uProj.w) / uProj.y;
  vec3 ray = uMt * vec3(dxc / uProj.x, dy, 1.0);
  float il = 1.0 / length(ray);
  float e = ray.y * il;
  float t = sqrt(clamp(e, 0.0, 1.0));
  vec3 c = hz + (zn - hz) * t;
  float sd = dot(ray, sun) * il;
  if (sd > 0.9) {
    float h = (sd - 0.9) * 10.0;
    float glow = min(1.0, h * h * h * h * 0.35 + (sd > 0.9994 ? 1.0 : sd > 0.999 ? (sd - 0.999) * 2500.0 : 0.0));
    c += (vec3(255.0, 246.0, 222.0) - c) * glow;
  }
  if (e > 0.015) {
    float tt = 9000.0 / ray.y;
    float px = uCamPos.x + ray.x * tt, pz = uCamPos.z + ray.z * tt;
    float foot = 8.0 * tt * il / uProj.x / 260.0 / max(0.05, e);
    float nv = mipc(px / 260.0 + uT * 2.2, pz / 260.0 + uT * 0.9, foot);
    float k = clamp((nv + 0.02) / 0.22, 0.0, 1.0);
    k = k * k * (3.0 - 2.0 * k) * clamp((e - 0.015) / 0.12, 0.0, 1.0);
    if (k > 0.0) {
      float dense = clamp(nv * 2.2, 0.0, 1.0), lit = max(0.0, sd);
      vec3 cl = vec3(248.0 - 52.0 * dense + 7.0 * lit, 249.0 - 48.0 * dense + 4.0 * lit, 252.0 - 38.0 * dense);
      c += (cl - c) * k * 0.92;
    }
  }
  oColor = vec4(clamp(c, 0.0, 255.0) / 255.0, 1.0);
}
)";

// lissage des bords : même algorithme que le rendu logiciel (FXAA)
const char *FS_FXAA = R"(#version 330 core
uniform sampler2D uImg;
uniform vec2 uSize;
out vec4 oColor;
float luma(vec3 c) { return floor(floor(c.r * 255.0 + 0.5) * 77.0 + floor(c.g * 255.0 + 0.5) * 150.0 + floor(c.b * 255.0 + 0.5) * 29.0) / 256.0; }
vec3 at(vec2 p) { return texelFetch(uImg, ivec2(clamp(p, vec2(0.0), uSize - 1.0)), 0).rgb; }
vec3 fetch(vec2 p) {   // lecture bilinéaire, coordonnées en pixels (centre = entier)
  p = clamp(p, vec2(0.0), uSize - 1.0);
  return texture(uImg, (p + 0.5) / uSize).rgb;
}
void main() {
  vec2 x = floor(gl_FragCoord.xy);
  vec3 cM = at(x);
  if (x.x < 1.0 || x.y < 1.0 || x.x > uSize.x - 2.0 || x.y > uSize.y - 2.0) { oColor = vec4(cM, 1.0); return; }
  vec3 cN = at(x + vec2(0, -1)), cS = at(x + vec2(0, 1)), cW = at(x + vec2(-1, 0)), cE = at(x + vec2(1, 0));
  if (cM == cN && cM == cS && cM == cW && cM == cE) { oColor = vec4(cM, 1.0); return; }
  float iM = floor(luma(cM)), iN = floor(luma(cN)), iS = floor(luma(cS)), iW = floor(luma(cW)), iE = floor(luma(cE));
  float imin = min(iM, min(min(iN, iS), min(iW, iE))), imax = max(iM, max(max(iN, iS), max(iW, iE)));
  if (imax - imin < max(16.0, floor(imax * 42.0 / 256.0))) { oColor = vec4(cM, 1.0); return; }
  float lmin = imin / 255.0, lmax = imax / 255.0;
  float lNW = floor(luma(at(x + vec2(-1, -1)))) / 255.0, lNE = floor(luma(at(x + vec2(1, -1)))) / 255.0;
  float lSW = floor(luma(at(x + vec2(-1, 1)))) / 255.0, lSE = floor(luma(at(x + vec2(1, 1)))) / 255.0;
  float dx = -((lNW + lNE) - (lSW + lSE)), dy = (lNW + lSW) - (lNE + lSE);
  float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);
  float rcp = 1.0 / (min(abs(dx), abs(dy)) + reduce);
  vec2 dir = clamp(vec2(dx, dy) * rcp, -8.0, 8.0);
  vec3 A = 0.5 * (fetch(x + dir * (1.0 / 3.0 - 0.5)) + fetch(x + dir * (2.0 / 3.0 - 0.5)));
  vec3 B = 0.5 * A + 0.25 * (fetch(x - dir * 0.5) + fetch(x + dir * 0.5));
  float lB = dot(B * 255.0, vec3(77.0, 150.0, 29.0)) / 65280.0;
  oColor = vec4((lB < lmin || lB > lmax) ? A : B, 1.0);
}
)";

GLuint compile(GLenum type, const char *src, std::string &err) {
  GLuint s = p_glCreateShader(type);
  p_glShaderSource(s, 1, &src, nullptr);
  p_glCompileShader(s);
  GLint ok = 0;
  p_glGetShaderiv(s, K_COMPILE, &ok);
  if (!ok) {
    char log[2048] = {}; p_glGetShaderInfoLog(s, sizeof log, nullptr, log);
    err = std::string("compilation d'un shader : ") + log;
    return 0;
  }
  return s;
}

GLuint link(const char *vs, const char *fs, std::string &err) {
  GLuint v = compile(K_VERT, vs, err);
  if (!v) return 0;
  GLuint f = compile(K_FRAG, fs, err);
  if (!f) return 0;
  GLuint p = p_glCreateProgram();
  p_glAttachShader(p, v); p_glAttachShader(p, f);
  p_glLinkProgram(p);
  p_glDeleteShader(v); p_glDeleteShader(f);
  GLint ok = 0;
  p_glGetProgramiv(p, K_LINK, &ok);
  if (!ok) {
    char log[2048] = {}; p_glGetProgramInfoLog(p, sizeof log, nullptr, log);
    err = std::string("édition de liens d'un programme : ") + log;
    return 0;
  }
  return p;
}
}  // namespace

Gpu3D::~Gpu3D() {
  if (ctx_) SDL_GL_DeleteContext(ctx_);
  if (win_) SDL_DestroyWindow(win_);
}

bool Gpu3D::init(std::string &err) {
  SDL_Window *prevWin = SDL_GL_GetCurrentWindow();
  SDL_GLContext prevCtx = SDL_GL_GetCurrentContext();
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  win_ = SDL_CreateWindow("scr-gl", 0, 0, 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  if (!win_) { err = std::string("fenêtre OpenGL : ") + SDL_GetError(); return false; }
  ctx_ = SDL_GL_CreateContext(win_);
  // le moteur d'affichage SDL garde ses propres réglages : on les rend au contexte précédent
  SDL_GL_ResetAttributes();
  if (!ctx_) { err = std::string("contexte OpenGL 3.3 : ") + SDL_GetError(); return false; }
  bool ok = loadGl() && build(err);
  if (ok) {
    const char *r = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
    name_ = r ? r : "OpenGL";
  } else if (err.empty()) err = "fonctions OpenGL 3.3 absentes";
  if (prevCtx) SDL_GL_MakeCurrent(prevWin, prevCtx);
  ok_ = ok;
  return ok;
}

bool Gpu3D::build(std::string &err) {
  progPoly_ = link(VS_POLY, FS_POLY, err);
  if (!progPoly_) return false;
  progSky_ = link(VS_FULL, FS_SKY, err);
  if (!progSky_) return false;
  progFxaa_ = link(VS_FULL, FS_FXAA, err);
  if (!progFxaa_) return false;
  p_glGenVertexArrays(1, &vaoFull_);   // passes plein écran : aucun sommet lu
  p_glGenVertexArrays(1, &vao_);
  p_glBindVertexArray(vao_);
  p_glGenBuffers(1, &vbo_); p_glGenBuffers(1, &ibo_);
  p_glBindBuffer(K_ARRAY, vbo_);
  p_glBindBuffer(K_ELEMENT, ibo_);
  const int S = 29 * 4;
  const int sizes[9] = {3, 4, 3, 3, 3, 3, 4, 3, 3};
  int off = 0;
  for (int a = 0; a < 9; a++) {
    p_glVertexAttribPointer(GLuint(a), sizes[a], GL_FLOAT, GL_FALSE, S, reinterpret_cast<const void *>(intptr_t(off * 4)));
    p_glEnableVertexAttribArray(GLuint(a));
    off += sizes[a];
  }
  // textures de bruit, avec leurs niveaux de mip-map
  glGenTextures(5, tex_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  for (int t = 0; t < 5; t++) {
    glBindTexture(GL_TEXTURE_2D, tex_[t]);
    const auto &lv = Renderer3D::mipLevels(t);
    for (size_t l = 0; l < lv.size(); l++) {
      const int n = 256 >> l;
      glTexImage2D(GL_TEXTURE_2D, GLint(l), K_R32F, n, n, 0, K_RED, GL_FLOAT, lv[l].data());
    }
    glTexParameteri(GL_TEXTURE_2D, K_LOD_BASE, 0);
    glTexParameteri(GL_TEXTURE_2D, K_LEVEL_MAX, GLint(lv.size()) - 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  }
  p_glUseProgram(progPoly_);
  for (int t = 0; t < 4; t++) {
    char nm[16]; std::snprintf(nm, sizeof nm, "uTex[%d]", t);
    p_glUniform1i(p_glGetUniformLocation(progPoly_, nm), t);
  }
  p_glUseProgram(progSky_);
  p_glUniform1i(p_glGetUniformLocation(progSky_, "uCloud"), 4);
  p_glUseProgram(progFxaa_);
  p_glUniform1i(p_glGetUniformLocation(progFxaa_, "uImg"), 5);
  p_glGenFramebuffers(2, fbo_);
  glGenTextures(2, color_);
  p_glGenRenderbuffers(1, &depth_);
  return glGetError() == GL_NO_ERROR || (err = "erreur OpenGL à la préparation", false);
}

void Gpu3D::resize(int W, int H) {
  if (W == W_ && H == H_) return;
  W_ = W; H_ = H;
  for (int k = 0; k < 2; k++) {
    glBindTexture(GL_TEXTURE_2D, color_[k]);
    glTexImage2D(GL_TEXTURE_2D, 0, K_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);   // GL_CLAMP_TO_EDGE
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    p_glBindFramebuffer(K_FB, fbo_[k]);
    p_glFramebufferTexture2D(K_FB, K_COLOR0, GL_TEXTURE_2D, color_[k], 0);
  }
  p_glBindRenderbuffer(K_RB, depth_);
  p_glRenderbufferStorage(K_RB, K_DEPTH24, W, H);
  p_glBindFramebuffer(K_FB, fbo_[0]);
  p_glFramebufferRenderbuffer(K_FB, K_DEPTH_ATT, K_RB, depth_);
}

bool Gpu3D::render(const Renderer3D &r, uint32_t *out) {
  if (!ok_ || failed_ || r.W_ <= 0 || r.H_ <= 0) return false;
  SDL_Window *prevWin = SDL_GL_GetCurrentWindow();
  SDL_GLContext prevCtx = SDL_GL_GetCurrentContext();
  if (SDL_GL_MakeCurrent(win_, ctx_) != 0) { failed_ = true; return false; }
  resize(r.W_, r.H_);
  p_glBindFramebuffer(K_FB, fbo_[0]);
  if (p_glCheckFramebufferStatus(K_FB) != K_FB_OK) { failed_ = true; if (prevCtx) SDL_GL_MakeCurrent(prevWin, prevCtx); return false; }
  glViewport(0, 0, r.W_, r.H_);
  const Camera &c = r.cam_;
  float mt[9];
  for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) mt[i * 3 + j] = float(r.M_[i][j]);   // M en lignes = M^T en colonnes
  auto chan = [](uint32_t v, int sh) { return float((v >> sh) & 255) / 255.f; };
  glClearColor(chan(r.bg_, 16), chan(r.bg_, 8), chan(r.bg_, 0), 1.f);
  glClearDepth(1.0);
  glDepthMask(GL_TRUE);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (r.sky_) {   // ciel derrière tout
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    p_glUseProgram(progSky_);
    p_glUniform4f(p_glGetUniformLocation(progSky_, "uProj"), float(c.focal), float(c.focalY), float(c.cx), float(c.cy));
    p_glUniformMatrix3fv(p_glGetUniformLocation(progSky_, "uMt"), 1, GL_FALSE, mt);
    p_glUniform3f(p_glGetUniformLocation(progSky_, "uCamPos"), float(c.pos.x), float(c.pos.y), float(c.pos.z));
    p_glUniform3f(p_glGetUniformLocation(progSky_, "uBase"), float((r.skyBase_ >> 16) & 255), float((r.skyBase_ >> 8) & 255), float(r.skyBase_ & 255));
    p_glUniform1f(p_glGetUniformLocation(progSky_, "uT"), float(r.skyT_));
    p_glActiveTexture(K_TEX0 + 4); glBindTexture(GL_TEXTURE_2D, tex_[4]);
    p_glBindVertexArray(vaoFull_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  // polygones : sommets (repère caméra, découpés au plan proche) et réglages de texture
  auto fill = [&](const std::vector<Renderer3D::SPoly> &L) {
    vtx_.clear(); idx_.clear(); runs_.clear();
    for (const auto &p : L) {
      const bool fixed = p.fixedDepth >= 0;
      if (runs_.empty() || runs_.back().fixed != fixed) runs_.push_back({idx_.size(), 0, fixed});
      const unsigned base = unsigned(vtx_.size() / 29);
      const Renderer3D::TexPlane *tp = p.tex >= 0 ? &r.texs_[size_t(p.tex)] : nullptr;
      for (int i = 0; i < p.cn; i++) {
        const Vec3 &v = r.cv_[size_t(p.cfirst + i)];
        float row[29] = {float(v.x), float(v.y), float(v.z), chan(p.color, 16), chan(p.color, 8), chan(p.color, 0), 1.f,
                         p.fixedDepth, p.alpha, float(tp ? tp->t.kind + 1 : 0)};
        if (tp) {
          const TexInfo &t = tp->t;
          const float o9[9] = {float(t.o.x), float(t.o.y), float(t.o.z), float(t.u.x), float(t.u.y), float(t.u.z), float(t.v.x), float(t.v.y), float(t.v.z)};
          std::memcpy(row + 10, o9, sizeof o9);
          row[19] = float(t.s0); row[20] = float(t.s1); row[21] = float(t.w0); row[22] = float(t.w1);
          row[23] = float(t.r); row[24] = float(t.blur); row[25] = float(t.contrast);
          row[26] = float(tp->n.x); row[27] = float(tp->n.y); row[28] = float(tp->n.z);
        }
        vtx_.insert(vtx_.end(), row, row + 29);
      }
      for (int i = 1; i + 1 < p.cn; i++) { idx_.push_back(base); idx_.push_back(base + unsigned(i)); idx_.push_back(base + unsigned(i) + 1); }
      runs_.back().count = idx_.size() - runs_.back().first;
    }
    p_glBindVertexArray(vao_);
    p_glBindBuffer(K_ARRAY, vbo_);
    p_glBufferData(K_ARRAY, ptrdiff_t(vtx_.size() * 4), vtx_.data(), K_STREAM);
    p_glBindBuffer(K_ELEMENT, ibo_);
    p_glBufferData(K_ELEMENT, ptrdiff_t(idx_.size() * 4), idx_.data(), K_STREAM);
  };
  p_glUseProgram(progPoly_);
  p_glUniform4f(p_glGetUniformLocation(progPoly_, "uProj"), float(c.focal), float(c.focalY), float(c.cx), float(c.cy));
  p_glUniform2f(p_glGetUniformLocation(progPoly_, "uSize"), float(r.W_), float(r.H_));
  p_glUniformMatrix3fv(p_glGetUniformLocation(progPoly_, "uMt"), 1, GL_FALSE, mt);
  p_glUniform3f(p_glGetUniformLocation(progPoly_, "uCamPos"), float(c.pos.x), float(c.pos.y), float(c.pos.z));
  p_glUniform1f(p_glGetUniformLocation(progPoly_, "uFocal"), float(c.focal));
  for (int t = 0; t < 4; t++) { p_glActiveTexture(K_TEX0 + GLenum(t)); glBindTexture(GL_TEXTURE_2D, tex_[t]); }
  const GLint uTrans = p_glGetUniformLocation(progPoly_, "uTrans");
  glEnable(GL_DEPTH_TEST);
  // surfaces opaques : même ordre que le rendu logiciel ; à profondeur égale, le premier dessiné reste
  // (sauf décor lointain à profondeur fixée, où le dernier l'emporte : ordre des collines)
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  p_glUniform1f(uTrans, 0.f);
  fill(r.polys_);
  for (const Run &run : runs_) {
    glDepthFunc(run.fixed ? GL_LEQUAL : GL_LESS);
    glDrawElements(GL_TRIANGLES, GLsizei(run.count), GL_UNSIGNED_INT, reinterpret_cast<const void *>(run.first * 4));
  }
  // ombres et fumée : testées contre la profondeur sans l'écrire, mélangées dans l'ordre d'envoi
  if (!r.trans_.empty()) {
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    p_glUniform1f(uTrans, 1.f);
    glDepthFunc(GL_LEQUAL);
    fill(r.trans_);
    if (!idx_.empty()) glDrawElements(GL_TRIANGLES, GLsizei(idx_.size()), GL_UNSIGNED_INT, nullptr);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
  }
  glDisable(GL_DEPTH_TEST);
  int src = 0;
  if (r.aa_ && r.W_ >= 3 && r.H_ >= 3) {   // lissage dans la seconde image
    p_glBindFramebuffer(K_FB, fbo_[1]);
    p_glUseProgram(progFxaa_);
    p_glUniform2f(p_glGetUniformLocation(progFxaa_, "uSize"), float(r.W_), float(r.H_));
    p_glActiveTexture(K_TEX0 + 5); glBindTexture(GL_TEXTURE_2D, color_[0]);
    p_glBindVertexArray(vaoFull_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    src = 1;
  }
  // relecture : ligne 0 de l'image OpenGL = haut de l'écran (projection retournée), pixels ARGB
  p_glBindFramebuffer(K_FB, fbo_[src]);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, r.W_, r.H_, K_BGRA, K_REV, out);
  const bool good = glGetError() == GL_NO_ERROR;
  p_glBindFramebuffer(K_FB, 0);
  if (prevCtx) SDL_GL_MakeCurrent(prevWin, prevCtx);
  if (!good) { failed_ = true; return false; }
  return true;
}

}  // namespace scr
