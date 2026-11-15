// track.cpp — décodage des circuits (port fidèle de $48390, vérifié octet par octet en JS)
#include "track.hpp"
#include "tnt.hpp"

#include <algorithm>
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
  const Bytes *extra = nullptr;   // circuits TNT : table + tracés, à TNT_TABLE / TNT_DATA
  Bytes tntTable;
  uint8_t b(uint32_t a) const {
    if (extra && a >= TNT_DATA && a - TNT_DATA < extra->size()) return (*extra)[a - TNT_DATA];
    if (extra && a >= TNT_TABLE && a - TNT_TABLE < tntTable.size()) return tntTable[a - TNT_TABLE];
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

Track buildTrack(const Mem &m, int track, uint32_t table, uint32_t base, const std::string &name) {
  uint32_t ptr = base + (m.le(table + 2 * track) - 0xb100), pos = 0;
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
  tr.name = name;
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
    if (pi > 0 && !secs.empty() && !all.empty()) {   // raccord avec la pièce précédente
      auto l = R(secs[0].l), r = R(secs[0].r);
      const Section &e = all.back();
      const float g = std::max(std::hypot(float(l.first + p.x * CELL) - e.lx, float(l.second + p.z * CELL) - e.lz),
                               std::hypot(float(r.first + p.x * CELL) - e.rx, float(r.second + p.z * CELL) - e.rz));
      if (g > tr.maxGap) { tr.maxGap = g; tr.gapPiece = pi; }
    }
    for (size_t k = 0; k < secs.size(); k++) {
      if (pi > 0 && k == 0) continue;    // section partagée avec la pièce précédente
      auto l = R(secs[k].l), r = R(secs[k].r);
      all.push_back({float(l.first + p.x * CELL), float(secs[k].hl), float(l.second + p.z * CELL),
                     float(r.first + p.x * CELL), float(secs[k].hr), float(r.second + p.z * CELL), secs[k].fl, pi});
    }
  }
  if (all.size() > 2) {   // fermeture : la dernière section doit retrouver la première
    const Section &f0 = all[0], &e = all.back();
    tr.closeGap = std::max(std::hypot(f0.lx - e.lx, f0.lz - e.lz), std::hypot(f0.rx - e.rx, f0.rz - e.rz));
    tr.closeHeight = std::max(std::fabs(f0.ly - e.ly), std::fabs(f0.ry - e.ry));
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

std::vector<Track> decodeTracks(const Bytes &image, const TrackSet *ts) {
  Mem m{image};
  for (int k = 0; k < 11; k++)
    if (m.b(NAMES + k) != (uint8_t)"LITTLE RAMP"[k]) throw std::runtime_error("version du jeu non reconnue");
  std::vector<Track> out;
  if (ts) {   // circuits TNT : même routine, autre table et autre base (comme le chargeur de la version TNT)
    m.extra = &ts->data;
    for (uint16_t o : ts->offsets) { uint16_t p = uint16_t(0xb100 + o); m.tntTable.push_back(uint8_t(p)); m.tntTable.push_back(uint8_t(p >> 8)); }
    for (int n = 0; n < 8; n++) {
      std::string nm = ts->names[n];
      while (!nm.empty() && nm.back() == ' ') nm.pop_back();
      out.push_back(buildTrack(m, n, TNT_TABLE, TNT_DATA, nm));
    }
    return out;
  }
  for (int n = 0; n < 8; n++) out.push_back(buildTrack(m, n, TRACK_TABLE, BLOB, TRACK_NAMES[n]));
  return out;
}

bool checkTrackSet(const Bytes &image, const TrackSet &ts, std::string &report) {
  std::vector<Track> tracks;
  try { tracks = decodeTracks(image, &ts); }
  catch (std::exception &e) { report = std::string("circuits illisibles : ") + e.what(); return false; }
  bool ok = true;
  report.clear();
  char line[200];
  for (size_t k = 0; k < tracks.size(); k++) {
    const Track &t = tracks[k];
    std::string name = ts.names[k];
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (t.pieces < 4 || t.secs.size() < 8) {
      std::snprintf(line, sizeof line, "%s : trop peu de pièces (%d)\n", name.c_str(), t.pieces); report += line; ok = false; continue;
    }
    if (t.maxGap > 48) {   // les circuits d'origine se raccordent à 1 unité près
      std::snprintf(line, sizeof line, "%s : la pièce %d ne se raccorde pas à la précédente (écart %.0f)\n", name.c_str(), t.gapPiece + 1, t.maxGap);
      report += line; ok = false;
    }
    if (t.closeGap > 48 || t.closeHeight > 32) {
      std::snprintf(line, sizeof line, "%s : le circuit ne se referme pas (écart %.0f, hauteur %.0f)\n", name.c_str(), t.closeGap, t.closeHeight);
      report += line; ok = false;
    }
  }
  if (ok) report = "8 circuits valides";
  return ok;
}

TrackSet originalTrackSet(const Bytes &image) {
  Mem m{image};
  TrackSet ts;
  uint32_t lo = 0xffffffff, hi = 0;
  for (int k = 0; k < 8; k++) {
    const uint32_t off = m.le(TRACK_TABLE + 2 * k) - 0xb100;
    lo = std::min(lo, off); hi = std::max(hi, off + 256);
    for (int c = 0; c < 16; c++) ts.names[k] += char(m.b(NAMES + 16 * k + c));
  }
  hi = std::min<uint32_t>(hi, uint32_t(image.size() - (BLOB - BASE)));
  for (int k = 0; k < 8; k++) ts.offsets[k] = uint16_t(m.le(TRACK_TABLE + 2 * k) - 0xb100 - lo);
  ts.data.assign(image.begin() + (BLOB - BASE) + lo, image.begin() + (BLOB - BASE) + hi);
  for (int k = 0; k < 16; k++) ts.initials[k] = m.b(0x4d35c + k);
  for (int k = 0; k < 8; k++) ts.counts[k] = m.b(0x4e7e4 + k);
  for (int k = 0; k < 64; k++) ts.params[k] = m.b(0x1451a + k);
  return ts;
}

// formes : sections transversales (gauche, droite) en coordonnées locales de case, dans l'ordre
std::string shapesJson(const Bytes &image) {
  Mem m{image};
  std::string j = "[";
  for (int s = 0; s < 16; s++) {
    std::string one;
    try {   // forme absente ou inutilisée : liste vide
      uint32_t a = p6502(m.le(SHAPE_TABLE + 2 * s)), o = m.b(a), n = m.b(a + o), base = a + o + 7;
      for (uint32_t i = 0; i < n; i++)
        one += (i ? ",[" : "[") + std::to_string(s16(m.le(base + 4 * i))) + "," + std::to_string(s16(m.le(base + 4 * i + 2))) + "]";
    } catch (...) { one.clear(); }
    j += (s ? ",[" : "[") + one + "]";
  }
  return j + "]";
}

// profils de hauteur : pour chaque code (0 à 127), hauteurs relatives des 16 premières sections, format
// court (octets) et format long (mots, profil gauche avec le bit 7)
std::string profilesJson(const Bytes &image) {
  Mem m{image};
  std::string j = "[";
  for (int code = 0; code < 128; code++) {
    const uint32_t ptr = m.le(PROFILE_TABLE + ((code << 1) & 0xff));
    j += code ? ",{" : "{";
    for (int neg = 0; neg < 2; neg++) {
      j += neg ? ",\"w\":[" : "\"b\":[";
      for (int i = 0; i < 16; i++) {
        long v = -1;
        try { v = long(profile(m, ptr, neg != 0, i)); } catch (...) {}
        j += (i ? "," : "") + std::to_string(v);
      }
      j += "]";
    }
    j += "}";
  }
  return j + "]";
}

}  // namespace scr
