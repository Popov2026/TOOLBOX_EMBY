// render3d.cpp — voir render3d.hpp
#include "render3d.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

namespace scr {

Renderer3D::Renderer3D() {
  unsigned n = std::thread::hardware_concurrency();
  threads_ = int(std::clamp(n ? n : 1u, 1u, 16u));
}

void Renderer3D::begin(int W, int H, const Camera &cam, uint32_t background) {
  W_ = W; H_ = H; cam_ = cam; bg_ = background;
  cyw_ = std::cos(cam.yaw); syw_ = std::sin(cam.yaw);
  cp_ = std::cos(cam.pitch); sp_ = std::sin(cam.pitch);
  cr_ = std::cos(cam.roll); sr_ = std::sin(cam.roll);
  polys_.clear(); sx_.clear(); sy_.clear(); texs_.clear();
  // matrice de rotation monde -> caméra (colonnes = images des axes du monde)
  for (int c = 0; c < 3; c++) {
    Vec3 e{cam.pos.x + (c == 0), cam.pos.y + (c == 1), cam.pos.z + (c == 2)};
    Vec3 r = toCam(e);
    M_[0][c] = r.x; M_[1][c] = r.y; M_[2][c] = r.z;
  }
  if ((int)zbuf_.size() != W * H) { zbuf_.assign(size_t(W) * H, 0.f); idbuf_.assign(size_t(W) * H, -1); }
}

Vec3 Renderer3D::toCam(const Vec3 &p) const {
  double x = p.x - cam_.pos.x, y = p.y - cam_.pos.y, z = p.z - cam_.pos.z;
  double x1 = x * cyw_ - z * syw_, z1 = x * syw_ + z * cyw_;           // lacet
  double y2 = y * cp_ - z1 * sp_, z2 = y * sp_ + z1 * cp_;             // tangage
  double x3 = x1 * cr_ - y2 * sr_, y3 = y2 * cr_ + x1 * sr_;           // roulis
  return {x3, y3, z2};
}

void Renderer3D::poly(const Vec3 *pts, int n, uint32_t color) {
  Vec3 cam[32];
  for (int i = 0; i < n; i++) cam[i] = toCam(pts[i]);
  addClipped(cam, n, color, -1);
}

void Renderer3D::polyFar(const Vec3 *pts, int n, uint32_t color, float depth) {
  Vec3 cam[32];
  for (int i = 0; i < n; i++) cam[i] = toCam(pts[i]);
  addClipped(cam, n, color, depth);
}

int Renderer3D::addTex(const TexInfo &t, const Vec3 &n, double d) {
  texs_.push_back({t, n, d});
  return int(texs_.size()) - 1;
}

void Renderer3D::polyTex(const Vec3 *pts, int n, uint32_t color, const TexInfo &t) {
  // plan du polygone dans le monde (normale de Newell)
  double nx = 0, ny = 0, nz = 0;
  for (int i = 0; i < n; i++) {
    const Vec3 &a = pts[i], &b = pts[(i + 1) % n];
    nx += (a.y - b.y) * (a.z + b.z); ny += (a.z - b.z) * (a.x + b.x); nz += (a.x - b.x) * (a.y + b.y);
  }
  double l = std::sqrt(nx * nx + ny * ny + nz * nz);
  if (l < 1e-12) return;
  Vec3 N{nx / l, ny / l, nz / l};
  int ti = addTex(t, N, N.x * pts[0].x + N.y * pts[0].y + N.z * pts[0].z);
  Vec3 cam[32];
  for (int i = 0; i < n; i++) cam[i] = toCam(pts[i]);
  addClipped(cam, n, color, -1, ti);
}

void Renderer3D::polyFarTex(const Vec3 *pts, int n, uint32_t color, float depth, const TexInfo &t, double h) {
  int ti = addTex(t, Vec3{0, 1, 0}, h);
  Vec3 cam[32];
  for (int i = 0; i < n; i++) cam[i] = toCam(pts[i]);
  addClipped(cam, n, color, depth, ti);
}

namespace {
inline uint32_t hash2(int x, int y, uint32_t seed) {
  uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
// bruit de valeur lissé, -1..1
inline double vnoise(double x, double y, uint32_t seed) {
  double fx = std::floor(x), fy = std::floor(y);
  int ix = int(fx), iy = int(fy);
  double tx = x - fx, ty = y - fy;
  tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty);
  auto v = [&](int a, int b) { return (hash2(a, b, seed) & 0xffff) / 32767.5 - 1.0; };
  double a = v(ix, iy) + (v(ix + 1, iy) - v(ix, iy)) * tx, b = v(ix, iy + 1) + (v(ix + 1, iy + 1) - v(ix, iy + 1)) * tx;
  return a + (b - a) * ty;
}
// octave de bruit, atténuée quand elle devient plus fine que le pixel (pas de scintillement au loin)
inline double oct(double x, double y, double scale, double amp, double foot, uint32_t seed) {
  double k = std::clamp(1.5 - foot / scale * 2.0, 0.0, 1.0);
  return k > 0 ? vnoise(x / scale, y / scale, seed) * amp * k : 0.0;
}
inline uint32_t mulRGB(uint32_t c, double k) {
  auto ch = [&](int s) { return uint32_t(std::clamp(int(((c >> s) & 255) * k + 0.5), 0, 255)) << s; };
  return 0xff000000u | ch(16) | ch(8) | ch(0);
}

// texture de luminance répétable 256×256 précalculée (somme d'octaves de bruit périodique) avec ses
// niveaux de mip-map : la lecture coûte deux lectures bilinéaires au lieu de plusieurs bruits par pixel
struct MipTex {
  static constexpr int N = 256;
  std::vector<std::vector<float>> lv;   // niveau 0 : N×N, puis N/2…1
  MipTex(std::initializer_list<std::pair<int, float>> octaves, uint32_t seed) {
    std::vector<float> a(size_t(N) * N, 0.f);
    for (auto [period, amp] : octaves) {   // période en texels (divise N : texture répétable)
      int cells = N / period;
      for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
          float fx = float(x) / period, fy = float(y) / period;
          int ix = int(fx), iy = int(fy);
          float tx = fx - ix, ty = fy - iy;
          tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty);
          auto v = [&](int i, int j) { return (hash2(i % cells, j % cells, seed + uint32_t(period)) & 0xffff) / 32767.5f - 1.f; };
          float p = v(ix, iy) + (v(ix + 1, iy) - v(ix, iy)) * tx, q = v(ix, iy + 1) + (v(ix + 1, iy + 1) - v(ix, iy + 1)) * tx;
          a[size_t(y) * N + x] += (p + (q - p) * ty) * amp;
        }
    }
    lv.push_back(a);
    for (int n = N / 2; n >= 1; n /= 2) {
      const std::vector<float> &b = lv.back();
      std::vector<float> c(size_t(n) * n);
      for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
          c[size_t(y) * n + x] = 0.25f * (b[size_t(2 * y) * 2 * n + 2 * x] + b[size_t(2 * y) * 2 * n + 2 * x + 1] +
                                          b[size_t(2 * y + 1) * 2 * n + 2 * x] + b[size_t(2 * y + 1) * 2 * n + 2 * x + 1]);
      lv.push_back(std::move(c));
    }
  }
  // u, v en texels du niveau 0 ; foot = taille du pixel en texels (choix du niveau)
  float sample(double u, double v, double foot) const {
    int l = 0;
    while (foot > 1.0 && l + 1 < int(lv.size())) { foot *= 0.5; u *= 0.5; v *= 0.5; l++; }
    const int n = N >> l, m = n - 1;
    const std::vector<float> &t = lv[size_t(l)];
    u -= 0.5; v -= 0.5;
    double fu = std::floor(u), fv = std::floor(v);
    int x = int(int64_t(fu) & m), y = int(int64_t(fv) & m), x1 = (x + 1) & m, y1 = (y + 1) & m;
    float tx = float(u - fu), ty = float(v - fv);
    float a = t[size_t(y) * n + x] + (t[size_t(y) * n + x1] - t[size_t(y) * n + x]) * tx;
    float b = t[size_t(y1) * n + x] + (t[size_t(y1) * n + x1] - t[size_t(y1) * n + x]) * tx;
    return a + (b - a) * ty;
  }
};
// sol : grandes plaques (texture « macro », 16 unités par texel) + touffes et grain (0,25 unité par texel)
const MipTex &groundMacro() { static const MipTex t({{64, 0.16f}, {16, 0.10f}}, 1); return t; }
const MipTex &groundDetail() { static const MipTex t({{128, 0.08f}, {32, 0.09f}, {8, 0.11f}, {2, 0.09f}}, 2); return t; }
const MipTex &roadDetail() { static const MipTex t({{256, 0.06f}, {64, 0.05f}, {8, 0.07f}, {2, 0.06f}}, 3); return t; }
const MipTex &wallDetail() { static const MipTex t({{64, 0.06f}, {8, 0.07f}}, 4); return t; }
}  // namespace

