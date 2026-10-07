// hdview.cpp — voir hdview.hpp
#include "hdview.hpp"

#include <algorithm>
#include <cmath>

namespace scr {
namespace {

constexpr uint32_t A_POS = 0x10ac2, A_PITCH = 0x10ace, A_YAW = 0x10ad0, A_ROLL = 0x10ad2, A_TRACK = 0x1112d;
constexpr double PI = 3.14159265358979323846;
constexpr double ANG = 2 * PI / 65536;
constexpr double GROUND_RAW = 512;    // niveau du sol (hauteur brute)

double wrapPi(double a) {
  while (a > PI) a -= 2 * PI;
  while (a < -PI) a += 2 * PI;
  return a;
}

uint32_t shadeARGB(uint32_t c, double k) {
  auto ch = [&](int s) { return uint32_t(std::clamp(int(((c >> s) & 255) * k + 0.5), 0, 255)) << s; };
  return 0xff000000u | ch(16) | ch(8) | ch(0);
}

// collines de l'horizon : deux couches déterministes
struct Hills {
  double h[2][65];
  Hills() {
    uint32_t r = 12345;
    auto rnd = [&] { r = r * 1103515245u + 12345u; return ((r >> 8) & 0xffff) / 65535.0; };
    for (int l = 0; l < 2; l++) {
      double v = 0.03;
      for (int i = 0; i <= 64; i++) {
        v += (rnd() - 0.5) * (l ? 0.02 : 0.035);
        v = std::clamp(v, 0.004, l ? 0.045 : 0.07);
        h[l][i] = v;
      }
      h[l][64] = h[l][0];
    }
  }
};
const Hills HILLS;

}  // namespace

HdView::HdView(const Machine &m) : tracks_(decodeTracks(m.image())) {}

void HdView::afterFrame(const Machine &m) {
  frame_++;
  if (m.ticks() != lastTicks_) {
    lastTicks_ = m.ticks();
    if (lastTickFrame_) {
      intervals_.push_back(double(frame_ - lastTickFrame_));
      while (intervals_.size() > 8) intervals_.pop_front();
      // retard = plus long intervalle récent (+1 trame : l'état est relevé une trame après le tick)
      double mx = 0;
      for (double d : intervals_) mx = std::max(mx, std::min(d, 30.0));
      double target = mx + 1;
      // le retard s'adapte vite à la hausse, lentement à la baisse (pas d'à-coups)
      tickPeriod_ = target > tickPeriod_ ? target : 0.95 * tickPeriod_ + 0.05 * target;
    }
    lastTickFrame_ = frame_;
    // la physique est terminée bien avant la fin de la trame suivante : on lit l'état alors
    pendingSnap_ = true;
    return;
  }
  if (pendingSnap_) {
    pendingSnap_ = false;
    Snap s;
    s.t = frame_;
    s.p.x = int32_t(m.l(A_POS)) / 65536.0 * 16;
    s.p.y = int32_t(m.l(A_POS + 4)) / 65536.0 * 16;
    s.p.z = int32_t(m.l(A_POS + 8)) / 65536.0 * 16;
    s.p.pitch = int16_t(m.w(A_PITCH)) * ANG;
    s.p.yaw = m.w(A_YAW) * ANG;
    s.p.roll = int16_t(m.w(A_ROLL)) * ANG;
    s.track = m.b(A_TRACK) & 7;
    snaps_.push_back(s);
    while (snaps_.size() > 32) snaps_.pop_front();
  }
}

Pose HdView::poseFromState(const uint8_t *b) {
  auto L = [&](int o) { return int32_t((uint32_t(b[o]) << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]); };
  auto W = [&](int o) { return uint16_t((b[o] << 8) | b[o + 1]); };
  Pose p;
  p.x = L(0) / 65536.0 * 16; p.y = L(4) / 65536.0 * 16; p.z = L(8) / 65536.0 * 16;
  p.pitch = int16_t(W(12)) * ANG; p.yaw = W(14) * ANG; p.roll = int16_t(W(16)) * ANG;
  return p;
}

double HdView::playTime(double t) {
  double target = t - delay();
  double dt = t - lastT_;
  lastT_ = t;
  if (dt < 0 || dt > 25 || std::fabs(target - playT_) > 25) playT_ = target;          // reprise / saut
  else playT_ += dt * std::clamp(1 + 0.08 * (target - playT_), 0.75, 1.25);
  return playT_;
}

bool HdView::racing() const { return !snaps_.empty() && frame_ - lastTickFrame_ < 40; }

Pose HdView::poseAt(double t) const {
  if (snaps_.empty()) return {};
  if (t <= snaps_.front().t) return snaps_.front().p;
  for (size_t i = 0; i + 1 < snaps_.size(); i++) {
    const Snap &a = snaps_[i], &b = snaps_[i + 1];
    if (t < b.t) {
      double k = (t - a.t) / (b.t - a.t);
      double d = std::fabs(b.p.x - a.p.x) + std::fabs(b.p.y - a.p.y) + std::fabs(b.p.z - a.p.z);
      if (d > 3000 || a.track != b.track) return k < 0.5 ? a.p : b.p;   // grue / changement de circuit
      Pose p;
      p.x = a.p.x + (b.p.x - a.p.x) * k; p.y = a.p.y + (b.p.y - a.p.y) * k; p.z = a.p.z + (b.p.z - a.p.z) * k;
      p.yaw = a.p.yaw + wrapPi(b.p.yaw - a.p.yaw) * k;
      p.pitch = a.p.pitch + wrapPi(b.p.pitch - a.p.pitch) * k;
      p.roll = a.p.roll + wrapPi(b.p.roll - a.p.roll) * k;
      return p;
    }
  }
  return snaps_.back().p;
}

void HdView::renderScene(const Pose &p, int track, uint32_t *out, int W, int H, double focal, double cx, double cy,
                         const Machine &m) {
  const HdParams &P = params;
  // couleurs : palette courante du jeu
  auto pal = [&](int i) { return m.paletteARGB(i); };
  uint32_t sky = pal(7), ground = pal(13), hill = pal(5), hillFar = shadeARGB(pal(5), 1.25), road = pal(1),
           road2 = pal(2), line = pal(3), wall = pal(10), wall2 = pal(15);

  // caméra : position de la voiture + œil, dans le repère de la voiture
  Camera c;
  double pitch = P.pitchSign * p.pitch, roll = P.rollSign * p.roll;
  double sy = std::sin(p.yaw), cyw = std::cos(p.yaw);
  const double HSCALE = P.hScale;
  c.pos = {p.x + sy * P.eyeFwd + cyw * P.eyeSide, p.y * HSCALE / 0.5 + P.eyeUp, p.z + cyw * P.eyeFwd - sy * P.eyeSide};
  c.yaw = p.yaw + P.yawOffset; c.pitch = pitch + P.pitchOffset; c.roll = roll;
  c.focal = focal; c.focalY = focal * P.aspect; c.cx = cx; c.cy = cy; c.nearZ = 2;
  r3d_.begin(W, H, c, sky);

  // sol et collines (au plus loin, dans l'ordre)
  const double gy = GROUND_RAW * HSCALE, R = 2e6;
  Vec3 g[4] = {{c.pos.x - R, gy, c.pos.z - R}, {c.pos.x + R, gy, c.pos.z - R}, {c.pos.x + R, gy, c.pos.z + R}, {c.pos.x - R, gy, c.pos.z + R}};
  r3d_.polyFar(g, 4, ground, 1e-12f);
  const double dist = 1e6;
  for (int l = 1; l >= 0; l--) {
    for (int i = 0; i < 64; i++) {
      double a0 = i / 64.0 * 2 * PI, a1 = (i + 1) / 64.0 * 2 * PI, k = l ? 1.25 : 1;
      Vec3 q[4] = {{c.pos.x + std::sin(a0) * dist, gy, c.pos.z + std::cos(a0) * dist},
                   {c.pos.x + std::sin(a1) * dist, gy, c.pos.z + std::cos(a1) * dist},
                   {c.pos.x + std::sin(a1) * dist, gy + HILLS.h[l][i + 1] * dist * k, c.pos.z + std::cos(a1) * dist},
                   {c.pos.x + std::sin(a0) * dist, gy + HILLS.h[l][i] * dist * k, c.pos.z + std::cos(a0) * dist}};
      r3d_.polyFar(q, 4, l ? hillFar : hill, l ? 2e-12f : 3e-12f);
    }
  }

  // circuit
  const Track &tr = tracks_[track & 7];
  const size_t n = tr.secs.size();
  const double lw = 0.035;    // largeur des lignes de bord (fraction de la largeur de route)
  for (size_t i = 0; i < n; i++) {
    const Section &A = tr.secs[i], &B = tr.secs[(i + 1) % n];
    Vec3 aL{A.lx, A.ly * HSCALE, A.lz}, aR{A.rx, A.ry * HSCALE, A.rz}, bL{B.lx, B.ly * HSCALE, B.lz}, bR{B.rx, B.ry * HSCALE, B.rz};
    // flancs jusqu'au sol, rouges et blancs en alternance par pièce
    uint32_t wc = (A.piece & 1) ? wall2 : wall;
    auto wallQuad = [&](const Vec3 &p0, const Vec3 &p1) {
      if (p0.y <= gy + 0.5 && p1.y <= gy + 0.5) return;
      Vec3 q[4] = {p0, p1, {p1.x, gy, p1.z}, {p0.x, gy, p0.z}};
      r3d_.poly(q, 4, wc);
    };
    wallQuad(aL, bL);
    wallQuad(bR, aR);
    // dessus de la route (pas pour les sections verticales des sauts), couleur alternée par pièce
    double horiz = std::hypot((B.lx + B.rx - A.lx - A.rx) * 0.5, (B.lz + B.rz - A.lz - A.rz) * 0.5);
    if (horiz < 1) continue;
    uint32_t rc = (A.piece & 1) ? road2 : road;
    auto lerp = [](const Vec3 &a, const Vec3 &b, double t) { return Vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; };
    Vec3 aL1 = lerp(aL, aR, lw), aR1 = lerp(aR, aL, lw), bL1 = lerp(bL, bR, lw), bR1 = lerp(bR, bL, lw);
    Vec3 mid[4] = {aL1, aR1, bR1, bL1};
    r3d_.poly(mid, 4, rc);
    Vec3 ql[4] = {aL, aL1, bL1, bL};
    r3d_.poly(ql, 4, line);
    Vec3 qr[4] = {aR1, aR, bR, bR1};
    r3d_.poly(qr, 4, line);
  }
  r3d_.finish(out);
}

void HdView::render(const Machine &m, double t, uint32_t *out, int W, int H) {
  const double s = H / 200.0, ox = (W - 320 * s) / 2;
  if (!racing()) {
    // hors course : écran d'origine agrandi, centré
    static uint32_t st[320 * 200];
    m.screenARGB(st);
    std::fill(out, out + size_t(W) * H, 0xff000000u);
    for (int y = 0; y < H; y++) {
      int sy = std::min(199, int(y / s));
      for (int x = std::max(0, int(ox)); x < std::min(W, int(ox + 320 * s)); x++)
        out[size_t(y) * W + x] = st[sy * 320 + std::min(319, int((x - ox) / s))];
    }
    return;
  }
  double T = playTime(t);
  Pose p = poseAt(T);
  int track = snaps_.back().track;
  renderScene(p, track, out, W, H, params.focal * s, ox + params.cx * s, params.cy * s, m);
  if (!params.cockpit) return;
  overlay_.resize(320 * 200);
  m.overlayARGB(overlay_.data());
  if (mapW_ != W || mapH_ != H) {
    mapW_ = W; mapH_ = H;
    mapX_.assign(W, -1); mapY_.assign(H, -1);
    for (int x = 0; x < W; x++) { double u = (x + 0.5 - ox) / s; if (u >= 0 && u < 320) mapX_[x] = int(u); }
    for (int y = 0; y < H; y++) mapY_[y] = std::min(199, int((y + 0.5) / s));
  }
  for (int y = 0; y < H; y++) {
    const uint32_t *src = &overlay_[mapY_[y] * 320];
    uint32_t *o = out + size_t(y) * W;
    for (int x = 0; x < W; x++) {
      int u = mapX_[x];
      if (u >= 0 && src[u]) o[x] = src[u];
    }
  }
}

}  // namespace scr
