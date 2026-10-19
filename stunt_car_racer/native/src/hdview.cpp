// hdview.cpp — voir hdview.hpp
#include "hdview.hpp"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <thread>
#include <cstdio>
#include <cstdlib>

namespace scr {
namespace {

constexpr uint32_t A_POS = 0x10ac2, A_PITCH = 0x10ace, A_YAW = 0x10ad0, A_ROLL = 0x10ad2, A_TRACK = 0x1112d;
constexpr double PI = 3.14159265358979323846;
constexpr double ANG = 2 * PI / 65536;
constexpr double GROUND_RAW = 512;    // niveau du sol (hauteur brute)
constexpr double START_LINE_W = 40;   // épaisseur de la ligne de départ (unités du monde)

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

HdView::HdView(const Machine &m) : tracks_(decodeTracks(m.image(), m.trackSet())) { Renderer3D::warmTextures(); }

// flamme du boost en haute définition, tirée pixel pour pixel du sprite d'origine : même silhouette
// (les 4 flammes de chaque côté), même place, mêmes couleurs (bord jaune, cœur blanc) ; les contours
// en escalier sont lissés par un agrandissement filtré, avec un léger halo autour.
const HdView::FlameImg &HdView::flameFor(int id, double s, const Machine &m) {
  FlameImg &F = flames_[id];
  if (F.scale == s && !F.px.empty()) return F;
  F = FlameImg{};
  F.scale = s;
  std::vector<int> px;
  int w, h;
  if (!m.spritePixels(id, px, w, h)) return F;
  // champs à la résolution d'origine : présence de flamme et cœur blanc, avec un pixel de marge
  const int M = 4, sw = w + 2 * M, sh = h + 2 * M;
  std::vector<float> fire(size_t(sw) * sh, 0.f), core(fire.size(), 0.f);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      int v = px[size_t(y) * w + x];
      if (v < 0) continue;
      uint32_t c = m.paletteARGB(v);
      int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
      size_t k = size_t(y + M) * sw + x + M;
      fire[k] = 1.f;
      core[k] = (b > 128 && r > 128 && g > 128) ? 1.f : 0.f;
    }
  // flou gaussien séparable (sigma en pixels d'origine)
  auto blur = [&](const std::vector<float> &in, double sigma) {
    int rad = std::max(1, int(std::ceil(sigma * 3)));
    std::vector<float> k(2 * rad + 1), tmp(in.size()), out(in.size());
    float sum = 0;
    for (int i = -rad; i <= rad; i++) sum += k[i + rad] = float(std::exp(-i * i / (2 * sigma * sigma)));
    for (auto &v : k) v /= sum;
    for (int y = 0; y < sh; y++)
      for (int x = 0; x < sw; x++) {
        float a = 0;
        for (int i = -rad; i <= rad; i++) a += k[i + rad] * in[size_t(y) * sw + std::clamp(x + i, 0, sw - 1)];
        tmp[size_t(y) * sw + x] = a;
      }
    for (int y = 0; y < sh; y++)
      for (int x = 0; x < sw; x++) {
        float a = 0;
        for (int i = -rad; i <= rad; i++) a += k[i + rad] * tmp[size_t(std::clamp(y + i, 0, sh - 1)) * sw + x];
        out[size_t(y) * sw + x] = a;
      }
    return out;
  };
  std::vector<float> fireS = blur(fire, 0.6), coreS = blur(core, 0.7), halo = blur(fire, 2.0);
  auto sample = [&](const std::vector<float> &f, double u, double v) {   // bilinéaire, u/v en pixels d'origine
    u -= 0.5; v -= 0.5;
    int x0 = int(std::floor(u)), y0 = int(std::floor(v));
    double tx = u - x0, ty = v - y0;
    auto at = [&](int x, int y) { return (x < 0 || y < 0 || x >= sw || y >= sh) ? 0.f : f[size_t(y) * sw + x]; };
    return (at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx) * (1 - ty) + (at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx) * ty;
  };
  auto smooth = [](double a, double b, double x) { double t = std::clamp((x - a) / (b - a), 0.0, 1.0); return t * t * (3 - 2 * t); };
  // couleurs du jeu : jaune (bord) et blanc (cœur)
  uint32_t yel = 0xffffff00u, wht = 0xffffffffu;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      int v = px[size_t(y) * w + x];
      if (v < 0) continue;
      uint32_t c = m.paletteARGB(v);
      if ((c & 255) > 128) wht = c; else yel = c;
    }
  F.ox = -M * s; F.oy = -M * s;
  F.w = int(std::ceil(sw * s)); F.h = int(std::ceil(sh * s));
  F.px.assign(size_t(F.w) * F.h * 4, 0.f);
  for (int Y = 0; Y < F.h; Y++)
    for (int X = 0; X < F.w; X++) {
      double u = (X + 0.5) / s, v = (Y + 0.5) / s;
      double a = smooth(0.38, 0.62, sample(fireS, u, v));          // contour lissé de la silhouette d'origine
      double wc = smooth(0.30, 0.65, sample(coreS, u, v));         // cœur blanc
      double g = sample(halo, u, v) * 0.35 * (1 - a);               // halo (lumière ajoutée)
      auto ch = [&](int sh8) {
        double cy = (yel >> sh8) & 255, cw = (wht >> sh8) & 255;
        return float((cy + (cw - cy) * wc) / 255.0);
      };
      float *o = &F.px[(size_t(Y) * F.w + X) * 4];
      o[0] = float(a);                                   // couverture
      o[1] = float(a) * ch(16); o[2] = float(a) * ch(8); o[3] = float(a) * ch(0);   // couleur prémultipliée
      // halo : jaune orangé additif
      o[1] += float(g * 1.0); o[2] += float(g * 0.75); o[3] += float(g * 0.15);
    }
  return F;
}

void HdView::drawFlame(int id, double x0, double y0, double s, const Machine &m, uint32_t *out, int W, int H) {
  const FlameImg &F = flameFor(id, s, m);
  if (F.px.empty()) return;
  int bx = int(std::lround(x0 + F.ox)), by = int(std::lround(y0 + F.oy));
  for (int Y = std::max(0, -by); Y < F.h && by + Y < H; Y++) {
    uint32_t *o = out + size_t(by + Y) * W;
    for (int X = std::max(0, -bx); X < F.w && bx + X < W; X++) {
      const float *f = &F.px[(size_t(Y) * F.w + X) * 4];
      if (f[0] <= 0.f && f[1] <= 0.f) continue;
      uint32_t c = o[bx + X];
      auto mix = [&](int sh8, float add) {
        return uint32_t(std::clamp(int(((c >> sh8) & 255) * (1 - f[0]) + add * 255 + 0.5f), 0, 255)) << sh8;
      };
      o[bx + X] = 0xff000000u | mix(16, f[1]) | mix(8, f[2]) | mix(0, f[3]);
    }
  }
}

