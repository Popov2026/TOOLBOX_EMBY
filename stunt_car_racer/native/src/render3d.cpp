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
  polys_.clear(); sx_.clear(); sy_.clear();
  if ((int)zbuf_.size() != W * H) zbuf_.assign(size_t(W) * H, 0.f);
}

Vec3 Renderer3D::toCam(const Vec3 &p) const {
  double x = p.x - cam_.pos.x, y = p.y - cam_.pos.y, z = p.z - cam_.pos.z;
  double x1 = x * cyw_ - z * syw_, z1 = x * syw_ + z * cyw_;           // lacet
  double y2 = y * cp_ - z1 * sp_, z2 = y * sp_ + z1 * cp_;             // tangage
  double x3 = x1 * cr_ - y2 * sr_, y3 = y2 * cr_ + x1 * sr_;           // roulis
  return {x3, y3, z2};
}

void Renderer3D::poly(const Vec3 *pts, int n, uint32_t color) {
  Vec3 cam[8];
  for (int i = 0; i < n; i++) cam[i] = toCam(pts[i]);
  addClipped(cam, n, color, -1);
}

void Renderer3D::polyFar(const Vec3 *pts, int n, uint32_t color, float depth) {
  Vec3 cam[8];
  for (int i = 0; i < n; i++) cam[i] = toCam(pts[i]);
  addClipped(cam, n, color, depth);
}

void Renderer3D::addClipped(const Vec3 *in, int n, uint32_t color, float fixedDepth) {
  const double zn = cam_.nearZ;
  // rejet rapide : tout derrière le plan proche
  bool any = false;
  for (int i = 0; i < n; i++) any |= in[i].z >= zn;
  if (!any) return;
  // découpage au plan proche
  Vec3 out[16];
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
  polys_.push_back({first, k, color, ia, ib, ic, ymin, ymax, fixedDepth});
}

void Renderer3D::rasterBand(uint32_t *out, int y0, int y1) {
  for (int y = y0; y < y1; y++) {
    std::fill(out + size_t(y) * W_, out + size_t(y + 1) * W_, bg_);
    std::fill(zbuf_.begin() + size_t(y) * W_, zbuf_.begin() + size_t(y + 1) * W_, 0.f);
  }
  for (const SPoly &p : polys_) {
    int ry0 = std::max(y0, int(std::ceil(p.ymin - 0.5f))), ry1 = std::min(y1, int(std::ceil(p.ymax - 0.5f)));
    const float *X = &sx_[p.first], *Y = &sy_[p.first];
    for (int y = ry0; y < ry1; y++) {
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
      if (p.fixedDepth >= 0) {
        for (int x = xa; x < xb; x++)
          if (p.fixedDepth >= zb[x]) { zb[x] = p.fixedDepth; o[x] = p.color; }
      } else {
        float d = p.ia * (xa + 0.5f) + p.ib * yc + p.ic;
        for (int x = xa; x < xb; x++, d += p.ia)
          if (d > zb[x]) { zb[x] = d; o[x] = p.color; }
      }
    }
  }
}

void Renderer3D::finish(uint32_t *out) {
  int n = std::min(threads_, H_);
  if (n <= 1) { rasterBand(out, 0, H_); return; }
  std::vector<std::thread> th;
  for (int k = 0; k < n; k++) {
    int a = H_ * k / n, b = H_ * (k + 1) / n;
    th.emplace_back([this, out, a, b] { rasterBand(out, a, b); });
  }
  for (auto &t : th) t.join();
}

}  // namespace scr
