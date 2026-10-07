// hdview.cpp — voir hdview.hpp
#include "hdview.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

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

HdView::HdView(const Machine &m) : tracks_(decodeTracks(m.image())) {
  // bruit de valeur périodique (128×128, 4 octaves) pour les flammes
  const int N = 128;
  std::vector<float> g(size_t(N) * N);
  uint32_t r = 12345;
  for (auto &v : g) { r = r * 1664525u + 1013904223u; v = float((r >> 8) & 0xffff) / 65535.f; }
  noise_.assign(size_t(N) * N, 0.f);
  for (int oct = 0, cell = 32; oct < 4; oct++, cell /= 2) {
    float amp = 0.5f / float(1 << oct);
    for (int y = 0; y < N; y++)
      for (int x = 0; x < N; x++) {
        float fx = float(x) / cell, fy = float(y) / cell;
        int x0 = int(fx), y0 = int(fy);
        float tx = fx - x0, ty = fy - y0;
        tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty);
        int cn = N / cell;
        auto G = [&](int a, int b) { return g[size_t((b % cn) * 37 + oct * 911) % g.size() * 0 + size_t(((b % cn) * cn + (a % cn)) * 7 + oct * 131) % g.size()]; };
        float v = (G(x0, y0) * (1 - tx) + G(x0 + 1, y0) * tx) * (1 - ty) + (G(x0, y0 + 1) * (1 - tx) + G(x0 + 1, y0 + 1) * tx) * ty;
        noise_[size_t(y) * N + x] += v * amp;
      }
  }
  for (auto &v : noise_) v /= 0.9375f;
}

float HdView::noiseAt(float x, float y) const {
  const int N = 128;
  float fx = x - std::floor(x / N) * N, fy = y - std::floor(y / N) * N;
  int x0 = int(fx) % N, y0 = int(fy) % N, x1 = (x0 + 1) % N, y1 = (y0 + 1) % N;
  float tx = fx - std::floor(fx), ty = fy - std::floor(fy);
  return (noise_[size_t(y0) * N + x0] * (1 - tx) + noise_[size_t(y0) * N + x1] * tx) * (1 - ty) +
         (noise_[size_t(y1) * N + x0] * (1 - tx) + noise_[size_t(y1) * N + x1] * tx) * ty;
}

// bouches d'échappement : taches de la silhouette du sprite de flamme d'origine (composantes 8-connexes)
const std::vector<HdView::Jet> &HdView::jetsFor(int id, const Machine &m) {
  auto it = jets_.find(id);
  if (it != jets_.end()) return it->second;
  std::vector<Jet> &out = jets_[id];
  std::vector<int> px;
  int w, h;
  if (!m.spritePixels(id, px, w, h)) return out;
  std::vector<int> lab(px.size(), 0);
  int n = 0;
  for (int i = 0; i < int(px.size()); i++) {
    if (px[i] < 0 || lab[i]) continue;
    n++;
    std::vector<int> st{i};
    lab[i] = n;
    double sx = 0, sy = 0; int cnt = 0, ymaxB = 0;
    std::vector<int> members;
    while (!st.empty()) {
      int k = st.back(); st.pop_back();
      int x = k % w, y = k / w;
      sx += x; sy += y; cnt++; ymaxB = std::max(ymaxB, y); members.push_back(k);
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          int xx = x + dx, yy = y + dy;
          if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
          int q = yy * w + xx;
          if (px[q] >= 0 && !lab[q]) { lab[q] = n; st.push_back(q); }
        }
    }
    if (cnt >= 12) {
      // base : milieu des 3 rangées les plus basses de la tache (la flamme naît au-dessus de la bouche)
      double bx = 0; int bn = 0;
      for (int k : members) if (k / w >= ymaxB - 2) { bx += k % w; bn++; }
      // la flamme naît au centre de la bouche (entre le centre de la tache et sa base)
      double r = std::sqrt(cnt / 3.14159);
      out.push_back({(bx / bn + sx / cnt) / 2 + 0.5, std::min(double(ymaxB), sy / cnt + r * 0.35) + 0.5, r});
    }
  }
  return out;
}

