// scenery.cpp — placement du décor autour des circuits (voir scenery.hpp)
#include "scenery.hpp"

#include <algorithm>
#include <cmath>

namespace scr {
namespace {

struct Rng {
  uint32_t s;
  uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  double uni() { return (next() & 0xffffff) / double(0x1000000); }
};

// distance au circuit (bord de la route compris) sur une grille, plafonnée à CAP
struct Clearance {
  static constexpr double CELL = 128, CAP = 3000;
  double x0 = 0, z0 = 0;
  int nx = 0, nz = 0;
  std::vector<float> d;
  explicit Clearance(const Track &t) {
    double mnx = 1e9, mnz = 1e9, mxx = -1e9, mxz = -1e9;
    for (auto &s : t.secs) {
      mnx = std::min({mnx, double(s.lx), double(s.rx)}); mxx = std::max({mxx, double(s.lx), double(s.rx)});
      mnz = std::min({mnz, double(s.lz), double(s.rz)}); mxz = std::max({mxz, double(s.lz), double(s.rz)});
    }
    const double M = 16000;   // le décor s'étend autour du circuit
    x0 = mnx - M; z0 = mnz - M;
    nx = int((mxx - mnx + 2 * M) / CELL) + 1; nz = int((mxz - mnz + 2 * M) / CELL) + 1;
    d.assign(size_t(nx) * nz, float(CAP));
    const size_t n = t.secs.size();
    for (size_t i = 0; i < n; i++) {
      const Section &a = t.secs[i], &b = t.secs[(i + 1) % n];
      // segment entre les milieux, demi-largeur de la route
      const double ax = (a.lx + a.rx) / 2, az = (a.lz + a.rz) / 2, bx = (b.lx + b.rx) / 2, bz = (b.lz + b.rz) / 2;
      const double hw = std::max(std::hypot(a.rx - a.lx, a.rz - a.lz), std::hypot(b.rx - b.lx, b.rz - b.lz)) / 2;
      const double ex = bx - ax, ez = bz - az, el = ex * ex + ez * ez;
      int cx0 = int((std::min(ax, bx) - CAP - hw - x0) / CELL), cx1 = int((std::max(ax, bx) + CAP + hw - x0) / CELL);
      int cz0 = int((std::min(az, bz) - CAP - hw - z0) / CELL), cz1 = int((std::max(az, bz) + CAP + hw - z0) / CELL);
      cx0 = std::max(cx0, 0); cz0 = std::max(cz0, 0); cx1 = std::min(cx1, nx - 1); cz1 = std::min(cz1, nz - 1);
      for (int gz = cz0; gz <= cz1; gz++)
        for (int gx = cx0; gx <= cx1; gx++) {
          const double px = x0 + (gx + 0.5) * CELL, pz = z0 + (gz + 0.5) * CELL;
          double k = el > 1e-9 ? ((px - ax) * ex + (pz - az) * ez) / el : 0;
          k = std::clamp(k, 0.0, 1.0);
          const double dd = std::hypot(px - ax - ex * k, pz - az - ez * k) - hw;
          float &c = d[size_t(gz) * nx + gx];
          c = std::min(c, float(std::max(0.0, dd)));
        }
    }
  }
  double at(double x, double z) const {
    int gx = int((x - x0) / CELL), gz = int((z - z0) / CELL);
    if (gx < 0 || gz < 0 || gx >= nx || gz >= nz) return CAP;
    return d[size_t(gz) * nx + gx];
  }
  // plus petite distance au circuit sur un rectangle (milieu de façade, direction de façade, longueur, profondeur)
  double rect(double x, double z, double fx, double fz, double len, double depth) const {
    double m = CAP;
    for (double a = -len / 2; a <= len / 2 + 1; a += 100)
      for (double b = 0; b <= depth + 1; b += 100)
        m = std::min(m, at(x + fz * a - fx * b, z - fx * a - fz * b));
    return m;
  }
};

}  // namespace

std::vector<SceneObj> buildScenery(const Track &t) {
  std::vector<SceneObj> out;
  const size_t n = t.secs.size();
  if (n < 4) return out;
  Clearance cl(t);
  uint32_t h = 2166136261u;
  for (auto &s : t.secs) { h = (h ^ uint32_t(int(s.lx) * 31 + int(s.lz))) * 16777619u; }
  Rng rng{h | 1};
  struct Disk { double x, z, r; };
  std::vector<Disk> taken;
  auto freeAt = [&](double x, double z, double r) {
    for (auto &d : taken) if (std::hypot(d.x - x, d.z - z) < d.r + r) return false;
    return true;
  };
  // repère d'une section : milieu, direction de la route, normale vers la droite, demi-largeur
  auto frame = [&](size_t i, double &cx, double &cz, double &nx, double &nz, double &hw) {
    const Section &s = t.secs[i];
    cx = (s.lx + s.rx) / 2; cz = (s.lz + s.rz) / 2;
    nx = s.rx - s.lx; nz = s.rz - s.lz; hw = std::hypot(nx, nz) / 2;
    nx /= 2 * hw; nz /= 2 * hw;
  };
  // tribunes et panneaux le long de la route, de part et d'autre, tournés vers elle
  auto alongside = [&](int kind, int count, double off, double len, double depth, double minClear) {
    for (int k = 0; k < count; k++) {
      for (int tries = 0; tries < 12; tries++) {
        size_t i = (size_t(k) * n / size_t(count) + rng.next() % std::max<size_t>(1, n / size_t(count))) % n;
        double cx, cz, nx, nz, hw;
        frame(i, cx, cz, nx, nz, hw);
        const double side = rng.next() & 1 ? 1 : -1, d = hw + off + rng.uni() * off * 0.4;
        const double x = cx + nx * side * d, z = cz + nz * side * d, fx = -nx * side, fz = -nz * side;
        const double r = std::max(len, depth) * 0.6;
        if (cl.rect(x, z, fx, fz, len, depth) < minClear || !freeAt(x - fx * depth / 2, z - fz * depth / 2, r)) continue;
        taken.push_back({x - fx * depth / 2, z - fz * depth / 2, r});
        out.push_back({kind, x, z, fx, fz, len, 1, rng.next()});
        break;
      }
    }
  };
  alongside(SCENE_STAND, 6, 1100, 1500, 700, 700);
  alongside(SCENE_BOARD, 14, 480, 520, 40, 260);
  // arbres : bosquets autour du circuit (plus serrés dans certaines zones), jamais près de la route
  const double bx0 = cl.x0 + 2000, bz0 = cl.z0 + 2000, bw = cl.nx * cl.CELL - 4000, bd = cl.nz * cl.CELL - 4000;
  std::vector<Disk> groves;
  for (int g = 0; g < 14; g++) groves.push_back({bx0 + rng.uni() * bw, bz0 + rng.uni() * bd, 2500 + rng.uni() * 3500});
  int placed = 0;
  for (int tries = 0; tries < 6000 && placed < 320; tries++) {
    double x, z;
    if (rng.uni() < 0.75) {   // dans un bosquet
      const Disk &g = groves[rng.next() % groves.size()];
      const double a = rng.uni() * 6.2831853, r = std::sqrt(rng.uni()) * g.r;
      x = g.x + std::cos(a) * r; z = g.z + std::sin(a) * r;
    } else { x = bx0 + rng.uni() * bw; z = bz0 + rng.uni() * bd; }
    const double size = 0.75 + rng.uni() * 0.6;
    if (cl.at(x, z) < 650 + 200 * size || !freeAt(x, z, 110 * size)) continue;
    taken.push_back({x, z, 110 * size});
    out.push_back({rng.uni() < 0.55 ? SCENE_PINE : SCENE_TREE, x, z, 0, 1, 0, size, rng.next()});
    placed++;
  }
  return out;
}

}  // namespace scr