void Renderer3D::warmTextures() { groundMacro(); groundDetail(); roadDetail(); wallDetail(); }

// couleur d'un pixel texturé : p = point du monde, foot = taille du pixel sur la surface (unités du monde)
uint32_t Renderer3D::shade(const TexPlane &tp, uint32_t color, const Vec3 &p, double foot) const {
  const TexInfo &t = tp.t;
  double k = 1;
  switch (t.kind) {
    case TEX_GROUND:   // herbe rase et terre : grandes plaques, touffes, grain
      k += groundMacro().sample(p.x / 16, p.z / 16, foot / 16) + groundDetail().sample(p.x * 4, p.z * 4, foot * 4);
      break;
    case TEX_ROAD: {   // béton : taches, grain fin, traces de pneus sombres dans le sens de la route
      k += roadDetail().sample(p.x * 4, p.z * 4, foot * 4) + groundMacro().sample(p.z / 8, p.x / 8, foot / 8) * 0.5;
      double s = (p.x - t.o.x) * t.u.x + (p.y - t.o.y) * t.u.y + (p.z - t.o.z) * t.u.z;
      double ss = (s - t.s0) / std::max(1e-6, t.s1 - t.s0);
      double wtr = std::max(0.035, foot / std::max(1e-6, t.s1 - t.s0));   // traces adoucies au loin
      double tr = std::exp(-std::pow((ss - 0.3) / (wtr + 0.05), 2)) + std::exp(-std::pow((ss - 0.7) / (wtr + 0.05), 2));
      k -= 0.09 * tr * std::min(1.0, 0.06 / (wtr + 0.025));
      break;
    }
    case TEX_WALL: {   // panneaux : bandes horizontales et grain
      double sy = p.y / 24.0;
      double band = std::fabs(sy - std::floor(sy) - 0.5);
      double kb = std::clamp(1.5 - foot / 24.0 * 3.0, 0.0, 1.0);
      k += (band > 0.45 ? -0.12 : 0.0) * kb + wallDetail().sample((p.x + p.z) * 2, p.y * 2, foot * 2);
      break;
    }
    case TEX_PAINT: {  // carrosserie peinte : dégradé, reflet, joints de panneaux
      Vec3 d{p.x - t.o.x, p.y - t.o.y, p.z - t.o.z};
      double s = d.x * t.u.x + d.y * t.u.y + d.z * t.u.z, w = d.x * t.v.x + d.y * t.v.y + d.z * t.v.z;
      double ws = std::clamp((w - t.w0) / std::max(1e-6, t.w1 - t.w0), 0.0, 1.0);
      double ss = std::clamp((s - t.s0) / std::max(1e-6, t.s1 - t.s0), 0.0, 1.0);
      k = 0.86 + 0.24 * ws;                                                  // plus clair vers le haut
      k += 0.18 * std::exp(-std::pow((ws - 0.72) * 9, 2));                   // reflet
      double e = std::min(std::min(s - t.s0, t.s1 - s), std::min(w - t.w0, t.w1 - w));
      double seam = std::max(1.2, foot * 1.5);
      if (e < seam) k *= 0.62;                                               // joint au bord du panneau
      else if (std::fabs(ss - 0.5) * (t.s1 - t.s0) < seam * 0.6 && t.s1 - t.s0 > 30) k *= 0.8;   // joint central
      k += oct(s, w, 3, 0.025, foot, 31);
      break;
    }
    case TEX_TIRE: {   // bande de roulement : sculptures transversales
      Vec3 d{p.x - t.o.x, p.y - t.o.y, p.z - t.o.z};
      double a = std::atan2(d.x * t.v.x + d.y * t.v.y + d.z * t.v.z, d.x * t.u.x + d.y * t.u.y + d.z * t.u.z);
      double g = std::sin(a * 18);
      double kb = std::clamp(1.5 - foot / (t.r * 0.35) * 2.0, 0.0, 1.0);
      return mulRGB(0xff262626u, 1 + (g > 0.3 ? 0.55 : 0.0) * kb);
    }
    case TEX_RIM: {    // flanc du pneu, jante à trois branches, moyeu
      Vec3 d{p.x - t.o.x, p.y - t.o.y, p.z - t.o.z};
      double s = d.x * t.u.x + d.y * t.u.y + d.z * t.u.z, w = d.x * t.v.x + d.y * t.v.y + d.z * t.v.z;
      double r = std::sqrt(s * s + w * w) / t.r;
      if (r > 0.97) return 0xff181818u;
      if (r > 0.66) return mulRGB(0xff2a2a2au, 1 + 0.25 * (r - 0.66) / 0.31);   // flanc
      if (r > 0.6) return 0xff505050u;
      double a = std::atan2(w, s);
      if (r > 0.22 && r < 0.55 && std::fabs(std::sin(a * 3)) > 0.55) return mulRGB(0xff9a9a9au, 0.55);   // ajours
      uint32_t metal = r < 0.22 ? 0xffd8d8d8u : 0xffb8b8b8u;
      return mulRGB(metal, 0.85 + 0.3 * (0.5 - w / (t.r * 2)));
    }
    default: return color;
  }
  return mulRGB(color, k);
}

