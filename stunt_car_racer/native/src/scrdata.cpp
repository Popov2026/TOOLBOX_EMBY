// scrdata.cpp — voir scrdata.hpp (port de tools/scr_tool.py, vérifié sur la même image).
#include "scrdata.hpp"

#include <cctype>
#include <cstring>
#include <cstdio>
#include <functional>

namespace scr {

// lectures bornées : un fichier mal formé (ou piégé) donne une erreur, jamais une lecture ou une
// écriture hors du tampon
static void need(const Bytes &b, size_t o, size_t n) {
  if (o > b.size() || n > b.size() - o) throw std::runtime_error("fichier du jeu invalide");
}
static uint8_t at(const Bytes &b, size_t o) { need(b, o, 1); return b[o]; }
static uint16_t le16(const Bytes &b, size_t o) { need(b, o, 2); return b[o] | (b[o + 1] << 8); }
static uint32_t be32(const Bytes &b, size_t o) {
  need(b, o, 4);
  return (uint32_t(b[o]) << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3];
}

std::map<std::string, Bytes> fatList(const Bytes &img) {
  if (img.size() < 512) throw std::runtime_error("image disque trop petite");
  unsigned bps = le16(img, 11), spc = img[13] ? img[13] : 1, res = le16(img, 14), nfat = img[16];
  unsigned ndir = le16(img, 17), spf = le16(img, 22);
  size_t fat = size_t(res) * bps, root = size_t(res + nfat * spf) * bps, data = root + ndir * 32;
  size_t csize = size_t(spc) * bps;
  auto next = [&](unsigned n) {
    size_t o = fat + n * 3 / 2;
    unsigned v = le16(img, o);
    return (n & 1) ? v >> 4 : v & 0xfff;
  };
  auto chain = [&](unsigned cl) {
    Bytes out;
    for (int guard = 0; cl >= 2 && cl < 0xff0 && guard < 4096; guard++) {
      size_t o = data + size_t(cl - 2) * csize;
      if (o + csize > img.size()) break;
      out.insert(out.end(), img.begin() + o, img.begin() + o + csize);
      cl = next(cl);
    }
    return out;
  };
  std::map<std::string, Bytes> files;
  std::function<void(const Bytes &, const std::string &, int)> walk;
  walk = [&](const Bytes &buf, const std::string &path, int depth) {
    if (depth > 8) return;   // dossiers qui se contiennent eux-mêmes
    for (size_t i = 0; i + 32 <= buf.size(); i += 32) {
      if (buf[i] == 0) break;
      if (buf[i] == 0xe5 || buf[i] == 0x2e) continue;
      std::string name(reinterpret_cast<const char *>(&buf[i]), 8), ext(reinterpret_cast<const char *>(&buf[i + 8]), 3);
      while (!name.empty() && name.back() == ' ') name.pop_back();
      while (!ext.empty() && ext.back() == ' ') ext.pop_back();
      if (!ext.empty()) name += "." + ext;
      unsigned attr = buf[i + 11], cl = le16(buf, i + 26);
      uint32_t size = buf[i + 28] | (buf[i + 29] << 8) | (buf[i + 30] << 16) | (uint32_t(buf[i + 31]) << 24);
      if (attr & 0x08) continue;
      if (attr & 0x10) walk(chain(cl), path + name + "/", depth + 1);
      else {
        Bytes d = chain(cl);
        if (d.size() > size) d.resize(size);
        files[path + name] = d;
      }
    }
  };
  need(img, root, data - root);
  walk(Bytes(img.begin() + root, img.begin() + data), "", 0);
  return files;
}

Bytes byteKiller(const Bytes &buf, size_t end, bool &ok) {
  size_t p = end;
  if (end > buf.size()) throw std::runtime_error("flux compressé invalide");
  auto rl = [&]() { if (p < 4) throw std::runtime_error("flux compressé invalide"); p -= 4; return be32(buf, p); };
  uint32_t length = rl(), crc = rl(), d0 = rl();
  crc ^= d0;
  if (length > 16 * 1024 * 1024) throw std::runtime_error("flux compressé invalide");
  Bytes out(length);
  int64_t q = length;
  auto bit = [&]() {
    uint32_t c = d0 & 1;
    d0 >>= 1;
    if (d0 == 0) { uint32_t n = rl(); crc ^= n; c = n & 1; d0 = (n >> 1) | 0x80000000u; }
    return c;
  };
  auto bits = [&](int n) { uint32_t v = 0; while (n--) v = (v << 1) | bit(); return v; };
  auto lit = [&](uint32_t n) { while (n--) { if (--q < 0) throw std::runtime_error("flux invalide"); out[q] = uint8_t(bits(8)); } };
  auto copy = [&](uint32_t n, uint32_t off) {
    while (n--) { if (--q < 0 || q + off >= length) throw std::runtime_error("flux invalide"); out[q] = out[q + off]; }
  };
  while (q > 0) {
    if (!bit()) {
      if (!bit()) lit(bits(3) + 1);
      else copy(2, bits(8));
    } else {
      uint32_t t = bits(2);
      if (t == 3) lit(bits(8) + 9);
      else if (t == 2) { uint32_t n = bits(8) + 1; copy(n, bits(12)); }
      else copy(t + 3, bits(9 + t));
    }
  }
  ok = crc == 0;
  return out;
}

Bytes unpackPrg(const Bytes &prg, bool &ok) {
  if (prg.size() < 0x1c) throw std::runtime_error("exécutable invalide");
  return byteKiller(prg, 0x1c + be32(prg, 2) + be32(prg, 6), ok);
}

static Bytes innerFromUnpacked(const Bytes &out) {
  size_t tlen = be32(out, 2);
  for (size_t i = (0x1c + tlen > 0x40 ? 0x1c + tlen - 0x40 : 0); i + 1 < out.size(); i++)
    if (out[i] == 0x60 && out[i + 1] == 0x1a) return Bytes(out.begin() + i, out.end());
  throw std::runtime_error("programme du jeu introuvable dans GAME.PUT");
}

// Pack-Ice (« ICE! », compacteur courant sur Atari ST) : décompression, en lisant les données à
// l'envers comme la routine 68000 d'origine. wide : lecture des bits par mots de 32 bits (version
// 2.4) au lieu d'octets (2.3x). Toutes les lectures et écritures sont bornées.
Bytes iceDepack(const Bytes &f, bool wide) {
  size_t st = std::string::npos;
  for (size_t i = 0; i + 12 <= f.size(); i++)
    if (f[i] == 'I' && f[i + 1] == 'C' && f[i + 2] == 'E' && f[i + 3] == '!') { st = i; break; }
  if (st == std::string::npos) throw std::runtime_error("pas de données Pack-Ice");
  const size_t plen = be32(f, st + 4), ulen = be32(f, st + 8);
  if (plen < 12 || st + plen > f.size() || ulen == 0 || ulen > 4 * 1024 * 1024) throw std::runtime_error("Pack-Ice : en-tête invalide");
  Bytes out(ulen);
  size_t src = st + plen;            // a5 : fin des données compressées (lues à l'envers)
  const size_t srcMin = st + 12;
  int64_t dst = int64_t(ulen);        // a6 : fin de la destination
  uint32_t buf = 0;
  auto bad = [] { return std::runtime_error("Pack-Ice : données invalides"); };
  auto rdByte = [&]() -> uint32_t { if (src <= srcMin) throw bad(); return f[--src]; };
  auto reload = [&]() -> uint32_t {
    if (!wide) return rdByte();
    uint32_t v = 0; for (int k = 0; k < 4; k++) v |= rdByte() << (8 * k); return v;   // move.l -(a5) : octets lus de la fin
  };
  const uint32_t topBit = wide ? 0x80000000u : 0x80u, mask = wide ? 0xffffffffu : 0xffu;
  auto bit = [&]() -> uint32_t {        // add d7,d7 ; si zéro : nouveau mot, addx (bit marqueur)
    uint32_t c = (buf & topBit) ? 1 : 0;
    buf = (buf << 1) & mask;
    if (buf == 0) { uint32_t n = reload(); c = (n & topBit) ? 1 : 0; buf = ((n << 1) | 1) & mask; }
    return c;
  };
  auto bits = [&](int n) -> uint32_t { uint32_t v = 0; for (int k = 0; k <= n; k++) v = (v << 1) | bit(); return v; };   // dbf : n+1 bits
  auto put = [&](uint8_t v) { if (dst <= 0) throw bad(); out[size_t(--dst)] = v; };
  buf = reload();
  static const uint32_t dmask[5] = {0x7fff, 0x00ff, 0x0007, 0x0003, 0x0003};
  static const int dbits[5] = {14, 7, 2, 1, 1}, dadd[5] = {269, 14, 7, 4, 1};
  static const int8_t ltab[10] = {9, 1, 0, -1, -1, 8, 4, 2, 1, 0};
  static const int8_t obits[4] = {11, 4, 7, 0};
  static const int16_t oadd[3] = {0x11f, -1, 0x1f};
  for (;;) {
    if (bit()) {   // octets recopiés tels quels
      uint32_t n = 0;
      if (bit()) {
        int k = 4;
        for (; k >= 0; k--) { n = bits(dbits[k]); if (n != dmask[k]) break; }
        if (k < 0) k = 0;
        n += uint32_t(dadd[k]);
      }
      for (uint32_t i = 0; i <= n; i++) put(uint8_t(rdByte()));
    }
    if (dst <= 0) break;
    // chaîne déjà décompressée : longueur puis distance
    int d2 = 3;
    while (d2 >= 0 && bit()) d2--;
    const int lb = ltab[1 + d2];
    uint32_t d1 = lb >= 0 ? bits(lb) : 0;
    int len = ltab[6 + d2] + int(d1);
    int32_t off;
    if (len == 0) {
      int nb = 5, add = -1;
      if (bit()) { nb = 8; add = 0x3f; }
      off = int32_t(bits(nb)) + add;
    } else {
      int k = 1;
      while (k >= 0 && bit()) k--;
      off = int32_t(bits(obits[1 + k])) + oadd[1 + k];
      if (off < 0) off -= len;
    }
    int64_t from = dst + 2 + len + off;
    for (int i = 0; i <= len + 1; i++) {
      if (from <= 0 || from > int64_t(ulen)) throw bad();
      put(out[size_t(--from)]);
    }
    if (dst <= 0) break;
  }
  return out;
}

namespace {
// programme du jeu dans un bloc de données : en-tête $601A et noms des circuits à leur place
// (« LITTLE RAMP » en $13498 une fois chargé en $10100)
bool findGame(const Bytes &d, Bytes &out) {
  const size_t names = 0x1c + (0x13498 - 0x10100);
  for (size_t i = 0; i + names + 11 <= d.size(); i++) {
    if (d[i] != 0x60 || d[i + 1] != 0x1a) continue;
    if (std::memcmp(&d[i + names], "LITTLE RAMP", 11) != 0) continue;
    out.assign(d.begin() + long(i), d.end());
    return true;
  }
  return false;
}
// tel quel, décompressé ByteKiller, décompressé Pack-Ice (deux variantes)
bool gameFrom(const Bytes &f, Bytes &out) {
  if (findGame(f, out)) return true;
  try { bool ok = false; Bytes u = unpackPrg(f, ok); if (ok && findGame(u, out)) return true; } catch (...) {}
  for (bool wide : {false, true}) {
    try { if (findGame(iceDepack(f, wide), out)) return true; } catch (...) {}
  }
  return false;
}
}  // namespace

// programme du jeu : fichier programme donné directement, ou cherché dans les fichiers de la
// disquette (GAME.PUT de la version d'origine, ou STUNTCAR.PRG, STUNT.PRG, STUNTCAR.EXE... des
// autres copies, compressés ByteKiller ou Pack-Ice, ou pas)
Bytes innerProgram(const Bytes &file) {
  Bytes out;
  if (file.size() > 2 && file[0] == 0x60 && file[1] == 0x1a) {
    if (gameFrom(file, out)) return out;
    return file;
  }
  auto files = fatList(file);
  std::vector<const Bytes *> order;   // GAME.PUT d'abord, puis les plus gros fichiers
  for (auto &kv : files) {
    std::string up;
    for (char ch : kv.first) up += char(toupper(static_cast<unsigned char>(ch)));
    if (up.size() >= 8 && up.compare(up.size() - 8, 8, "GAME.PUT") == 0) order.insert(order.begin(), &kv.second);
    else if (kv.second.size() > 100000) order.push_back(&kv.second);
  }
  for (const Bytes *f : order) if (gameFrom(*f, out)) return out;
  throw std::runtime_error("programme de Stunt Car Racer introuvable sur cette disquette");
}

Bytes relocate(const Bytes &prg, uint32_t base, uint32_t &bss) {
  const size_t tlen = be32(prg, 2), dlen = be32(prg, 6), slen = be32(prg, 14);
  bss = be32(prg, 10);
  need(prg, 0x1c, tlen + dlen);
  Bytes img(prg.begin() + 0x1c, prg.begin() + 0x1c + tlen + dlen);
  size_t r = 0x1c + tlen + dlen + slen;
  uint32_t off = be32(prg, r);
  r += 4;
  if (off) {
    size_t a = off;
    for (;;) {
      uint32_t v = be32(img, a) + base;   // be32 vérifie que a + 4 tient dans le programme
      img[a] = v >> 24; img[a + 1] = v >> 16; img[a + 2] = v >> 8; img[a + 3] = v;
      // fin du fichier = fin de la table (certaines copies du jeu ont perdu l'octet 0 final)
      uint8_t c = r < prg.size() ? prg[r++] : 0;
      if (c == 0) break;
      while (c == 1) { a += 254; c = r < prg.size() ? prg[r++] : 0; }
      a += c;
    }
  }
  return img;
}

Bytes readFile(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) throw std::runtime_error("impossible d'ouvrir " + path);
  Bytes b;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
  fclose(f);
  return b;
}

}  // namespace scr