void HdView::afterFrame(const Machine &m) {
  frame_++;
  {   // dégâts de la voiture du joueur : une hausse = choc ou réception brutale (étincelles, fumée)
    int d = m.b(0x10939) + m.b(0x1093a) + m.b(0x1093b);
    if (ownDamage_ >= 0 && d > ownDamage_) ownImpact_ = std::min(63, ownImpact_ + (d - ownDamage_) * 4);
    ownDamage_ = d;
  }
  {   // vent relatif : vitesse au sol de la voiture ($10AD4 / $10AD8)
    double vx = int16_t(m.w(0x10ad4)), vz = int16_t(m.w(0x10ad8));
    windTarget_ = std::clamp(std::sqrt(vx * vx + vz * vz) / 20000.0, 0.0, 1.0);
  }
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
    s.opp = opponentFromGame(m, s.track);
    s.oppImpact = oppImpact_;
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

static const int FLAME_IDS[] = {6, 7, 49, 8, 9, 50};   // flammes du boost (gauche / droite)

int HdView::loadAssets(Machine &m, const std::string &dir) {
  int n = dir.empty() ? 0 : assets.load(dir);
  for (auto &[id, spr] : assets.sprites()) m.watchSprite(id);
  if (params.builtinFlames)
    for (int id : FLAME_IDS) m.watchSprite(id);
  return n;
}

// image HD d'un sprite : celle du dossier hd/ (les flammes sans image sont dessinées par drawFlame)
const HdSprite *HdView::spriteFor(int id, const Machine &) { return assets.sprite(id); }

bool HdView::racing() const { return !snaps_.empty() && frame_ - lastTickFrame_ < 40; }

Pose HdView::poseAt(double t) const {
  if (snaps_.empty()) return {};
  if (t <= snaps_.front().t) return snaps_.front().p;
  for (size_t i = 0; i + 1 < snaps_.size(); i++) {
    const Snap &a = snaps_[i], &b = snaps_[i + 1];
    // au-delà du dernier état connu (retard court) : trajectoire prolongée d'au plus 0,6 intervalle
    const bool last = i + 2 == snaps_.size();
    if (t < b.t || last) {
      double k = std::min((t - a.t) / (b.t - a.t), 1.6);
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

bool HdView::surfaceRaw(const Track &t, double x, double z, double yRef, double &y) {
  const int n = int(t.secs.size());
  if (!n) return false;
  int from = 0, to = n - 1;
  if (hint_ >= 0 && hint_ < n) { from = hint_ - 24; to = hint_ + 24; }
  double best = 1e30;
  bool found = false;
  for (int k = from; k <= to; k++) {
    int i = ((k % n) + n) % n;
    const Section &A = t.secs[i], &B = t.secs[(i + 1) % n];
    const double P[4][3] = {{A.lx, A.ly, A.lz}, {A.rx, A.ry, A.rz}, {B.rx, B.ry, B.rz}, {B.lx, B.ly, B.lz}};
    static const int tri[2][3] = {{0, 1, 2}, {0, 2, 3}};
    for (const auto &T : tri) {
      const double *a = P[T[0]], *b = P[T[1]], *c = P[T[2]];
      double v0x = b[0] - a[0], v0z = b[2] - a[2], v1x = c[0] - a[0], v1z = c[2] - a[2], v2x = x - a[0], v2z = z - a[2];
      double den = v0x * v1z - v1x * v0z;
      if (std::fabs(den) < 1e-9) continue;
      double u = (v2x * v1z - v1x * v2z) / den, v = (v0x * v2z - v2x * v0z) / den;
      if (u < -1e-6 || v < -1e-6 || u + v > 1 + 1e-6) continue;
      double h = a[1] + u * (b[1] - a[1]) + v * (c[1] - a[1]);
      double score = std::fabs(h - yRef) + (h > yRef + 400 ? 1e6 : 0);   // pas une route bien au-dessus (croisements)
      if (score < best) { best = score; y = h; hint_ = i; found = true; }
    }
  }
  if (!found && hint_ >= 0) { hint_ = -1; return surfaceRaw(t, x, z, yRef, y); }   // recherche complète
  return found;
}

// position de l'adversaire : pièce $10907, section $108F6 + fraction $108F7/256, position en travers
// de la route $109D6 (0 = bord gauche, 255 = bord droit). Hauteur : surface de la route, avec une
// trajectoire balistique au-dessus des sauts (la route s'y dérobe plus vite que la gravité).
OppPose HdView::opponentFromGame(const Machine &m, int track) {
  OppPose o;
  if (!m.opponentInRace() || !params.opponent3D) { oppHave_ = false; return o; }
  const Track &t = tracks_[track & 7];
  const int n = int(t.secs.size());
  int piece = m.b(0x10907), sec = m.b(0x108f6), first = -1;
  double fr = m.b(0x108f7) / 256.0, u = std::clamp(m.w(0x109d6) / 256.0, 0.0, 1.0);
  for (int i = 0; i < n; i++)
    if (t.secs[i].piece == piece) { first = i; break; }
  if (first < 0) { oppHave_ = false; return o; }
  int g0 = ((first - 1 + sec) % n + n) % n, g1 = (g0 + 1) % n;
  const Section &A = t.secs[g0], &B = t.secs[g1];
  auto L = [&](double a, double b) { return a + (b - a) * fr; };
  double lx = L(A.lx, B.lx), lz = L(A.lz, B.lz), rx = L(A.rx, B.rx), rz = L(A.rz, B.rz), hl = L(A.ly, B.ly), hr = L(A.ry, B.ry);
  o.x = lx + (rx - lx) * u; o.z = lz + (rz - lz) * u;
  double surf = hl + (hr - hl) * u;
  // repère : avant = sens de la section, droite = bord gauche -> bord droit (hauteurs en unités monde)
  const double hs = params.hScale;
  double fx = (B.lx + B.rx - A.lx - A.rx) / 2, fz = (B.lz + B.rz - A.lz - A.rz) / 2;
  double fy = ((B.ly + B.ry - A.ly - A.ry) / 2) * hs;
  double fl = std::sqrt(fx * fx + fy * fy + fz * fz);
  if (fl < 1e-6) { fx = 0; fy = 0; fz = 1; fl = 1; }   // section verticale (saut) : garder un repère plat
  if (std::hypot(fx, fz) < 1) { fy = 0; fl = std::hypot(fx, fz) > 1e-6 ? std::hypot(fx, fz) : 1; }
  o.fwd = {fx / fl, fy / fl, fz / fl};
  double ry = (hr - hl) * hs, rl = std::sqrt((rx - lx) * (rx - lx) + ry * ry + (rz - lz) * (rz - lz));
  o.right = {(rx - lx) / rl, ry / rl, (rz - lz) / rl};
  // hauteur : celle que le jeu calcule pour l'adversaire, au sol comme en vol ($10B50 gauche,
  // $10B52 droite, $10B54 milieu ; même échelle que la route) — trajectoire en cloche pendant les sauts
  double y = int16_t(m.w(0x10b54));
  if (std::fabs(y - surf) > 20000) y = surf;   // valeur absente : sur la route
  {   // valeur figée alors que l'adversaire roule (non calculée par le jeu) : sur la route
    const bool moved = std::hypot(o.x - oppPx_, o.z - oppPz_) > 1;
    oppStale_ = oppHave_ && moved && y == oppRaw_ ? oppStale_ + 1 : 0;
    oppRaw_ = y;
    if (oppStale_ > 8 && std::fabs(y - surf) > 100) y = surf;
  }
  oppImpact_ = 0;
  const double air = y - surf;
  if (!oppHave_ || std::fabs(y - oppY_) > 3000) { oppVy_ = 0; oppAir_ = air; oppHave_ = true; oppPx_ = o.x; oppPz_ = o.z; }
  else {
    const double vy = y - oppY_;
    // atterrissage : en l'air au dernier état, sur la route maintenant, en descendant vite
    if (oppAir_ > 40 && air < 40 && oppVy_ < -30) oppImpact_ = std::clamp(int(-oppVy_ / 5), 4, 50);
    // en vol : la voiture suit sa trajectoire (nez vers le haut à la montée, vers le bas à la descente)
    const double step = std::hypot(o.x - oppPx_, o.z - oppPz_);
    if (air > 40 && step > 1) {
      const double hx = o.fwd.x, hz = o.fwd.z, hl = std::hypot(hx, hz);
      if (hl > 1e-6) {
        const double slope = std::clamp(vy * hs / step, -1.5, 1.5);
        const double l = std::sqrt(1 + slope * slope);
        o.fwd = {hx / hl / l, slope / l, hz / hl / l};
      }
    }
    oppVy_ = vy; oppAir_ = air; oppPx_ = o.x; oppPz_ = o.z;
  }
  oppY_ = y;
  o.yRaw = oppY_;
  o.valid = true;
  return o;
}

// flammes du boost : comme vu du cockpit, le moteur V8 sur le capot crache 8 petites flammes
// (4 échappements de chaque côté) qui montent et partent vers l'arrière ; repère et échelle de addCar
void HdView::addFlames(const OppPose &o, double hs, int seed) {
  const Vec3 r = o.right, f = o.fwd;
  Vec3 u{f.y * r.z - f.z * r.y, f.z * r.x - f.x * r.z, f.x * r.y - f.y * r.x};
  if (u.y < 0) u = {-u.x, -u.y, -u.z};
  const double base = o.yRaw * hs, K = 0.55;
  auto W = [&](double x, double y, double z) {
    x *= K; y *= K; z *= K;
    return Vec3{o.x + r.x * x + u.x * y + f.x * z, base + r.y * x + u.y * y + f.y * z, o.z + r.z * x + u.z * y + f.z * z};
  };
  uint32_t h = (flameTick_++ + uint32_t(seed) * 7919u) * 2654435761u;
  for (int side = -1; side <= 1; side += 2)
    for (int k = 0; k < 4; k++) {
      h = h * 1103515245u + 12345u;
      // échappements le long du moteur, sortant du toit de la cabine (pente : y 45 à l'avant z 150,
      // 99 à l'arrière z -95, demi-largeur 37) ; pied légèrement enfoncé, caché par la carrosserie
      const double z0 = 128 - k * 22, x0 = side * 24, y0 = 45 + 54 * (150 - z0) / 245 - 3;
      double up = 34 + double((h >> 16) % 26), back = 14 + double((h >> 10) % 12), w = 6 + double((h >> 8) % 3);
      // jet en dégradé : jaune à la sortie de l'échappement, orange au milieu, rouge à la pointe
      // (tranches successives d'une pyramide qui s'affine vers le haut, l'arrière et l'extérieur)
      auto grad = [](double s, double flick) {
        static const double C[3][3] = {{255, 230, 70}, {255, 140, 25}, {225, 35, 15}};   // jaune, orange, rouge
        double t = std::min(std::max(s, 0.0), 1.0) * 2;
        int i = t >= 1 ? 1 : 0; double k = t - i;
        uint32_t col = 0xff000000u;
        for (int c = 0; c < 3; c++) {
          double v = (C[i][c] + (C[i + 1][c] - C[i][c]) * k) * flick;
          col |= uint32_t(std::min(255.0, std::max(0.0, v))) << (16 - 8 * c);
        }
        return col;
      };
      const double flick = 0.88 + 0.12 * double((h >> 3) % 8) / 7;   // vacillement de la luminosité
      const double tx = side * up * 0.8, ty = up, tz = -back;           // pointe : inclinée vers l'extérieur (en V)
      const int bands = 8;
      auto ring = [&](double s, Vec3 out[4]) {
        double wd = w * (1 - s), cx = x0 + tx * s, cy = y0 + ty * s, cz = z0 + tz * s;
        out[0] = W(cx - wd, cy, cz - wd); out[1] = W(cx + wd, cy, cz - wd);
        out[2] = W(cx + wd, cy, cz + wd); out[3] = W(cx - wd, cy, cz + wd);
      };
      Vec3 lo[4], hi[4];
      ring(0, lo);
      for (int bnd = 0; bnd < bands; bnd++) {
        double s0 = double(bnd) / bands, s1 = double(bnd + 1) / bands;
        uint32_t col = grad((s0 + s1) / 2, flick);
        if (bnd == bands - 1) {   // dernière tranche : pointe
          Vec3 tip = W(x0 + tx, y0 + ty, z0 + tz);
          for (int e = 0; e < 4; e++) { Vec3 t[3] = {lo[e], lo[(e + 1) % 4], tip}; r3d_.poly(t, 3, col); }
        } else {
          ring(s1, hi);
          for (int e = 0; e < 4; e++) { Vec3 q[4] = {lo[e], lo[(e + 1) % 4], hi[(e + 1) % 4], hi[e]}; r3d_.poly(q, 4, col); }
          std::copy(hi, hi + 4, lo);
        }
      }
    }
}

OppPose HdView::ghostFromPose(const Pose &p) const {
  // même convention que la caméra : avant horizontal (sin lacet, cos lacet), droite (cos lacet, -sin lacet)
  OppPose o;
  double sy = std::sin(p.yaw), cy = std::cos(p.yaw);
  double pitch = params.pitchSign * p.pitch, roll = params.rollSign * p.roll;
  double cp = std::cos(pitch), sp = std::sin(pitch), cr = std::cos(roll), sr = std::sin(roll);
  o.fwd = {sy * cp, sp, cy * cp};
  o.right = {cy * cr, -sr, -sy * cr};
  o.x = p.x; o.z = p.z;
  o.yRaw = p.y * 2;   // la caméra utilise p.y × hScale / 0,5 : hauteur brute = 2 × p.y
  o.valid = true;
  return o;
}

OppPose HdView::opponentAt(double t) const {
  if (snaps_.empty()) return {};
  if (t <= snaps_.front().t) return snaps_.front().opp;
  for (size_t i = 0; i + 1 < snaps_.size(); i++) {
    const OppPose &a = snaps_[i].opp, &b = snaps_[i + 1].opp;
    // au-delà du dernier état : trajectoire prolongée comme celle de la voiture du joueur (poseAt),
    // sinon l'adversaire s'arrête un instant pendant que la caméra avance (tremblement avant-arrière)
    const bool last = i + 2 == snaps_.size();
    if (t >= snaps_[i + 1].t && !last) continue;
    if (!a.valid || !b.valid) return b.valid ? b : a;
    double k = std::min((t - snaps_[i].t) / (snaps_[i + 1].t - snaps_[i].t), 1.6);
    if (std::fabs(b.x - a.x) + std::fabs(b.z - a.z) > 3000) return k < 0.5 ? a : b;
    auto lv = [&](const Vec3 &p, const Vec3 &q) {
      Vec3 v{p.x + (q.x - p.x) * k, p.y + (q.y - p.y) * k, p.z + (q.z - p.z) * k};
      double l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
      return l > 1e-9 ? Vec3{v.x / l, v.y / l, v.z / l} : q;
    };
    OppPose o;
    o.valid = true;
    o.x = a.x + (b.x - a.x) * k; o.z = a.z + (b.z - a.z) * k; o.yRaw = a.yRaw + (b.yRaw - a.yRaw) * k;
    o.right = lv(a.right, b.right); o.fwd = lv(a.fwd, b.fwd);
    return o;
  }
  return snaps_.back().opp;
}

// voiture adverse en polygones : châssis, nez, arceau, moteur et 4 roues (couleurs de la palette du jeu)
void HdView::addCar(const OppPose &o, double hs, const Machine &m, bool closed) {
  const Vec3 r = o.right, f = o.fwd;
  Vec3 u{f.y * r.z - f.z * r.y, f.z * r.x - f.x * r.z, f.x * r.y - f.y * r.x};   // haut = avant × droite
  if (u.y < 0) u = {-u.x, -u.y, -u.z};
  const double base = o.yRaw * hs;
  const double K = 0.55;   // échelle calée sur la voiture d'origine (≈ 22 pixels de large à 1450 unités)
  auto W = [&](double x, double y, double z) {   // repère voiture -> monde
    x *= K; y *= K; z *= K;
    return Vec3{o.x + r.x * x + u.x * y + f.x * z, base + r.y * x + u.y * y + f.y * z, o.z + r.z * x + u.z * y + f.z * z};
  };
  auto pal = [&](int i) { return m.paletteARGB(i); };
  const uint32_t red = pal(10), redLight = pal(11), pink = pal(12), black = pal(0), grey = pal(14), dark = pal(9);
  // boîte : 6 faces, couleurs dessus / côtés / avant-arrière
  auto box = [&](double x0, double x1, double y0, double y1, double z0, double z1, uint32_t top, uint32_t side, uint32_t end) {
    Vec3 c[8] = {W(x0, y0, z0), W(x1, y0, z0), W(x1, y0, z1), W(x0, y0, z1), W(x0, y1, z0), W(x1, y1, z0), W(x1, y1, z1), W(x0, y1, z1)};
    Vec3 q[6][4] = {{c[4], c[5], c[6], c[7]}, {c[0], c[3], c[7], c[4]}, {c[1], c[5], c[6], c[2]},
                    {c[0], c[4], c[5], c[1]}, {c[3], c[2], c[6], c[7]}, {c[0], c[1], c[2], c[3]}};
    uint32_t col[6] = {top, side, side, end, end, black};
    for (int k = 0; k < 6; k++) r3d_.poly(q[k], 4, col[k]);
  };
  const bool hd = params.highDetail;
  // roue : prisme d'axe latéral (octogonal comme l'original, rond et texturé en détails élevés)
  auto wheel = [&](double xc, double zc, double rad, double wid) {
    const int N = hd ? 28 : 8;
    Vec3 a[32], b[32];
    for (int k = 0; k < N; k++) {
      double ang = (k + 0.5) * 2 * 3.14159265 / N, yy = rad + std::sin(ang) * rad, zz = zc + std::cos(ang) * rad;
      a[k] = W(xc - wid / 2, yy, zz); b[k] = W(xc + wid / 2, yy, zz);
    }
    // rotation : angle = distance parcourue / rayon ; le haut de la roue avance
    // oppRoll_ : angle affiché d'une roue arrière (rayon 25) ; les roues avant, plus petites, tournent plus vite
    const double phi = oppRoll_ * 25 / rad, cp = std::cos(phi), sp = std::sin(phi);
    TexInfo tire; tire.kind = TEX_TIRE; tire.o = W(xc, rad, zc); tire.r = rad * K;
    tire.u = {f.x * cp - u.x * sp, f.y * cp - u.y * sp, f.z * cp - u.z * sp};
    tire.v = {f.x * sp + u.x * cp, f.y * sp + u.y * cp, f.z * sp + u.z * cp};
    // flou de mouvement : angle parcouru pendant l'image (le motif est moyenné sur cet angle)
    tire.blur = 0.5 * oppRollStep_ * 25 / rad; tire.contrast = oppRollContrast_;   // obturateur à 180°
    for (int k = 0; k < N; k++) {
      Vec3 q[4] = {a[k], a[(k + 1) % N], b[(k + 1) % N], b[k]};
      if (hd) r3d_.polyTex(q, 4, black, tire); else r3d_.poly(q, 4, black);
    }
    if (hd) {   // flancs : pneu, jante et moyeu
      TexInfo rim = tire; rim.kind = TEX_RIM;
      rim.o = W(xc - wid / 2, rad, zc); r3d_.polyTex(a, N, black, rim);
      rim.o = W(xc + wid / 2, rad, zc); r3d_.polyTex(b, N, black, rim);
    } else {
      r3d_.poly(a, N, black);   // roues noires, comme l'original
      r3d_.poly(b, N, black);
    }
  };
  // face peinte (détails élevés) : repère local o + s·ax + w·ay, étendue [s0,s1]×[w0,w1] en unités voiture
  auto paint = [&](const Vec3 *q, int n, uint32_t col, Vec3 ax, Vec3 ay, double s0, double s1, double w0, double w1) {
    if (!hd) { r3d_.poly(q, n, col); return; }
    TexInfo t; t.kind = TEX_PAINT; t.o = W(0, 0, 0); t.u = ax; t.v = ay;
    t.s0 = s0 * K; t.s1 = s1 * K; t.w0 = w0 * K; t.w1 = w1 * K;
    r3d_.polyTex(q, n, col, t);
  };
  // proportions mesurées sur la voiture d'origine vue de derrière : roues carrées aussi larges que
  // hautes, cabine aussi large que deux roues et deux fois plus haute qu'elles, dont le toit (rose)
  // descend vers l'avant
  wheel(-69, -85, 25, 50); wheel(69, -85, 25, 50);
  wheel(-66, 105, 23, 44); wheel(66, 105, 23, 44);
  box(-44, 44, 12, 28, -110, 140, black, black, black);        // châssis sous la cabine
  {   // cabine en coin : arrière haut en trapèze (plus étroit en haut et incliné vers l'avant), avant bas
    const double x0 = -44, x1 = 44, xt = 37, yb = 28, zr = -120, zrt = -95, zf = 150, yr = 99, yf = 45;
    Vec3 rb0 = W(x0, yb, zr), rb1 = W(x1, yb, zr), rt0 = W(-xt, yr, zrt), rt1 = W(xt, yr, zrt);
    Vec3 fb0 = W(x0, yb, zf), fb1 = W(x1, yb, zf), ft0 = W(-xt, yf, zf), ft1 = W(xt, yf, zf);
    Vec3 rear[4] = {rb0, rb1, rt1, rt0}, front[4] = {fb0, ft0, ft1, fb1}, top[4] = {rt0, rt1, ft1, ft0};
    Vec3 left[4] = {rb0, rt0, ft0, fb0}, right[4] = {rb1, fb1, ft1, rt1}, bottom[4] = {rb0, fb0, fb1, rb1};
    paint(rear, 4, red, r, u, x0, x1, yb, yr); r3d_.poly(front, 4, red); paint(top, 4, pink, r, f, x0, x1, zrt, zf);
    // comme l'original : seul le flanc droit (rose) est dessiné, jamais le gauche
    // (voitures des autres joueurs : carrosserie fermée, on ne voit pas les flammes à travers)
    paint(right, 4, pink, f, u, zr, zf, yb, yr); r3d_.poly(bottom, 4, black);
    if (closed) paint(left, 4, pink, f, u, zr, zf, yb, yr);
  }
  (void)dark; (void)grey;
}

// ------------------------------------------------------------------ décor autour du circuit
void HdView::drawScenery(const Camera &c, int track, double hs) {
  auto it = scenery_.find(track & 7);
  if (it == scenery_.end()) it = scenery_.emplace(track & 7, buildScenery(tracks_[track & 7])).first;
  const double gy = GROUND_RAW * hs;
  const double fwx = std::sin(c.yaw), fwz = std::cos(c.yaw);
  const double Lx = 0.5, Ly = 0.7, Lz = 0.55, Ll = std::sqrt(Lx * Lx + Ly * Ly + Lz * Lz);   // lumière (côté du soleil)
  auto lit = [&](uint32_t col, const Vec3 &nrm) {
    const double nl = std::sqrt(nrm.x * nrm.x + nrm.y * nrm.y + nrm.z * nrm.z);
    const double d = nl > 1e-9 ? (nrm.x * Lx + nrm.y * Ly + nrm.z * Lz) / (nl * Ll) : 0;
    return shadeARGB(col, 0.58 + 0.5 * std::max(0.0, d));
  };
  auto normal = [](const Vec3 *p, int n) {
    Vec3 r{0, 0, 0};
    for (int i = 0; i < n; i++) {
      const Vec3 &a = p[i], &b = p[(i + 1) % n];
      r.x += (a.y - b.y) * (a.z + b.z); r.y += (a.z - b.z) * (a.x + b.x); r.z += (a.x - b.x) * (a.y + b.y);
    }
    return r;
  };
  // face vue de devant seulement (sommets dans le sens qui donne une normale vers l'extérieur)
  auto face = [&](const Vec3 *p, int n, uint32_t col, const TexInfo *tex = nullptr) {
    Vec3 nr = normal(p, n);
    if (nr.x * (c.pos.x - p[0].x) + nr.y * (c.pos.y - p[0].y) + nr.z * (c.pos.z - p[0].z) <= 0) return;
    if (tex) r3d_.polyTex(p, n, lit(col, nr), *tex);
    else r3d_.poly(p, n, lit(col, nr));
  };
  // pavé orienté : centre du bas (x, z), axes horizontaux a (longueur) et b (profondeur), hauteurs y0..y1
  auto box = [&](double x, double z, double ax, double az, double hl, double hd, double y0, double y1, uint32_t col) {
    const double bx = -az, bz = ax;
    Vec3 q[8];
    for (int i = 0; i < 8; i++) {
      const double sa = (i & 1) ? hl : -hl, sb = (i & 2) ? hd : -hd;
      q[i] = {x + ax * sa + bx * sb, (i & 4) ? y1 : y0, z + az * sa + bz * sb};
    }
    const int F[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    for (auto &fc : F) { Vec3 p[4] = {q[fc[0]], q[fc[1]], q[fc[2]], q[fc[3]]}; face(p, 4, col); }
  };
  for (const SceneObj &o : it->second) {
    const double dx = o.x - c.pos.x, dz = o.z - c.pos.z, dist = std::hypot(dx, dz);
    const double rad = o.kind == SCENE_STAND ? 1100 : o.kind == SCENE_BOARD ? 400 : 260 * o.size;
    if (dist > 42000) continue;
    const double along = dx * fwx + dz * fwz, side = std::fabs(dx * fwz - dz * fwx);
    if (along < -rad || side > along * 1.9 + rad * 1.5) continue;   // derrière la caméra ou hors du champ
    const uint32_t sd = o.seed;
    if (o.kind == SCENE_PINE || o.kind == SCENE_TREE) {
      const double k = o.size;
      static const uint32_t GREENS[4] = {0xff2f6a2cu, 0xff3a7a30u, 0xff2a5c34u, 0xff4a8236u};
      const uint32_t leaf = GREENS[sd % 4], trunk = 0xff5a3e26u;
      const bool farLod = dist > 15000;
      if (dist < 9000 && params.shadows) {   // ombre au pied
        Vec3 sh[8];
        for (int i = 0; i < 8; i++) { double a = i * 0.785398; sh[i] = {o.x + std::cos(a) * 125 * k + 40 * k, gy + 1.5, o.z + std::sin(a) * 125 * k + 30 * k}; }
        r3d_.polyAlpha(sh, 8, 0xff000000u, 0.28f);
      }
      if (!farLod) box(o.x, o.z, 1, 0, 9 * k, 9 * k, gy, gy + (o.kind == SCENE_PINE ? 70 : 115) * k, trunk);
      const int N = farLod ? 5 : 7;
      auto ring = [&](double y, double r, Vec3 *v, double rot) {
        for (int i = 0; i < N; i++) { double a = rot + i * 6.2831853 / N; v[i] = {o.x + std::cos(a) * r, y, o.z + std::sin(a) * r}; }
      };
      const double rot = (sd >> 8) % 100 * 0.06;
      if (o.kind == SCENE_PINE) {   // sapin : cônes empilés
        const double L[3][3] = {{55, 125, 160}, {150, 98, 145}, {240, 68, 140}};
        for (int l = farLod ? 1 : 0; l < 3; l++) {
          Vec3 v[8]; ring(gy + L[l][0] * k, L[l][1] * k * (farLod ? 1.25 : 1), v, rot + l * 0.4);
          const Vec3 top{o.x, gy + (L[l][0] + L[l][2]) * k, o.z};
          for (int i = 0; i < N; i++) { Vec3 t[3] = {v[(i + 1) % N], v[i], top}; face(t, 3, shadeARGB(leaf, 0.92 + 0.06 * l)); }
          if (!farLod) { Vec3 b[8]; for (int i = 0; i < N; i++) b[i] = v[i]; face(b, N, shadeARGB(leaf, 0.7)); }
        }
      } else {   // feuillu : boule à facettes
        Vec3 r0[8], r1[8], r2[8];
        ring(gy + 135 * k, 85 * k, r0, rot); ring(gy + 215 * k, 135 * k, r1, rot + 0.3); ring(gy + 305 * k, 115 * k, r2, rot);
        const Vec3 bot{o.x, gy + 105 * k, o.z}, top{o.x, gy + 375 * k, o.z};
        const uint32_t lf = shadeARGB(leaf, 1.08);
        for (int i = 0; i < N; i++) {
          const int j = (i + 1) % N;
          Vec3 a[3] = {r0[i], r0[j], bot}; face(a, 3, lf);
          Vec3 b[4] = {r0[j], r0[i], r1[i], r1[j]}; face(b, 4, lf);
          Vec3 cq[4] = {r1[j], r1[i], r2[i], r2[j]}; face(cq, 4, lf);
          Vec3 d[3] = {r2[j], r2[i], top}; face(d, 3, lf);
        }
      }
    } else if (o.kind == SCENE_BOARD) {   // panneau publicitaire sur deux poteaux, face au circuit
      static const uint32_t COLS[6] = {0xffc82020u, 0xff1f4fb8u, 0xff1d8a3au, 0xfff0b818u, 0xff202020u, 0xffe0601cu};
      const double ax = o.fz, az = -o.fx;   // le long du panneau
      const double L = o.len, y0 = gy + 85, y1 = y0 + 150;
      for (int sgn = -1; sgn <= 1; sgn += 2)
        box(o.x + ax * sgn * L * 0.36, o.z + az * sgn * L * 0.36, ax, az, 7, 7, gy, y0 + 20, 0xff8a8a8au);
      const double hx = ax * L / 2, hz = az * L / 2, bx = -o.fx * 10, bz = -o.fz * 10;
      Vec3 fr[4] = {{o.x + hx, y0, o.z + hz}, {o.x - hx, y0, o.z - hz}, {o.x - hx, y1, o.z - hz}, {o.x + hx, y1, o.z + hz}};
      TexInfo t; t.kind = TEX_BOARD; t.o = {o.x + hx, y0, o.z + hz}; t.u = {-ax, 0, -az}; t.v = {0, 1, 0};
      t.s0 = 0; t.s1 = L; t.w0 = 0; t.w1 = 150; t.r = double(sd % 1000);
      Vec3 nr = normal(fr, 4);
      if (nr.x * (c.pos.x - fr[0].x) + nr.z * (c.pos.z - fr[0].z) > 0) r3d_.polyTex(fr, 4, COLS[sd % 6], t);
      else {   // dos et tranche
        Vec3 bk[4] = {{o.x - hx + bx, y0, o.z - hz + bz}, {o.x + hx + bx, y0, o.z + hz + bz}, {o.x + hx + bx, y1, o.z + hz + bz}, {o.x - hx + bx, y1, o.z - hz + bz}};
        face(bk, 4, 0xff9a9a9au);
      }
      Vec3 tp[4] = {{o.x + hx, y1, o.z + hz}, {o.x - hx, y1, o.z - hz}, {o.x - hx + bx, y1, o.z - hz + bz}, {o.x + hx + bx, y1, o.z + hz + bz}};
      face(tp, 4, 0xffbdbdbdu);
    } else {   // tribune : gradins garnis de public, murs, toit sur poteaux
      static const uint32_t ROOF[4] = {0xffb82828u, 0xff2a4ea8u, 0xffe8e8e8u, 0xff2f7a3au};
      const double ax = o.fz, az = -o.fx;   // le long de la tribune
      const double fx = o.fx, fz = o.fz, L = o.len, hl = L / 2;
      const int R = 7;
      const double D = 95, RISE = 55, base = gy + 40;
      auto P = [&](double a, double depth, double y) { return Vec3{o.x + ax * a - fx * depth, y, o.z + az * a - fz * depth}; };
      const uint32_t concrete = 0xffa6a39cu, dark = 0xff74716cu;
      { Vec3 q[4] = {P(-hl, 0, gy), P(hl, 0, gy), P(hl, 0, base), P(-hl, 0, base)}; face(q, 4, dark); }   // muret avant
      for (int r = 0; r < R; r++) {
        const double y = base + r * RISE, d0 = r * D, d1 = d0 + D;
        { Vec3 q[4] = {P(-hl, d0, y), P(-hl, d1, y), P(hl, d1, y), P(hl, d0, y)}; face(q, 4, concrete); }   // marche
        if (r + 1 < R) { Vec3 q[4] = {P(-hl, d1, y), P(hl, d1, y), P(hl, d1, y + RISE), P(-hl, d1, y + RISE)}; face(q, 4, dark); }
        if (dist < 26000) {   // rangée de spectateurs
          Vec3 q[4] = {P(-hl + 10, d0 + 40, y), P(hl - 10, d0 + 40, y), P(hl - 10, d0 + 40, y + 58), P(-hl + 10, d0 + 40, y + 58)};
          TexInfo t; t.kind = TEX_CROWD; t.o = P(-hl + 10, d0 + 40, y); t.u = {ax, 0, az}; t.v = {0, 1, 0};
          t.r = double(r * 131 + sd % 997);
          Vec3 nr = normal(q, 4);
          if (nr.x * (c.pos.x - q[0].x) + nr.y * (c.pos.y - q[0].y) + nr.z * (c.pos.z - q[0].z) > 0) r3d_.polyTex(q, 4, 0xff8a7c74u, t);
        }
      }
      const double dB = R * D, yTop = base + (R - 1) * RISE, yRoof = yTop + 190;
      { Vec3 q[4] = {P(hl, dB, gy), P(-hl, dB, gy), P(-hl, dB, yRoof), P(hl, dB, yRoof)}; face(q, 4, concrete); }   // mur du fond
      for (int sgn = -1; sgn <= 1; sgn += 2) {   // flancs
        Vec3 q[5] = {P(sgn * hl, 0, gy), P(sgn * hl, dB, gy), P(sgn * hl, dB, yRoof), P(sgn * hl, dB * 0.15, yTop + 40), P(sgn * hl, 0, base)};
        if (sgn > 0) face(q, 5, concrete);
        else { Vec3 w[5] = {q[4], q[3], q[2], q[1], q[0]}; face(w, 5, concrete); }
      }
      const uint32_t roof = ROOF[sd % 4];
      const double yF = yRoof - 25;   // toit légèrement incliné vers l'avant, dépassant au-dessus des premiers rangs
      { Vec3 q[4] = {P(-hl - 30, -60, yF), P(hl + 30, -60, yF), P(hl + 30, dB + 20, yRoof), P(-hl - 30, dB + 20, yRoof)}; face(q, 4, roof); }
      { Vec3 q[4] = {P(-hl - 30, -60, yF - 14), P(-hl - 30, dB + 20, yRoof - 14), P(hl + 30, dB + 20, yRoof - 14), P(hl + 30, -60, yF - 14)}; face(q, 4, shadeARGB(roof, 0.7)); }
      { Vec3 q[4] = {P(-hl - 30, -60, yF - 14), P(hl + 30, -60, yF - 14), P(hl + 30, -60, yF), P(-hl - 30, -60, yF)}; face(q, 4, 0xffe8e8e8u); }   // bandeau
      for (int i = 0; i <= 3; i++) {   // poteaux avant
        const double a = -hl + 20 + i * (L - 40) / 3;
        Vec3 pp = P(a, -40, 0);
        box(pp.x, pp.z, ax, az, 8, 8, gy, yF - 14, 0xff8e8e8eu);
      }
    }
  }
}

void HdView::renderScene(const Pose &p, int track, uint32_t *out, int W, int H, double focal, double cx, double cy,
                         const Machine &m, const OppPose *opp) {
  const HdParams &P = params;
  // couleurs : palette courante du jeu
  auto pal = [&](int i) { return m.paletteARGB(i); };
  uint32_t sky = pal(7), ground = pal(13), hill = pal(5), hillFar = shadeARGB(pal(5), 1.25), road = pal(1),
           road2 = pal(2), line = pal(3), line2 = pal(9), wall = pal(10), wall2 = pal(15);

  // caméra : position de la voiture + œil, dans le repère de la voiture
  Camera c;
  double pitch = P.pitchSign * p.pitch, roll = P.rollSign * p.roll;
  double sy = std::sin(p.yaw), cyw = std::cos(p.yaw);
  const double HSCALE = P.hScale;
  c.pos = {p.x + sy * P.eyeFwd + cyw * P.eyeSide, p.y * HSCALE / 0.5 + P.eyeUp, p.z + cyw * P.eyeFwd - sy * P.eyeSide};
  // suspension très comprimée (réception de saut) : l'œil ne doit jamais passer sous la route
  {
    double yRaw = 0;
    if (surfaceRaw(tracks_[track & 7], c.pos.x, c.pos.z, p.y * 2, yRaw))
      if (c.pos.y < yRaw * HSCALE + P.minClearance) { eyeLift_ = std::max(eyeLift_, yRaw * HSCALE + P.minClearance - c.pos.y); clampCount++; }
  }
  c.pos.y += eyeLift_;
  eyeLift_ *= 0.85;   // la remontée de l'œil retombe progressivement (pas de saut de caméra)
  c.yaw = p.yaw + P.yawOffset; c.pitch = pitch + P.pitchOffset; c.roll = roll;
  c.focal = focal; c.focalY = focal * P.aspect; c.cx = cx; c.cy = cy; c.nearZ = 2;
  r3d_.begin(W, H, c, sky);
  r3d_.setSky(P.sky, sky, frame_ / 50.0);

  // sol et collines (au plus loin, dans l'ordre)
  const double gy = GROUND_RAW * HSCALE, R = 2e6;
  Vec3 g[4] = {{c.pos.x - R, gy, c.pos.z - R}, {c.pos.x + R, gy, c.pos.z - R}, {c.pos.x + R, gy, c.pos.z + R}, {c.pos.x - R, gy, c.pos.z + R}};
  if (P.highDetail) { TexInfo tg; tg.kind = TEX_GROUND; r3d_.polyFarTex(g, 4, ground, 1e-12f, tg, gy); }
  else r3d_.polyFar(g, 4, ground, 1e-12f);
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

  if (P.scenery) drawScenery(c, track, HSCALE);   // tribunes, panneaux, arbres

  // circuit
  const Track &tr = tracks_[track & 7];
  const size_t n = tr.secs.size();
  const double lw = 0.0175;   // largeur des lignes de bord (fraction de la largeur de route)
  // ligne de départ : l'original la trace à la dernière frontière de section de la pièce de départ
  // (drapeau $6EA7C posé en $52568, recopié dans la liste de polygones en $53DCE)
  size_t startSec = 0;
  while (startSec + 1 < n && tr.secs[startSec + 1].piece == tr.startPiece) startSec++;
  for (size_t i = 0; i < n; i++) {
    const Section &A = tr.secs[i], &B = tr.secs[(i + 1) % n];
    uint32_t lc = (i & 1) ? line : line2;   // lignes de bord : jaune et rouge sombre, une section sur deux (comme l'original)
    Vec3 aL{A.lx, A.ly * HSCALE, A.lz}, aR{A.rx, A.ry * HSCALE, A.rz}, bL{B.lx, B.ly * HSCALE, B.lz}, bR{B.rx, B.ry * HSCALE, B.rz};
    // flancs jusqu'au sol, rouges et blancs en alternance par pièce
    uint32_t wc = (A.piece & 1) ? wall2 : wall;
    auto wallQuad = [&](const Vec3 &p0, const Vec3 &p1) {
      if (p0.y <= gy + 0.5 && p1.y <= gy + 0.5) return;
      Vec3 q[4] = {p0, p1, {p1.x, gy, p1.z}, {p0.x, gy, p0.z}};
      if (P.highDetail) { TexInfo tw; tw.kind = TEX_WALL; r3d_.polyTex(q, 4, wc, tw); }
      else r3d_.poly(q, 4, wc);
    };
    wallQuad(aL, bL);
    wallQuad(bR, aR);
    // dessus de la route (pas pour les sections verticales des sauts), couleur alternée par pièce
    double horiz = std::hypot((B.lx + B.rx - A.lx - A.rx) * 0.5, (B.lz + B.rz - A.lz - A.rz) * 0.5);
    if (horiz < 1) continue;
    // pan d'un saut (marche, face de la rampe, pont levé) : pente de plus de 50° -> noir, comme le
    // rendu d'origine (les pentes où l'on roule restent sous 30° : Roller Coaster, Ski Jump)
    if (std::fabs((B.ly + B.ry - A.ly - A.ry) * 0.5) * HSCALE > 1.2 * horiz) {
      Vec3 f1[3] = {aL, aR, bR}, f2[3] = {aL, bR, bL};
      r3d_.poly(f1, 3, pal(0));
      r3d_.poly(f2, 3, pal(0));
      continue;
    }
    uint32_t rc = (A.piece & 1) ? road2 : road;
    auto lerp = [](const Vec3 &a, const Vec3 &b, double t) { return Vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; };
    Vec3 aL1 = lerp(aL, aR, lw), aR1 = lerp(aR, aL, lw), bL1 = lerp(bL, bR, lw), bR1 = lerp(bR, bL, lw);
    Vec3 mid[4] = {aL1, aR1, bR1, bL1};
    // en triangles : sur un virage relevé, les 4 coins d'une section ne sont pas coplanaires
    auto quad = [&](const Vec3 *q, uint32_t col) {
      Vec3 t1[3] = {q[0], q[1], q[2]}, t2[3] = {q[0], q[2], q[3]};
      if (P.highDetail) {
        // repère de la route : u = travers (bord gauche -> droit), pour les traces de pneus
        TexInfo tr; tr.kind = TEX_ROAD;
        double wl = std::sqrt((aR.x - aL.x) * (aR.x - aL.x) + (aR.y - aL.y) * (aR.y - aL.y) + (aR.z - aL.z) * (aR.z - aL.z));
        if (wl > 1e-6) { tr.o = aL; tr.u = {(aR.x - aL.x) / wl, (aR.y - aL.y) / wl, (aR.z - aL.z) / wl}; tr.s0 = 0; tr.s1 = wl; }
        r3d_.polyTex(t1, 3, col, tr);
        r3d_.polyTex(t2, 3, col, tr);
      } else {
        r3d_.poly(t1, 3, col);
        r3d_.poly(t2, 3, col);
      }
    };
    quad(mid, rc);
    Vec3 ql[4] = {aL, aL1, bL1, bL};
    quad(ql, lc);
    Vec3 qr[4] = {aR1, aR, bR, bR1};
    quad(qr, lc);
    if (i == startSec) {   // ligne de départ : trait blanc en travers de la route
      double fx = (bL.x + bR.x - aL.x - aR.x) / 2, fz = (bL.z + bR.z - aL.z - aR.z) / 2, fl = std::hypot(fx, fz);
      double k = std::min(1.0, START_LINE_W / std::max(1.0, fl));
      Vec3 s0 = aL, s1 = aR, s2 = lerp(aR, bR, k), s3 = lerp(aL, bL, k);
      for (Vec3 *v : {&s0, &s1, &s2, &s3}) v->y += 2.5;   // juste au-dessus du revêtement (sans scintillement)
      Vec3 sq[4] = {s0, s1, s2, s3};
      r3d_.poly(sq, 4, pal(15));
    }
  }
  if (opp && opp->valid) {
    if (P.shadows) addShadow(*opp, HSCALE, track);
    addCar(*opp, HSCALE, m);
  }
  for (auto &g : ghosts) {   // autres joueurs (course à plusieurs) et place de leur nom au-dessus de la voiture
    g.onScreen = false;
    if (!g.pose.valid) continue;
    // hauteur envoyée : point de référence de la physique, en général 40 à 220 unités sous la route
    // dessinée ; voiture posée sur la route (sauf en l'air, ou tombée loin sous une route qui la survole)
    {
      const int keepHint = hint_;
      double s;
      if (surfaceRaw(tracks_[track & 7], g.pose.x, g.pose.z, g.pose.yRaw, s) && s > g.pose.yRaw && s - g.pose.yRaw < 260) g.pose.yRaw = s;
      hint_ = keepHint;
    }
    if (P.shadows) addShadow(g.pose, HSCALE, track);
    {   // roues : rotation propre à chaque joueur, selon la distance parcourue depuis l'image précédente
      GhostWheels &gw = ghostWheels_[g.id];
      // choc signalé par ce joueur (numéro de choc changé) : étincelles chez nous aussi
      if (gw.impactStamp < 0) gw.impactStamp = g.impactStamp;
      else if (g.impactStamp != gw.impactStamp) {
        gw.impactStamp = g.impactStamp;
        if (P.particles && g.impact > 0)
          spawnImpact({g.pose.x + g.pose.fwd.x * 50, g.pose.yRaw * HSCALE + 8, g.pose.z + g.pose.fwd.z * 50}, g.impact, g.pose.fwd);
      }
      double step = 0;
      if (gw.prev.valid) {
        double dx = g.pose.x - gw.prev.x, dy = (g.pose.yRaw - gw.prev.yRaw) * HSCALE, dz = g.pose.z - gw.prev.z;
        step = dx * g.pose.fwd.x + dy * g.pose.fwd.y + dz * g.pose.fwd.z;
        if (std::fabs(step) > 400) step = 0;   // saut (grue, nouveau départ)
      }
      gw.prev = g.pose;
      const double CAP = 0.2, dphi = step / (25 * 0.55);
      const double keepRoll = oppRoll_, keepStep = oppRollStep_, keepContrast = oppRollContrast_;
      oppRollStep_ = std::clamp(dphi, -CAP, CAP);
      oppRollContrast_ = std::clamp(1 - (std::fabs(dphi) - CAP) / 4, 0.6, 1.0);
      gw.roll = std::fmod(gw.roll + oppRollStep_, 2 * 3.14159265358979 * 1000);
      oppRoll_ = gw.roll;
      addCar(g.pose, HSCALE, m, true);
      oppRoll_ = keepRoll; oppRollStep_ = keepStep; oppRollContrast_ = keepContrast;
    }
    if (g.boost) addFlames(g.pose, HSCALE, int(&g - ghosts.data()));
    Vec3 top{g.pose.x, g.pose.yRaw * HSCALE + 90, g.pose.z};
    Vec3 q = r3d_.toCam(top);
    g.dist = std::sqrt((g.pose.x - c.pos.x) * (g.pose.x - c.pos.x) + (g.pose.z - c.pos.z) * (g.pose.z - c.pos.z));
    if (q.z > 20) {
      g.sx = c.cx + c.focal * q.x / q.z; g.sy = c.cy - c.focalY * q.y / q.z;
      g.onScreen = g.sx > -50 && g.sx < W + 50 && g.sy > -20 && g.sy < H;
    }
  }
  if (P.particles) drawParticles(c);
  r3d_.setAntialias(P.antialias && W > 400);
  r3d_.finish(out);
}

// ombre d'une voiture : rectangle sombre semi-transparent posé sur la route sous la voiture (chaque
// coin à la hauteur de la route), plus grand et plus pâle quand la voiture est en l'air
void HdView::addShadow(const OppPose &o, double hs, int track) {
  const Track &T = tracks_[track & 7];
  const int keepHint = hint_;
  double s;
  bool onRoad = surfaceRaw(T, o.x, o.z, o.yRaw, s);
  if (!onRoad || s > o.yRaw + 80) s = GROUND_RAW;   // hors de la route : ombre sur le sol
  const double height = o.yRaw - s;                 // hauteur brute au-dessus de la surface
  if (height < -120) { hint_ = keepHint; return; }
  const double up = std::max(0.0, height);
  const float alpha = float(0.55 * std::clamp(1 - up / 1400, 0.2, 1.0));
  const double grow = 1 + up / 2500, K = 0.55;
  double fx = o.fwd.x, fz = o.fwd.z, fl = std::hypot(fx, fz);
  if (fl < 1e-6) { hint_ = keepHint; return; }
  fx /= fl; fz /= fl;
  const double rx = fz, rz = -fx;   // droite de la voiture (à plat)
  const double W2 = 58 * K * grow, Z0 = -128 * K * grow, Z1 = 158 * K * grow;
  const double cxs[4] = {-W2, W2, W2, -W2}, czs[4] = {Z0, Z0, Z1, Z1};
  Vec3 q[4];
  for (int k = 0; k < 4; k++) {
    double x = o.x + rx * cxs[k] + fx * czs[k], z = o.z + rz * cxs[k] + fz * czs[k], y = s;
    double sc;
    if (onRoad && surfaceRaw(T, x, z, s, sc) && std::fabs(sc - s) < 300) y = sc;
    q[k] = {x, y * hs + 2.0, z};
  }
  hint_ = keepHint;
  r3d_.polyAlpha(q, 4, 0xff000000u, alpha);
}

void HdView::impactAt(double x, double yRaw, double z, int strength) {
  if (params.particles) spawnImpact({x, yRaw * params.hScale + 8, z}, strength, {0, 0, 0});
}

// choc : gerbe d'étincelles (vers le haut et dans le sens du choc) et bouffées de fumée
void HdView::spawnImpact(const Vec3 &at, int strength, const Vec3 &dir) {
  auto rnd = [&] { partSeed_ = partSeed_ * 1103515245u + 12345u; return double((partSeed_ >> 8) & 0xffff) / 65535.0; };
  const int sparks = std::clamp(6 + strength * 2, 6, 60), puffs = std::clamp(1 + strength / 6, 1, 6);
  for (int i = 0; i < sparks; i++) {
    double a = rnd() * 6.2831853, sp = 3 + rnd() * (4 + strength * 0.15);
    Vec3 v{std::cos(a) * sp + dir.x * 4, 2 + rnd() * 6, std::sin(a) * sp + dir.z * 4};
    float life = float(12 + rnd() * 18);
    particles_.push_back({{at.x + (rnd() - 0.5) * 30, at.y + rnd() * 6, at.z + (rnd() - 0.5) * 30}, v, life, life, float(0.7 + rnd() * 0.6), 0});
  }
  for (int i = 0; i < puffs; i++) {
    float life = float(45 + rnd() * 40);
    particles_.push_back({{at.x + (rnd() - 0.5) * 40, at.y + rnd() * 10, at.z + (rnd() - 0.5) * 40},
                          {(rnd() - 0.5) * 1.2, 0.6 + rnd() * 0.8, (rnd() - 0.5) * 1.2}, life, life, float(10 + rnd() * 8), 1});
  }
  if (particles_.size() > 600) particles_.erase(particles_.begin(), particles_.begin() + long(particles_.size() - 600));
}

void HdView::stepParticles(double dt) {
  for (auto &q : particles_) {
    q.p.x += q.v.x * dt; q.p.y += q.v.y * dt; q.p.z += q.v.z * dt;
    if (q.kind == 0) { q.v.y -= 0.9 * dt; q.v.x *= std::pow(0.97, dt); q.v.z *= std::pow(0.97, dt); }
    else { q.v.x *= std::pow(0.96, dt); q.v.z *= std::pow(0.96, dt); q.v.y *= std::pow(0.985, dt); q.size += float(0.5 * dt); }
    q.life -= float(dt);
  }
  particles_.erase(std::remove_if(particles_.begin(), particles_.end(), [](const Particle &q) { return q.life <= 0; }), particles_.end());
}

void HdView::drawParticles(const Camera &c) {
  const Vec3 R{std::cos(c.yaw), 0, -std::sin(c.yaw)};
  for (const auto &q : particles_) {
    const float f = q.life / q.maxLife;
    if (q.kind == 0) {   // étincelle : traînée du jaune au rouge
      Vec3 tail{q.p.x - q.v.x * 1.5, q.p.y - q.v.y * 1.5, q.p.z - q.v.z * 1.5};
      double w = q.size;
      Vec3 s[4] = {{q.p.x - R.x * w, q.p.y + w, q.p.z - R.z * w}, {q.p.x + R.x * w, q.p.y - w, q.p.z + R.z * w},
                   {tail.x + R.x * w * 0.4, tail.y - w * 0.4, tail.z + R.z * w * 0.4}, {tail.x - R.x * w * 0.4, tail.y + w * 0.4, tail.z - R.z * w * 0.4}};
      uint32_t g = uint32_t(120 + 135 * f), b = uint32_t(60 * f * f);
      r3d_.poly(s, 4, 0xff000000u | 0xff0000u | (g << 8) | b);
    } else {   // fumée : carré face à la caméra, gris, de plus en plus pâle
      // près de la caméra : de plus en plus transparente (pas de voile sur tout l'écran)
      const double dx = q.p.x - c.pos.x, dy = q.p.y - c.pos.y, dz = q.p.z - c.pos.z, dist = std::sqrt(dx * dx + dy * dy + dz * dz);
      const float near = float(std::clamp((dist - 80) / 220, 0.0, 1.0));
      if (near <= 0) continue;
      double w = q.size;
      Vec3 s[8];   // bouffée : octogone face à la caméra
      for (int i = 0; i < 8; i++) {
        double a = i * 0.785398 + 0.39, cx = std::cos(a) * w, cy = std::sin(a) * w;
        s[i] = {q.p.x + R.x * cx, q.p.y + cy, q.p.z + R.z * cx};
      }
      r3d_.polyAlpha(s, 8, 0xff9a9a9au, 0.45f * f * near);
    }
  }
}

// cockpit fixe remplacé par hd/cockpit.png. Le cockpit du jeu reste la référence : là où il est
// comme au début de la course, on montre l'image HD ; ce qui a changé (fissure des dégâts, textes,
// sprites sans image HD) est dessiné par-dessus. Dans les afficheurs et la barre de vitesse, l'image
// HD sert de fond et les chiffres / la barre du jeu (pixels plus sombres ou plus clairs que le fond)
// sont posés dessus.
void HdView::drawCockpitHd(uint32_t *out, int W, int H, double s, double ox) {
  const HdImage &ck = assets.cockpit;
  if (ckW_ != W || ckH_ != H || ckImg_ != &ck || ckScaled_.size() != size_t(W) * H) {   // image à la taille de l'écran
    ckW_ = W; ckH_ = H; ckImg_ = &ck;
    ckScaled_.assign(size_t(W) * H, 0);
    const double xa = 32, xb = 288, la = ox + xa * s, lb = ox + xb * s;
    std::vector<double> us(static_cast<size_t>(W));
    for (int x = 0; x < W; x++) {
      const double X = x + 0.5;
      us[size_t(x)] = X < la ? (la > 0 ? X / la * xa : 0) : X >= lb ? xb + (X - lb) / std::max(1.0, W - lb) * (320 - xb) : xa + (X - la) / s;
    }
    for (int y = 0; y < H; y++) {
      const double v = std::clamp((y + 0.5) / s / 200.0 * ck.h - 0.5, 0.0, ck.h - 1.0);
      const int v0 = int(v), v1 = std::min(v0 + 1, ck.h - 1);
      const double fv = v - v0;
      for (int x = 0; x < W; x++) {
        const double u = std::clamp(us[size_t(x)] / 320.0 * ck.w - 0.5, 0.0, ck.w - 1.0);
        const int u0 = int(u), u1 = std::min(u0 + 1, ck.w - 1);
        const double fu = u - u0;
        const uint32_t q[4] = {ck.px[size_t(v0) * ck.w + u0], ck.px[size_t(v0) * ck.w + u1], ck.px[size_t(v1) * ck.w + u0], ck.px[size_t(v1) * ck.w + u1]};
        const double wq[4] = {(1 - fu) * (1 - fv), fu * (1 - fv), (1 - fu) * fv, fu * fv};
        double acc[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; k++) {
          const double a = (q[k] >> 24) / 255.0 * wq[k];
          acc[0] += a; acc[1] += ((q[k] >> 16) & 255) * a; acc[2] += ((q[k] >> 8) & 255) * a; acc[3] += (q[k] & 255) * a;
        }
        if (acc[0] <= 0.004) continue;
        ckScaled_[size_t(y) * W + x] = uint32_t(std::min(255.0, acc[0] * 255)) << 24 | uint32_t(acc[1] / acc[0]) << 16 |
                                      uint32_t(acc[2] / acc[0]) << 8 | uint32_t(acc[3] / acc[0]);
      }
    }
  }
  // cockpit de référence : relevé peu après le début de la course
  if (ckRefFrames_ < 12 && ++ckRefFrames_ == 12) ckRef_ = overlay_;
  const bool ref = ckRef_.size() == overlay_.size();
  // afficheurs (2 lignes à gauche, 2 à droite) et barre de vitesse, en pixels de l'écran d'origine
  struct Zone { int x0, y0, x1, y1; uint32_t bg; };
  Zone zones[5] = {{37, 178, 86, 186, 0}, {37, 188, 86, 196, 0}, {234, 178, 283, 186, 0}, {234, 188, 283, 196, 0}, {96, 173, 225, 177, 0}};
  static uint8_t zoneOf[320 * 200];
  std::memset(zoneOf, 0, sizeof zoneOf);
  auto luma = [](uint32_t c) { return int((c >> 16) & 255) * 77 + int((c >> 8) & 255) * 150 + int(c & 255) * 29 >> 8; };
  for (int k = 0; k < 5; k++) {   // fond de la zone : couleur la plus fréquente
    Zone &z = zones[k];
    std::map<uint32_t, int> cnt;
    for (int y = z.y0; y < z.y1; y++)
      for (int x = z.x0; x < z.x1; x++) { cnt[overlay_[size_t(y) * 320 + x]]++; zoneOf[y * 320 + x] = uint8_t(k + 1); }
    int best = -1;
    for (auto &[c, n] : cnt) if (n > best) { best = n; z.bg = c; }
  }
  for (int y = 0; y < H; y++) {
    const int v = mapY_[y];
    const uint32_t *src = &overlay_[size_t(v) * 320];
    const uint32_t *rf = ref ? &ckRef_[size_t(v) * 320] : nullptr;
    const uint32_t *hd = &ckScaled_[size_t(y) * W];
    uint32_t *o = out + size_t(y) * W;
    for (int x = 0; x < W; x++) {
      const int u = mapX_[x];
      if (u < 0) continue;
      const uint32_t c = src[u], h = hd[x];
      const int zk = zoneOf[v * 320 + u];
      bool lowres;
      if (zk) lowres = c && c != zones[zk - 1].bg && std::abs(luma(c) - luma(zones[zk - 1].bg)) > 30;   // chiffres, barre
      else lowres = c && (ref ? c != rf[u] : (h >> 24) == 0);   // changé depuis le début (ou hors de l'image HD)
      if (h >> 24) {
        const uint32_t a = h >> 24, d = o[x];
        if (a >= 255) o[x] = h;
        else {
          auto mix = [&](int sh) { return ((((h >> sh) & 255) * a + ((d >> sh) & 255) * (255 - a)) / 255) << sh; };
          o[x] = 0xff000000u | mix(16) | mix(8) | mix(0);
        }
      }
      if (lowres) o[x] = c;
    }
  }
}

void HdView::render(const Machine &m, double t, uint32_t *out, int W, int H) {
  const double s = H / 200.0, ox = (W - 320 * s) / 2;
  if (!racing()) {
    ckRefFrames_ = 0; ckRef_.clear();   // cockpit HD : nouvelle référence à la prochaine course
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
  wind_ += (windTarget_ - wind_) * 0.08;   // lissé (affichage à 60 i/s)
  Pose p = poseAt(T);
  if (viewSide != 0) { p.x += std::cos(p.yaw) * viewSide; p.z -= std::sin(p.yaw) * viewSide; }   // vue décalée sur le côté
  int track = snaps_.back().track;
  OppPose opp = opponentAt(T);
  {   // particules (étincelles, fumée) : avancées selon le temps écoulé, en trames
    double dt = partT_ < 0 ? 0 : std::clamp(T - partT_, 0.0, 4.0);
    partT_ = T;
    stepParticles(dt);
    for (const Snap &sn : snaps_)   // l'adversaire retombe brutalement : étincelles quand on le voit toucher la route
      if (sn.oppImpact > 0 && sn.t <= T && sn.t > oppImpactT_) {
        oppImpactT_ = sn.t;
        if (params.particles && opp.valid && T - sn.t < 30)
          spawnImpact({opp.x, opp.yRaw * params.hScale + 8, opp.z}, sn.oppImpact, opp.fwd);
      }
    if (ownImpact_ > 0) {   // choc de notre voiture : gerbe devant le capot
      const double sy = std::sin(p.yaw), cy = std::cos(p.yaw);
      if (params.particles)
        spawnImpact({p.x + sy * 120, p.y * params.hScale / 0.5 + 6, p.z + cy * 120}, ownImpact_, {sy, 0, cy});
      ownImpact_ = 0;
    }
  }
  {   // roues de l'adversaire : distance parcourue depuis l'image précédente, dans le sens de la marche
    double step = 0;
    if (opp.valid && oppPrev_.valid) {
      double dx = opp.x - oppPrev_.x, dy = (opp.yRaw - oppPrev_.yRaw) * params.hScale, dz = opp.z - oppPrev_.z;
      step = dx * opp.fwd.x + dy * opp.fwd.y + dz * opp.fwd.z;
      if (std::fabs(step) > 400) step = 0;   // saut (grue, nouveau départ)
    }
    // rotation réelle (distance / rayon) affichée jusqu'à 0,2 rad par image : au-delà, les sculptures
    // défileraient à l'envers (effet stroboscopique) ; la vitesse en plus se traduit en flou
    const double CAP = 0.2, dphi = step / (25 * 0.55);
    oppRollStep_ = std::clamp(dphi, -CAP, CAP);
    oppRollContrast_ = std::clamp(1 - (std::fabs(dphi) - CAP) / 4, 0.6, 1.0);
    oppRoll_ = std::fmod(oppRoll_ + oppRollStep_, 2 * PI * 1000);
    oppPrev_ = opp;
  }
  const_cast<Machine &>(m).hideOpponentPixels(opp.valid);
  renderScene(p, track, out, W, H, params.focal * s, ox + params.cx * s, params.cy * s, m, &opp);
  if (!params.cockpit) return;
  overlay_.resize(320 * 200);
  m.overlayARGB(overlay_.data(), assets.sprites().empty() && !params.builtinFlames ? nullptr : &visible_);
  if (mapW_ != W || mapH_ != H) {
    mapW_ = W; mapH_ = H;
    mapX_.assign(W, -1); mapY_.assign(H, -1);
    // 16/9 : la partie centrale (fenêtre, x 32..287) garde l'échelle ; les montants et les bords
    // du tableau de bord sont étirés jusqu'aux bords de l'image
    const double xa = 32, xb = 288, la = ox + xa * s, lb = ox + xb * s;
    for (int x = 0; x < W; x++) {
      double X = x + 0.5, u;
      if (X < la) u = la > 0 ? X / la * xa : 0;
      else if (X >= lb) u = xb + (X - lb) / std::max(1.0, W - lb) * (320 - xb);
      else u = xa + (X - la) / s;
      mapX_[x] = std::clamp(int(u), 0, 319);
    }
    for (int y = 0; y < H; y++) mapY_[y] = std::min(199, int((y + 0.5) / s));
  }
  if (!assets.cockpit.px.empty()) drawCockpitHd(out, W, H, s, ox);
  else
    for (int y = 0; y < H; y++) {
      const uint32_t *src = &overlay_[mapY_[y] * 320];
      uint32_t *o = out + size_t(y) * W;
      for (int x = 0; x < W; x++) {
        int u = mapX_[x];
        if (u >= 0 && src[u]) o[x] = src[u];
      }
    }
  // sprites remplacés par les images du dossier hd/, dans l'ordre où le jeu les a dessinés
  if (assets.sprites().empty() && !params.builtinFlames) return;
  const double xa = 32, xb = 288, la = ox + xa * s, lb = ox + xb * s;
  auto fx = [&](double u) { return u < xa ? u / xa * la : u >= xb ? lb + (u - xb) / (320 - xb) * (W - lb) : la + (u - xa) * s; };
  // ordre : celui du jeu, sauf priorité donnée par les images (moteur au-dessus des roues, flammes au-dessus du moteur)
  std::stable_sort(visible_.begin(), visible_.end(), [&](const SpriteDraw &a, const SpriteDraw &b) {
    auto lay = [&](int id) {
      const HdSprite *p = assets.sprite(id);
      if (p) return p->layer;
      return std::find(std::begin(FLAME_IDS), std::end(FLAME_IDS), id) != std::end(FLAME_IDS) ? 2 : 0;
    };
    return lay(a.id) < lay(b.id);
  });
  for (const SpriteDraw &e : visible_) {
    bool flameId = std::find(std::begin(FLAME_IDS), std::end(FLAME_IDS), e.id) != std::end(FLAME_IDS);
    // flammes : images du dossier hd/ seulement si demandé (d'anciennes versions y laissaient les leurs)
    const HdSprite *spr = flameId && !params.flameImages ? nullptr : spriteFor(e.id, m);
    if (!spr && flameId && params.builtinFlames) {
      // image d'animation choisie par le jeu (6/7/49 à gauche, 8/9/50 à droite), à sa place exacte
      drawFlame(e.id, fx(e.x), e.y * s, s, m, out, W, H);
      continue;
    }
    if (!spr) continue;
    double x0 = fx(e.x - spr->padL * e.w), x1 = fx(e.x + e.w * (1 + spr->padR)), y0 = (e.y - spr->padT * e.h) * s,
           y1 = (e.y + e.h * (1 + spr->padB)) * s;
    // échelle autour du point d'ancrage, puis décalage (en pixels d'origine)
    double cx = (x0 + x1) / 2, w2 = (x1 - x0) / 2 * spr->scale, h = (y1 - y0) * spr->scale;
    double ay = spr->anchor == 0 ? y0 : spr->anchor == 1 ? y1 : (y0 + y1) / 2;
    double ny0 = spr->anchor == 0 ? ay : spr->anchor == 1 ? ay - h : ay - h / 2;
    // vent : les flammes se couchent vers l'extérieur (l'arrière de la voiture). Flammes calculées :
    // images préparées par niveau de vent ; images du dossier hd/ : cisaillement et aplatissement
    bool flame = std::find(std::begin(FLAME_IDS), std::end(FLAME_IDS), e.id) != std::end(FLAME_IDS);
    double wk = (spr->wind >= 0 ? spr->wind : flame ? 1.0 : 0.0) * wind_;
    double side = e.x + e.w / 2 < 160 ? -1 : 1;
    bool baked = spr->windLevels > 1;
    double squash = baked ? 1 : 1 - 0.3 * std::min(1.0, wk);
    double top = ny0 + h * (1 - squash);
    HdAssets::draw(*spr, T / 50.0 * (1 + 0.8 * wk), cx - w2 + spr->dx * s, top + spr->dy * s, cx + w2 + spr->dx * s,
                   ny0 + h + spr->dy * s, out, W, H, baked ? 0 : side * 0.55 * wk, wk);
  }
}

}  // namespace scr