void Renderer3D::addClipped(const Vec3 *in, int n, uint32_t color, float fixedDepth, int tex) {
  const double zn = cam_.nearZ;
  // rejet rapide : tout derrière le plan proche
  bool any = false;
  for (int i = 0; i < n; i++) any |= in[i].z >= zn;
  if (!any) return;
  // découpage au plan proche
  Vec3 out[40];
  int m = 0;
  for (int i = 0; i < n; i++) {
    const Vec3 &a = in[i], &b = in[(i + 1) % n];
    bool ain = a.z >= zn, bin = b.z >= zn;
    if (ain) out[m++] = a;
    if (ain != bin) {
      double t = (zn - a.z) / (b.z - a.z);
      out[m++] = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, zn};
    }
  }
  if (m < 3) return;
  // projection en double, puis découpage aux bords d'une zone un peu plus grande que l'écran : un sommet
  // proche du plan proche se projette à des milliards de pixels, et en flottants simple précision les
  // bords du polygone deviendraient des escaliers de gros blocs (sol vu avec un fort roulis)
  const double f = cam_.focal, fy = cam_.focalY;
  double PX[48], PY[48], QX[48], QY[48];
  int k = m;
  for (int i = 0; i < m; i++) { PX[i] = cam_.cx + f * out[i].x / out[i].z; PY[i] = cam_.cy - fy * out[i].y / out[i].z; }
  const double gx0 = -W_ - 64.0, gx1 = 2.0 * W_ + 64, gy0 = -H_ - 64.0, gy1 = 2.0 * H_ + 64;
  for (int e = 0; e < 4 && k >= 3; e++) {
    auto inside = [&](double x, double y) { return e == 0 ? x >= gx0 : e == 1 ? x <= gx1 : e == 2 ? y >= gy0 : y <= gy1; };
    auto cut = [&](double ax, double ay, double bx, double by, double &x, double &y) {
      double lim = e == 0 ? gx0 : e == 1 ? gx1 : e == 2 ? gy0 : gy1;
      double t = e < 2 ? (lim - ax) / (bx - ax) : (lim - ay) / (by - ay);
      x = ax + (bx - ax) * t; y = ay + (by - ay) * t;
      if (e < 2) x = lim; else y = lim;
    };
    int q = 0;
    for (int i = 0; i < k && q < 46; i++) {
      int j = (i + 1) % k;
      bool ai = inside(PX[i], PY[i]), bi = inside(PX[j], PY[j]);
      if (ai) { QX[q] = PX[i]; QY[q] = PY[i]; q++; }
      if (ai != bi) { cut(PX[i], PY[i], PX[j], PY[j], QX[q], QY[q]); q++; }
    }
    k = q;
    for (int i = 0; i < k; i++) { PX[i] = QX[i]; PY[i] = QY[i]; }
  }
  if (k < 3) return;
  float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
  int first = (int)sx_.size();
  for (int i = 0; i < k; i++) {
    float X = float(PX[i]), Y = float(PY[i]);
    sx_.push_back(X); sy_.push_back(Y);
    xmin = std::min(xmin, X); xmax = std::max(xmax, X); ymin = std::min(ymin, Y); ymax = std::max(ymax, Y);
  }
  if (xmax < 0 || xmin > W_ || ymax < 0 || ymin > H_) { sx_.resize(first); sy_.resize(first); return; }
  // 1/z affine à l'écran : plan n·p = d (repère caméra), p = z·((sx-cx)/f, -(sy-cy)/fy, 1)
  float ia = 0, ib = 0, ic = 0;
  if (fixedDepth < 0) {
    const Vec3 &p0 = out[0];
    // normale par Newell (robuste pour un polygone quelconque)
    double nx = 0, ny = 0, nz = 0;
    for (int i = 0; i < m; i++) {
      const Vec3 &a = out[i], &b = out[(i + 1) % m];
      nx += (a.y - b.y) * (a.z + b.z); ny += (a.z - b.z) * (a.x + b.x); nz += (a.x - b.x) * (a.y + b.y);
    }
    double d = nx * p0.x + ny * p0.y + nz * p0.z;
    if (std::fabs(d) < 1e-9 * (std::fabs(nx) + std::fabs(ny) + std::fabs(nz) + 1e-30)) { sx_.resize(first); sy_.resize(first); return; }
    ia = float(nx / (f * d)); ib = float(-ny / (fy * d));
    ic = float((nz - nx * cam_.cx / f + ny * cam_.cy / fy) / d);
  }
  polys_.push_back({first, k, color, ia, ib, ic, ymin, ymax, fixedDepth, tex});
}