// jets de feu : cœur incandescent dans la bouche, cône qui s'évase vers le spectateur (à l'opposé du
// point de fuite), mèches turbulentes, plus long et plus large avec le vent ; mélange additif
void HdView::drawJets(const std::vector<Jet> &jets, double ox0, double oy0, double strength, double t, uint32_t *out, int W,
                      int H, double s, double vpx, double vpy, const std::function<double(double)> &fx) {
  const double wk = std::clamp(wind_, 0.0, 1.0);
  struct P { double cx, cy, R, dx, dy, L, spread; int seed, x0, x1, y0, y1; };
  std::vector<P> ps;
  int seed = 0, ymin = H, ymax = 0;
  for (const Jet &j : jets) {
    seed++;
    P p;
    p.cx = fx(ox0 + j.x); p.cy = (oy0 + j.y) * s; p.R = std::max(4.0, j.r * s); p.seed = seed;
    // direction : le feu monte au-dessus de la bouche ; en roulant, le vent l'incline un peu vers
    // l'extérieur (l'arrière de la voiture)
    double side = p.cx < vpx ? -1 : 1;
    double dx = side * 0.35 * wk, dy = -1, dl = std::hypot(dx, dy);
    p.dx = dx / dl; p.dy = dy / dl;
    double flick = 0.75 + 0.5 * noiseAt(float(t * 9 + seed * 17), float(seed * 31));   // vacillement de chaque jet
    p.L = p.R * (2.9 + 1.5 * wk) * strength * flick;
    p.spread = 0;
    double ext = p.R * 1.3 + p.spread * p.L;
    p.x0 = std::max(0, int(std::min(p.cx - p.R * 1.4, p.cx + p.dx * p.L - ext)));
    p.x1 = std::min(W, int(std::max(p.cx + p.R * 1.4, p.cx + p.dx * p.L + ext)) + 1);
    p.y0 = std::max(0, int(std::min(p.cy - p.R * 1.4, p.cy + p.dy * p.L - ext)));
    p.y1 = std::min(H, int(std::max(p.cy + p.R * 1.4, p.cy + p.dy * p.L + ext)) + 1);
    ymin = std::min(ymin, p.y0); ymax = std::max(ymax, p.y1);
    ps.push_back(p);
  }
  if (ps.empty()) return;
  auto band = [&](int ya, int yb) {
    for (const P &p : ps) {
      const double nx = -p.dy, ny = p.dx, iR2 = 1.0 / (p.R * p.R);
      for (int y = std::max(ya, p.y0); y < std::min(yb, p.y1); y++) {
        uint32_t *o = out + size_t(y) * W;
        for (int x = p.x0; x < p.x1; x++) {
          double vx = x + 0.5 - p.cx, vy = y + 0.5 - p.cy;
          double a = vx * p.dx + vy * p.dy, b = vx * nx + vy * ny, d2 = (vx * vx + vy * vy) * iR2;
          // lueur dans la bouche (juste sous la base de la flamme)
          double core = d2 < 3.0 ? std::exp(-d2 * 2.0) * 1.2 : 0.0;
          double jet = 0;
          if (a > -0.35 * p.R && a < p.L) {
            double u = std::max(0.0, a) / p.L;
            float n1 = noiseAt(float(a / p.R * 6 - t * 55 + p.seed * 40), float(p.seed * 23));
            float n2 = noiseAt(float(a / p.R * 9 - t * 80 + p.seed * 11), float(b / p.R * 3 + p.seed * 7));
            float n3 = noiseAt(float(a / p.R * 22 - t * 140), float(b / p.R * 14 + p.seed * 5));
            // flamme de bougie : large à la base, effilée au bout, qui ondule de plus en plus vers la pointe
            double width = p.R * 0.95 * std::pow(1 - u, 0.6) * (0.85 + 0.35 * n1) + 1e-6;
            double bb = std::fabs(b + (n2 - 0.5) * p.R * 1.6 * u) / width;
            if (bb < 1.3) {
              double body = std::clamp(1.15 - bb, 0.0, 1.0);
              double start = std::clamp((a + 0.35 * p.R) / (0.7 * p.R), 0.0, 1.0);
              start = start * start * (3 - 2 * start);
              double lick = std::clamp(0.7 + (n3 - 0.5) * 1.6 * u, 0.0, 1.0);   // mèches à la pointe
              jet = std::pow(body, 0.6) * start * lick * (1.6 - 0.9 * u);
            }
          }
          double hgt = std::max(core, jet) * strength;
          if (hgt <= 0.01) continue;
          hgt = std::min(hgt, 1.3);
          // couleur du feu : rouge sombre -> orange -> jaune -> blanc
          double r = std::clamp(hgt * 2.2, 0.0, 1.0), g = std::clamp(hgt * 2.0 - 0.6, 0.0, 1.0), bl = std::clamp(hgt * 2.2 - 1.7, 0.0, 1.0);
          // le feu dense couvre le fond (visible même sur la route claire), le reste s'ajoute en lueur
          double cover = std::clamp(hgt * 1.6 - 0.25, 0.0, 0.92), glow = std::clamp(hgt * 1.2, 0.0, 1.0) * 0.45;
          uint32_t c = o[x];
          double R0 = (c >> 16) & 255, G0 = (c >> 8) & 255, B0 = c & 255;
          int R8 = std::min(255, int(R0 * (1 - cover) + 255 * r * (cover + glow)));
          int G8 = std::min(255, int(G0 * (1 - cover) + 255 * g * (cover + glow)));
          int B8 = std::min(255, int(B0 * (1 - cover) + 255 * bl * (cover + glow)));
          o[x] = 0xff000000u | (uint32_t(R8) << 16) | (uint32_t(G8) << 8) | uint32_t(B8);
        }
      }
    }
  };
  // bandes horizontales sur tous les cœurs (les jets se recouvrent : chaque bande les traite tous)
  int nt = std::max(1, std::min(int(std::thread::hardware_concurrency()), 16));
  std::vector<std::thread> th;
  for (int k = 0; k < nt; k++) {
    int ya = ymin + (ymax - ymin) * k / nt, yb = ymin + (ymax - ymin) * (k + 1) / nt;
    th.emplace_back(band, ya, yb);
  }
  for (auto &tt : th) tt.join();
}

