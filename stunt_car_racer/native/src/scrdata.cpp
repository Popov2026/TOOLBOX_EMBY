// scrdata.cpp — voir scrdata.hpp (port de tools/scr_tool.py, vérifié sur la même image).
#include "scrdata.hpp"

#include <cctype>
#include <cstdio>
#include <functional>

namespace scr {

static uint16_t le16(const Bytes &b, size_t o) { return b[o] | (b[o + 1] << 8); }
static uint32_t be32(const Bytes &b, size_t o) {
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
    unsigned v = img[o] | (img[o + 1] << 8);
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
  std::function<void(const Bytes &, const std::string &)> walk;
  walk = [&](const Bytes &buf, const std::string &path) {
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
      if (attr & 0x10) walk(chain(cl), path + name + "/");
      else {
        Bytes d = chain(cl);
        if (d.size() > size) d.resize(size);
        files[path + name] = d;
      }
    }
  };
  walk(Bytes(img.begin() + root, img.begin() + data), "");
  return files;
}

Bytes byteKiller(const Bytes &buf, size_t end, bool &ok) {
  size_t p = end;
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

Bytes innerProgram(const Bytes &file) {
  bool ok = false;
  if (file.size() > 2 && file[0] == 0x60 && file[1] == 0x1a) {
    try {
      Bytes out = unpackPrg(file, ok);
      if (ok) return innerFromUnpacked(out);
    } catch (...) {}
    return file;  // déjà le programme interne
  }
  auto files = fatList(file);
  for (auto &kv : files) {
    const std::string &k = kv.first;
    if (k.size() >= 8) {
      std::string up;
      for (char ch : k) up += char(toupper(ch));
      if (up.size() >= 8 && up.compare(up.size() - 8, 8, "GAME.PUT") == 0) {
        Bytes out = unpackPrg(kv.second, ok);
        if (!ok) throw std::runtime_error("GAME.PUT : décompression invalide");
        return innerFromUnpacked(out);
      }
    }
  }
  throw std::runtime_error("GAME.PUT introuvable sur cette disquette");
}

Bytes relocate(const Bytes &prg, uint32_t base, uint32_t &bss) {
  uint32_t tlen = be32(prg, 2), dlen = be32(prg, 6), slen = be32(prg, 14);
  bss = be32(prg, 10);
  Bytes img(prg.begin() + 0x1c, prg.begin() + 0x1c + tlen + dlen);
  size_t r = 0x1c + tlen + dlen + slen;
  uint32_t off = be32(prg, r);
  r += 4;
  if (off) {
    uint32_t a = off;
    for (;;) {
      uint32_t v = be32(img, a) + base;
      img[a] = v >> 24; img[a + 1] = v >> 16; img[a + 2] = v >> 8; img[a + 3] = v;
      uint8_t c = prg[r++];
      if (c == 0) break;
      while (c == 1) { a += 254; c = prg[r++]; }
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