// lignes y ≡ k (mod n) : les lignes entrelacées équilibrent la charge entre cœurs (le sol texturé
// est en bas de l'image, le ciel en haut). Les surfaces texturées ne sont ombrées qu'une fois, après
// le test de profondeur de tous les polygones (ombrage différé : pas de calcul pour le sol caché par la route)
void Renderer3D::rasterBand(uint32_t *out, int k, int n) {
  for (int y = k; y < H_; y += n) {
    std::fill(out + size_t(y) * W_, out + size_t(y + 1) * W_, bg_);
    std::fill(zbuf_.begin() + size_t(y) * W_, zbuf_.begin() + size_t(y + 1) * W_, 0.f);
    std::fill(idbuf_.begin() + size_t(y) * W_, idbuf_.begin() + size_t(y + 1) * W_, -1);
  }
  for (const SPoly &p : polys_) {
    int ry0 = std::max(0, int(std::ceil(p.ymin - 0.5f))), ry1 = std::min(H_, int(std::ceil(p.ymax - 0.5f)));
    ry0 += ((k - ry0) % n + n) % n;   // première ligne de ce cœur
    const float *X = &sx_[p.first], *Y = &sy_[p.first];
    for (int y = ry0; y < ry1; y += n) {
      float yc = y + 0.5f, xl = 1e30f, xr = -1e30f;
      for (int i = 0; i < p.n; i++) {
        int j = (i + 1 == p.n) ? 0 : i + 1;
        float ya = Y[i], yb = Y[j];
        if ((ya <= yc && yc < yb) || (yb <= yc && yc < ya)) {
          float x = X[i] + (yc - ya) * (X[j] - X[i]) / (yb - ya);
          xl = std::min(xl, x); xr = std::max(xr, x);
        }
      }
      if (xl > xr) continue;
      int xa = std::max(0, int(std::ceil(xl - 0.5f))), xb = std::min(W_, int(std::ceil(xr - 0.5f)));
      if (xa >= xb) continue;
      uint32_t *o = out + size_t(y) * W_;
      float *zb = &zbuf_[size_t(y) * W_];
      int32_t *id = &idbuf_[size_t(y) * W_];
      const int32_t tid = p.tex;
      if (p.fixedDepth >= 0) {
        for (int x = xa; x < xb; x++)
          if (p.fixedDepth >= zb[x]) { zb[x] = p.fixedDepth; o[x] = p.color; id[x] = tid; }
      } else {
        float d = p.ia * (xa + 0.5f) + p.ib * yc + p.ic;
        for (int x = xa; x < xb; x++, d += p.ia)
          if (d > zb[x]) { zb[x] = d; o[x] = p.color; id[x] = tid; }
      }
    }
  }
  if (texs_.empty()) return;
  // ombrage des pixels texturés visibles, par suites de pixels du même polygone
  const double f = cam_.focal, fy = cam_.focalY;
  const double bx = M_[0][0] / f, by = M_[0][1] / f, bz = M_[0][2] / f;
  for (int y = k; y < H_; y += n) {
    uint32_t *o = out + size_t(y) * W_;
    const int32_t *id = &idbuf_[size_t(y) * W_];
    const double dy = -(y + 0.5 - cam_.cy) / fy;
    // direction du rayon dans le monde : M^T · (dx, dy, 1), affine en x
    const double ax = M_[1][0] * dy + M_[2][0], ay = M_[1][1] * dy + M_[2][1], az = M_[1][2] * dy + M_[2][2];
    for (int x = 0; x < W_;) {
      if (id[x] < 0) { x++; continue; }
      const int tid = id[x];
      int xe = x + 1;
      while (xe < W_ && id[xe] == tid) xe++;
      const TexPlane &tp = texs_[size_t(tid)];
      const double num = tp.d - (tp.n.x * cam_.pos.x + tp.n.y * cam_.pos.y + tp.n.z * cam_.pos.z);
      // point exact du monde et taille du pixel sur la surface pour le pixel x
      auto exact = [&](int xx, Vec3 &P, double &foot) {
        double dxc = xx + 0.5 - cam_.cx;
        double rx = ax + bx * dxc, ry = ay + by * dxc, rz = az + bz * dxc;
        double den = tp.n.x * rx + tp.n.y * ry + tp.n.z * rz;
        double t = std::fabs(den) > 1e-12 ? num / den : 1e7;
        if (t <= 0 || t > 1e7) t = 1e7;
        P = {cam_.pos.x + rx * t, cam_.pos.y + ry * t, cam_.pos.z + rz * t};
        double len = std::sqrt(rx * rx + ry * ry + rz * rz);
        foot = t * len / f / std::max(0.08, std::fabs(den) / len);   // plus grand en incidence rasante
      };
      const bool interp = tp.t.kind == TEX_GROUND || tp.t.kind == TEX_ROAD || tp.t.kind == TEX_WALL;
      if (!interp) {
        for (int xx = x; xx < xe; xx++) { Vec3 P; double foot; exact(xx, P, foot); o[xx] = shade(tp, o[xx], P, foot); }
      } else {
        // points exacts tous les 16 pixels, interpolation linéaire entre eux (écart de perspective invisible)
        for (int s0 = x; s0 < xe; s0 += 16) {
          int s1 = std::min(xe - 1, s0 + 16);
          Vec3 P0, P1; double f0, f1;
          exact(s0, P0, f0); exact(s1, P1, f1);
          double inv = 1.0 / std::max(1, s1 - s0);
          Vec3 dP{(P1.x - P0.x) * inv, (P1.y - P0.y) * inv, (P1.z - P0.z) * inv};
          double df = (f1 - f0) * inv;
          int last = std::min(xe, s0 + 16);
          for (int xx = s0; xx < last; xx++) {
            int j = xx - s0;
            o[xx] = shade(tp, o[xx], Vec3{P0.x + dP.x * j, P0.y + dP.y * j, P0.z + dP.z * j}, f0 + df * j);
          }
        }
      }
      x = xe;
    }
  }
}

void Renderer3D::finish(uint32_t *out) {
  int n = std::min(threads_, H_);
  if (n <= 1) { rasterBand(out, 0, 1); return; }
  std::vector<std::thread> th;
  for (int k = 0; k < n; k++) th.emplace_back([this, out, k, n] { rasterBand(out, k, n); });
  for (auto &t : th) t.join();
}

}  // namespace scr
