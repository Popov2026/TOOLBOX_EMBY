// tnt.cpp — lecture des circuits de Stunt Car Racer TNT (voir tnt.hpp)
#include "tnt.hpp"

#include <cstring>

namespace scr {
namespace {

uint32_t be32(const Bytes &d, size_t p) { return (uint32_t(d[p]) << 24) | (d[p + 1] << 16) | (d[p + 2] << 8) | d[p + 3]; }

// ---------------------------------------------------------------- archive LHA (niveaux 0/1, -lh0- et -lh5-)
struct BitIn {
  const uint8_t *d; size_t n, p = 0; uint32_t buf = 0; int cnt = 0;
  unsigned get(int k) {
    while (cnt < k) { buf = (buf << 8) | (p < n ? d[p] : 0); p++; cnt += 8; }
    cnt -= k;
    unsigned v = (buf >> cnt) & ((1u << k) - 1);
    buf &= (1u << cnt) - 1;
    return v;
  }
};

// code de Huffman canonique (longueurs de 1 à 16)
struct Huff {
  int count[17] = {}, first[17] = {}, offset[17] = {};
  std::vector<int> sym;
  int single = -1;   // un seul symbole (aucun bit lu)
  void build(const std::vector<int> &len) {
    std::memset(count, 0, sizeof count);
    for (int l : len) if (l > 0 && l <= 16) count[l]++;
    sym.assign(len.size(), 0);
    int code = 0, idx = 0;
    for (int l = 1; l <= 16; l++) { first[l] = code; offset[l] = idx; idx += count[l]; code = (code + count[l]) << 1; }
    std::vector<int> pos(offset, offset + 17);
    for (size_t s = 0; s < len.size(); s++) if (len[s] > 0 && len[s] <= 16) sym[size_t(pos[len[s]]++)] = int(s);
  }
  int decode(BitIn &b) const {
    if (single >= 0) return single;
    int code = 0;
    for (int l = 1; l <= 16; l++) {
      code = (code << 1) | int(b.get(1));
      if (code - first[l] < count[l]) return sym[size_t(offset[l] + code - first[l])];
    }
    throw std::runtime_error("archive LHA : code invalide");
  }
};

void readPt(BitIn &b, int nn, int nbit, int special, Huff &h) {
  int n = int(b.get(nbit));
  if (n == 0) { h.single = int(b.get(nbit)); return; }
  std::vector<int> len(size_t(nn), 0);
  for (int i = 0; i < n && i < nn;) {
    int c = int(b.get(3));
    if (c == 7) while (b.get(1)) c++;
    len[size_t(i++)] = c;
    if (i == special) i += int(b.get(2));
  }
  h.single = -1; h.build(len);
}

Bytes lh5(const uint8_t *data, size_t n, size_t size) {
  BitIn b{data, n};
  Bytes out;
  out.reserve(size);
  while (out.size() < size) {
    unsigned blk = b.get(16);
    Huff pt, c, p;
    readPt(b, 19, 5, 3, pt);
    int nc = int(b.get(9));
    if (nc == 0) c.single = int(b.get(9));
    else {
      std::vector<int> clen(510, 0);
      for (int i = 0; i < nc && i < 510;) {
        int k = pt.decode(b);
        if (k <= 2) {
          int run = k == 0 ? 1 : k == 1 ? int(b.get(4)) + 3 : int(b.get(9)) + 20;
          i += run;
        } else clen[size_t(i++)] = k - 2;
      }
      c.build(clen);
    }
    readPt(b, 14, 4, -1, p);
    for (unsigned k = 0; k < blk && out.size() < size; k++) {
      int s = c.decode(b);
      if (s < 256) { out.push_back(uint8_t(s)); continue; }
      int len = s - 256 + 3, pp = p.decode(b);
      size_t dist = pp <= 1 ? size_t(pp) : (size_t(1) << (pp - 1)) + b.get(pp - 1);
      if (dist + 1 > out.size()) throw std::runtime_error("archive LHA : données corrompues");
      size_t st = out.size() - dist - 1;
      for (int q = 0; q < len; q++) out.push_back(out[st + size_t(q)]);
    }
  }
  out.resize(size);
  return out;
}

// fichier « SCR-TNT » de l'archive (ou vide)
Bytes fromLha(const Bytes &d) {
  for (size_t p = 0; p + 24 < d.size() && d[p];) {
    size_t hs = d[p], lvl = d[p + 20], nl = d[p + 21];
    uint32_t comp = d[p + 7] | (d[p + 8] << 8) | (d[p + 9] << 16) | (uint32_t(d[p + 10]) << 24);
    uint32_t orig = d[p + 11] | (d[p + 12] << 8) | (d[p + 13] << 16) | (uint32_t(d[p + 14]) << 24);
    std::string name(reinterpret_cast<const char *>(&d[p + 22]), nl);
    size_t ext = 0;
    if (lvl == 1) {   // en-têtes étendus comptés dans la taille compressée
      size_t q = p + 22 + nl + 3, e = size_t(d[q] | (d[q + 1] << 8)); q += 2;
      while (e && q + e <= d.size()) { ext += e; q += e; e = size_t(d[q - 2] | (d[q - 1] << 8)); }
    } else if (lvl != 0) break;
    size_t data = p + 2 + hs + ext, clen = comp - ext;
    if (data + clen > d.size()) break;
    if (name == "SCR-TNT") {
      if (!std::memcmp(&d[p + 2], "-lh5-", 5)) return lh5(&d[data], clen, orig);
      if (!std::memcmp(&d[p + 2], "-lh0-", 5)) return Bytes(d.begin() + long(data), d.begin() + long(data + clen));
    }
    p = p + 2 + hs + comp;
  }
  return {};
}

}  // namespace

bool loadTrackSet(const std::string &path, TrackSet &ts, std::string &err) {
  Bytes d;
  try { d = readFile(path); } catch (std::exception &) { err = "fichier introuvable : " + path; return false; }
  if (d.size() > 7 && !std::memcmp(&d[2], "-lh", 3)) {
    try { d = fromLha(d); } catch (std::exception &e) { err = e.what(); return false; }
    if (d.empty()) { err = "pas de fichier SCR-TNT dans l'archive"; return false; }
  }
  // exécutable Amiga : premier hunk = chargeur TNT ; données à des positions fixes de ce chargeur
  if (d.size() < 0x1000 || be32(d, 0) != 0x3f3 || be32(d, 0x20) != 0x3e9) { err = "ce n'est pas le fichier SCR-TNT de Stunt Car Racer TNT"; return false; }
  const size_t L = 0x28;   // début du code du chargeur
  if (std::memcmp(&d[L + 0x2fe], "DIZZY DESCENT", 13) != 0) { err = "version de SCR-TNT non reconnue"; return false; }
  for (int k = 0; k < 8; k++) ts.names[k].assign(reinterpret_cast<const char *>(&d[L + 0x2fe + 16 * k]), 16);
  std::memcpy(ts.initials, &d[L + 0x2ee], 16);
  std::memcpy(ts.counts, &d[L + 0x37e], 8);
  std::memcpy(ts.params, &d[L + 0xaba], 64);
  for (int k = 0; k < 8; k++) ts.offsets[k] = uint16_t((d[L + 0x386 + 2 * k] << 8) | d[L + 0x387 + 2 * k]);
  ts.data.assign(d.begin() + long(L + 0x396), d.begin() + long(L + 0xaba));   // tracés, jusqu'à la table suivante
  for (uint16_t o : ts.offsets) if (o >= ts.data.size()) { err = "table des circuits TNT invalide"; return false; }
  return true;
}

}  // namespace scr
