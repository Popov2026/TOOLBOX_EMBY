// track.cpp — décodage des circuits (port fidèle de $48390, vérifié octet par octet en JS)
#include "track.hpp"

#include <cmath>
#include <stdexcept>

namespace scr {
namespace {

constexpr uint32_t BASE = 0x10100, BLOB = 0x13670, TRACK_TABLE = 0x13790, SHAPE_TABLE = 0x13670,
                   PROFILE_TABLE = 0x13690, SPECIAL_BBA = 0x48802, SPECIAL_C1E = 0x48804, NAMES = 0x13498;
const char *const TRACK_NAMES[8] = {"LITTLE RAMP", "STEPPING STONES", "HUMP BACK", "BIG RAMP",
                                    "SKI JUMP",    "DRAW BRIDGE",     "HIGH JUMP", "ROLLER COASTER"};

struct Mem {
  const Bytes &img;
  uint8_t b(uint32_t a) const {
    if (a < BASE || a - BASE >= img.size()) throw std::runtime_error("circuits : adresse hors programme");
    return img[a - BASE];
  }
  uint32_t le(uint32_t a) const { return b(a) | (b(a + 1) << 8); }
};
uint32_t p6502(uint32_t a) { return BLOB + (a - 0xb100); }
int s16(uint32_t v) { return int16_t(uint16_t(v)); }

uint32_t profile(const Mem &m, uint32_t ptr, bool neg, int i) {
  uint32_t a = p6502(ptr);
  if (neg) return ((m.b(a + 2 * i) & 0x7f) << 8) | m.b(a + 2 * i + 1);
  uint32_t x = m.b(a + i);
  return ((x & 0xf) << 8) | ((x << 1) & 0xe0);
}
bool profileFlag(const Mem &m, uint32_t ptr, bool neg, int i) { return m.b(p6502(ptr) + (neg ? 2 * i : i)) & 0x80; }

struct Piece { int x, z, type; uint32_t hL, hR, profL, profR; bool neg; };

Track buildTrack(const Mem &m, int track) {
  uint32_t ptr = p6502(m.le(TRACK_TABLE + 2 * track)), pos = 0;
  auto fetch = [&] { return m.b(ptr + pos++); };
  int hdr[4];
  for (int &h : hdr) h = fetch();
  uint32_t lo = fetch(), hi = fetch(), hl = (hi << 8) | lo, hr = hl;
  int c6 = 0, repeat = 0, prevType = 0, prevCoord = 0;
  std::vector<Piece> pieces;
  while ((int)pieces.size() < hdr[0]) {
    int t, ce6, dirb, c, bba, v;
    if (repeat) {
      repeat--; t = prevType; ce6 = dirb = t;
      if (t & 0x10) dirb ^= 0xc0;
      int step = (dirb & 0x40) ? 1 : 0x10;
      c = ((dirb & 0x80) ? prevCoord - step : prevCoord + step) & 0xff;
    } else {
      t = fetch();
      if ((t & 0xf) == 0xf) { repeat = t >> 4; continue; }
      ce6 = dirb = prevType = t; c = fetch();
    }
    prevCoord = c;
    int b905 = (c6 & 2) ? 0x80 : 0;
    if ((dirb & 0xf) >= 12) {
      ce6 &= 0xf0; bba = m.b(SPECIAL_BBA + (dirb & 0xf)); v = m.b(SPECIAL_C1E + (dirb & 0xf));
    } else {
      bba = fetch(); v = (dirb & 0x20) ? bba : fetch();
    }
    int c1e = (v & 0x7f) | b905;
    uint32_t pL = m.le(PROFILE_TABLE + ((bba << 1) & 0xff)), pR = m.le(PROFILE_TABLE + ((c1e << 1) & 0xff));
    bool neg = bba & 0x80;
    c6 = (c1e & 0x80) ? 2 : 0;
    uint32_t shape = p6502(m.le(SHAPE_TABLE + 2 * (ce6 & 0xf)));
    int nsec = m.b(shape + m.b(shape)), segs = ((nsec >> 1) - 1) & 0xff;
    uint32_t startL = (hl - profile(m, pL, neg, 0)) & 0xffff; hl = (startL + profile(m, pL, neg, segs)) & 0xffff;
    uint32_t startR = (hr - profile(m, pR, neg, 0)) & 0xffff; hr = (startR + profile(m, pR, neg, segs)) & 0xffff;
    pieces.push_back({c & 0xf, c >> 4, ce6, startL, startR, pL, pR, neg});
    c6 = (c6 + nsec - 2) & 2;
  }

  Track tr;
  tr.name = TRACK_NAMES[track];
  tr.pieces = (int)pieces.size();
  tr.startPiece = hdr[2];
  std::vector<Section> all;
  for (int pi = 0; pi < (int)pieces.size(); pi++) {
    const Piece &p = pieces[pi];
    int typ = p.type;
    uint32_t a = p6502(m.le(SHAPE_TABLE + 2 * (typ & 0xf))), o = m.b(a), n = m.b(a + o), base = a + o + 7;
    std::vector<std::pair<int, int>> pts;
    for (uint32_t i = 0; i < n; i++) pts.push_back({s16(m.le(base + 4 * i)), s16(m.le(base + 4 * i + 2))});
    struct S { std::pair<int, int> l, r; uint32_t hl, hr; int fl; };
    std::vector<S> secs;
    for (size_t k = 0; k + 1 < pts.size(); k += 2) {
      int i = int(k / 2);
      secs.push_back({pts[k], pts[k + 1], (p.hL + profile(m, p.profL, p.neg, i)) & 0xffff,
                      (p.hR + profile(m, p.profR, p.neg, i)) & 0xffff,
                      (profileFlag(m, p.profL, p.neg, i) ? 1 : 0) | (profileFlag(m, p.profR, p.neg, i) ? 2 : 0)});
    }
    if (typ & 0x10) {   // parcours inverse : sections à l'envers, gauche/droite permutés (hauteurs comprises par index)
      std::vector<S> rev;
      for (size_t k = secs.size(); k-- > 0;) rev.push_back({secs[k].r, secs[k].l, 0, 0, 0});
      for (size_t k = 0; k < rev.size(); k++) { rev[k].hl = secs[k].hl; rev[k].hr = secs[k].hr; rev[k].fl = secs[k].fl; }
      secs = rev;
    }
    int rot = (typ >> 6) & 3;
    auto R = [&](std::pair<int, int> q) -> std::pair<int, int> {
      int x = q.first, z = q.second;
      switch (rot) {
        case 1: return {z, CELL - x};
        case 2: return {CELL - x, CELL - z};
        case 3: return {CELL - z, x};
        default: return {x, z};
      }
    };
    for (size_t k = 0; k < secs.size(); k++) {
      if (pi > 0 && k == 0) continue;    // section partagée avec la pièce précédente
      auto l = R(secs[k].l), r = R(secs[k].r);
      all.push_back({float(l.first + p.x * CELL), float(secs[k].hl), float(l.second + p.z * CELL),
                     float(r.first + p.x * CELL), float(secs[k].hr), float(r.second + p.z * CELL), secs[k].fl, pi});
    }
  }
  // circuit fermé : la dernière section coïncide avec la première
  if (all.size() > 2 && std::fabs(all[0].lx - all.back().lx) + std::fabs(all[0].lz - all.back().lz) < 64) all.pop_back();
  // commencer à la pièce de départ
  size_t first = 0;
  while (first < all.size() && all[first].piece != tr.startPiece) first++;
  if (first < all.size()) {
    tr.secs.assign(all.begin() + first, all.end());
    tr.secs.insert(tr.secs.end(), all.begin(), all.begin() + first);
  } else tr.secs = all;
  return tr;
}

}  // namespace

std::vector<Track> decodeTracks(const Bytes &image) {
  Mem m{image};
  for (int k = 0; k < 11; k++)
    if (m.b(NAMES + k) != (uint8_t)"LITTLE RAMP"[k]) throw std::runtime_error("version du jeu non reconnue");
  std::vector<Track> out;
  for (int n = 0; n < 8; n++) out.push_back(buildTrack(m, n));
  return out;
}

}  // namespace scr