void HdView::afterFrame(const Machine &m) {
  frame_++;
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

// image HD d'un sprite : celle du dossier hd/ (les flammes sans image sont dessinées par drawJets)
const HdSprite *HdView::spriteFor(int id, const Machine &) { return assets.sprite(id); }

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
  // hauteur : au sol, ou en vol si la route descend plus vite que la chute libre
  const double G = 16;   // gravité, en hauteur brute par tick² (mesurée sur la voiture du joueur)
  if (!oppHave_ || std::fabs(surf - oppY_) > 3000) { oppY_ = surf; oppVy_ = 0; oppHave_ = true; }
  else {
    double pred = oppY_ + oppVy_ - G / 2;
    if (surf >= pred - 2) { oppVy_ = std::clamp(surf - oppY_, -400.0, 400.0); oppY_ = surf; }
    else { oppY_ = pred; oppVy_ -= G; }
  }
  o.yRaw = oppY_;
  o.valid = true;
  return o;
}

OppPose HdView::opponentAt(double t) const {
  if (snaps_.empty()) return {};
  if (t <= snaps_.front().t) return snaps_.front().opp;
  for (size_t i = 0; i + 1 < snaps_.size(); i++) {
    const OppPose &a = snaps_[i].opp, &b = snaps_[i + 1].opp;
    if (t >= snaps_[i + 1].t) continue;
    if (!a.valid || !b.valid) return b.valid ? b : a;
    double k = (t - snaps_[i].t) / (snaps_[i + 1].t - snaps_[i].t);
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
void HdView::addCar(const OppPose &o, double hs, const Machine &m) {
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
  // roue : prisme octogonal d'axe latéral
  auto wheel = [&](double xc, double zc, double rad, double wid) {
    Vec3 a[8], b[8];
    for (int k = 0; k < 8; k++) {
      double ang = (k + 0.5) * 3.14159265 / 4, yy = rad + std::sin(ang) * rad, zz = zc + std::cos(ang) * rad;
      a[k] = W(xc - wid / 2, yy, zz); b[k] = W(xc + wid / 2, yy, zz);
    }
    for (int k = 0; k < 8; k++) {
      Vec3 q[4] = {a[k], a[(k + 1) % 8], b[(k + 1) % 8], b[k]};
      r3d_.poly(q, 4, black);
    }
    r3d_.poly(a, 8, grey);
    r3d_.poly(b, 8, grey);
  };
  const double wx = 78, rearZ = -90, frontZ = 95;
  wheel(-wx, rearZ, 30, 34); wheel(wx, rearZ, 30, 34);
  wheel(-wx + 4, frontZ, 24, 26); wheel(wx - 4, frontZ, 24, 26);
  box(-52, 52, 14, 34, -130, 120, redLight, red, dark);      // châssis
  box(-34, 34, 14, 30, 120, 150, redLight, red, dark);        // nez
  box(-46, 46, 34, 58, -120, -10, red, dark, red);            // moteur / habitacle
  box(-40, 40, 58, 64, -70, -20, pink, red, pink);            // arceau
  box(-60, 60, 50, 56, -138, -118, redLight, dark, dark);     // aileron
}

void HdView::renderScene(const Pose &p, int track, uint32_t *out, int W, int H, double focal, double cx, double cy,
                         const Machine &m, const OppPose *opp) {
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
    // en triangles : sur un virage relevé, les 4 coins d'une section ne sont pas coplanaires
    auto quad = [&](const Vec3 *q, uint32_t col) {
      Vec3 t1[3] = {q[0], q[1], q[2]}, t2[3] = {q[0], q[2], q[3]};
      r3d_.poly(t1, 3, col);
      r3d_.poly(t2, 3, col);
    };
    quad(mid, rc);
    Vec3 ql[4] = {aL, aL1, bL1, bL};
    quad(ql, line);
    Vec3 qr[4] = {aR1, aR, bR, bR1};
    quad(qr, line);
  }
  if (opp && opp->valid) addCar(*opp, HSCALE, m);
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
  wind_ += (windTarget_ - wind_) * 0.08;   // lissé (affichage à 60 i/s)
  Pose p = poseAt(T);
  int track = snaps_.back().track;
  OppPose opp = opponentAt(T);
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
  for (const SpriteDraw &e : visible_) {
    const HdSprite *spr = spriteFor(e.id, m);
    bool flameId = std::find(std::begin(FLAME_IDS), std::end(FLAME_IDS), e.id) != std::end(FLAME_IDS);
    if (!spr && flameId && params.builtinFlames) {
      // intensité selon l'image d'animation du jeu (6/8 la plus forte, 49/50 la plus faible)
      double strength = (e.id == 6 || e.id == 8) ? 1.0 : (e.id == 7 || e.id == 9) ? 0.85 : 0.72;
      drawJets(jetsFor(e.id, m), e.x, e.y, strength, T / 50.0, out, W, H, s, fx(params.cx), params.cy * s, fx);
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
