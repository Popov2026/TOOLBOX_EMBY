// hdassets.cpp — voir hdassets.hpp
#include "hdassets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO_WARNINGS
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "third_party/stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace scr {
namespace fs = std::filesystem;

static bool loadImage(const fs::path &p, HdImage &img, bool &hasAlpha) {
  std::ifstream f(p, std::ios::binary);
  std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  int w, h, n;
  unsigned char *d = stbi_load_from_memory(buf.data(), int(buf.size()), &w, &h, &n, 4);
  if (!d) return false;
  img.w = w; img.h = h;
  img.px.resize(size_t(w) * h);
  hasAlpha = false;
  for (size_t i = 0; i < img.px.size(); i++) {
    unsigned char *q = d + i * 4;
    img.px[i] = (uint32_t(q[3]) << 24) | (uint32_t(q[0]) << 16) | (uint32_t(q[1]) << 8) | q[2];
    hasAlpha |= q[3] < 255;
  }
  stbi_image_free(d);
  return true;
}

int HdAssets::load(const std::string &dir) {
  sprites_.clear();
  report.clear();
  std::error_code ec;
  fs::path base = fs::u8path(dir);
  if (!fs::is_directory(base, ec)) return 0;
  // images : sprite_NN.png ou sprite_NN_K.png
  std::map<int, std::map<int, fs::path>> files;
  std::regex re(R"(sprite_(\d+)(?:_(\d+))?\.(png|jpg|jpeg))", std::regex::icase);
  for (auto &e : fs::directory_iterator(base, ec)) {
    std::smatch mm;
    std::string name = e.path().filename().u8string();
    if (std::regex_match(name, mm, re)) files[std::atoi(mm[1].str().c_str())][mm[2].matched ? std::atoi(mm[2].str().c_str()) : 0] = e.path();
  }
  std::map<int, bool> alpha;
  for (auto &[id, fr] : files) {
    HdSprite s;
    bool anyAlpha = false;
    for (auto &[k, p] : fr) {
      HdImage img;
      bool a;
      if (loadImage(p, img, a)) { s.frames.push_back(std::move(img)); anyAlpha |= a; }
      else report += "illisible : " + p.filename().u8string() + "\n";
    }
    if (s.frames.empty() || id < 0 || id > 255) continue;
    s.additive = !anyAlpha;
    sprites_[id] = std::move(s);
  }
  // réglages
  std::ifstream ini(base / "hd.ini");
  std::string line;
  int cur = -1;
  auto trim = [](std::string v) {
    auto c = v.find_first_of(";#");
    if (c != std::string::npos) v = v.substr(0, c);
    while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
    while (!v.empty() && std::isspace((unsigned char)v.front())) v.erase(v.begin());
    return v;
  };
  while (std::getline(ini, line)) {
    line = trim(line);
    if (line.empty()) continue;
    std::smatch mm;
    if (std::regex_match(line, mm, std::regex(R"(\[\s*sprite_(\d+)\s*\])", std::regex::icase))) { cur = std::atoi(mm[1].str().c_str()); continue; }
    auto eq = line.find('=');
    if (eq == std::string::npos || !sprites_.count(cur)) continue;
    std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    HdSprite &s = sprites_[cur];
    double d = std::atof(v.c_str());
    if (k == "blend") s.additive = v == "add";
    else if (k == "scale" && d > 0) s.scale = d;
    else if (k == "dx") s.dx = d;
    else if (k == "dy") s.dy = d;
    else if (k == "fps" && d > 0) s.fps = d;
    else if (k == "anchor") s.anchor = v == "top" ? 0 : v == "center" ? 2 : 1;
  }
  for (auto &[id, s] : sprites_)
    report += "sprite " + std::to_string(id) + " : " + std::to_string(s.frames.size()) + " image(s), " +
              (s.additive ? "add" : "alpha") + "\n";
  return int(sprites_.size());
}

void HdAssets::draw(const HdSprite &s, double t, double x0, double y0, double x1, double y1, uint32_t *out, int W, int H) {
  if (s.frames.empty() || x1 <= x0 || y1 <= y0) return;
  const HdImage &img = s.frames[size_t(std::fmod(std::max(0.0, t) * s.fps, double(s.frames.size())))];
  const double rw = x1 - x0, rh = y1 - y0;
  int xa = std::max(0, int(std::floor(x0))), xb = std::min(W, int(std::ceil(x1)));
  int ya = std::max(0, int(std::floor(y0))), yb = std::min(H, int(std::ceil(y1)));
  for (int y = ya; y < yb; y++) {
    double v = (y + 0.5 - y0) / rh * img.h - 0.5;
    if (v < -0.5 || v > img.h - 0.5) continue;
    int v0 = std::clamp(int(std::floor(v)), 0, img.h - 1), v1 = std::min(v0 + 1, img.h - 1);
    double fv = std::clamp(v - v0, 0.0, 1.0);
    uint32_t *o = out + size_t(y) * W;
    for (int x = xa; x < xb; x++) {
      double u = (x + 0.5 - x0) / rw * img.w - 0.5;
      if (u < -0.5 || u > img.w - 0.5) continue;
      int u0 = std::clamp(int(std::floor(u)), 0, img.w - 1), u1 = std::min(u0 + 1, img.w - 1);
      double fu = std::clamp(u - u0, 0.0, 1.0);
      // filtrage bilinéaire (canaux pondérés par l'alpha)
      double acc[4] = {0, 0, 0, 0};
      const uint32_t q[4] = {img.px[size_t(v0) * img.w + u0], img.px[size_t(v0) * img.w + u1], img.px[size_t(v1) * img.w + u0],
                             img.px[size_t(v1) * img.w + u1]};
      const double wq[4] = {(1 - fu) * (1 - fv), fu * (1 - fv), (1 - fu) * fv, fu * fv};
      for (int k = 0; k < 4; k++) {
        double a = (q[k] >> 24) / 255.0 * wq[k];
        acc[0] += a;
        acc[1] += ((q[k] >> 16) & 255) * a; acc[2] += ((q[k] >> 8) & 255) * a; acc[3] += (q[k] & 255) * a;
      }
      if (acc[0] <= 0) continue;
      double r = acc[1] / acc[0], g = acc[2] / acc[0], b = acc[3] / acc[0], a = acc[0];
      uint32_t c = o[x];
      double R = (c >> 16) & 255, G = (c >> 8) & 255, B = c & 255;
      if (s.additive) { R += r * a; G += g * a; B += b * a; }
      else { R += (r - R) * a; G += (g - G) * a; B += (b - B) * a; }
      o[x] = 0xff000000u | (uint32_t(std::min(255.0, R)) << 16) | (uint32_t(std::min(255.0, G)) << 8) | uint32_t(std::min(255.0, B));
    }
  }
}

}  // namespace scr

namespace scr {

namespace {
// bruit de valeur lissé, périodique en x (pas de couture), 4 octaves
struct Noise {
  std::vector<float> g;
  int n;
  explicit Noise(int seed, int size = 64) : g(size_t(size) * size), n(size) {
    uint32_t r = uint32_t(seed) * 2654435761u + 12345;
    for (auto &v : g) { r = r * 1664525u + 1013904223u; v = float((r >> 8) & 0xffff) / 65535.f; }
  }
  float at(float x, float y) const {
    int xi = int(std::floor(x)), yi = int(std::floor(y));
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    auto G = [&](int a, int b) { return g[size_t(((b % n) + n) % n) * n + ((a % n) + n) % n]; };
    float a = G(xi, yi), b = G(xi + 1, yi), c = G(xi, yi + 1), d = G(xi + 1, yi + 1);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
  }
  float fbm(float x, float y) const {
    float s = 0, amp = 0.5f, f = 1;
    for (int o = 0; o < 4; o++, amp *= 0.5f, f *= 2) s += at(x * f, y * f) * amp;
    return s / 0.9375f;
  }
};

void blur(std::vector<float> &a, int w, int h, int r) {   // flou boîte x3 ≈ gaussien
  std::vector<float> t(a.size());
  for (int pass = 0; pass < 3; pass++) {
    for (int y = 0; y < h; y++) {
      float acc = 0; int cnt = 0;
      for (int x = -r; x < w + r; x++) {
        if (x + r < w) { acc += a[size_t(y) * w + x + r]; cnt++; }
        if (x - r - 1 >= 0) { acc -= a[size_t(y) * w + x - r - 1]; cnt--; }
        if (x >= 0 && x < w) t[size_t(y) * w + x] = acc / float(2 * r + 1);
      }
    }
    for (int x = 0; x < w; x++) {
      float acc = 0;
      for (int y = -r; y < h + r; y++) {
        if (y + r < h) acc += t[size_t(y + r) * w + x];
        if (y - r - 1 >= 0) acc -= t[size_t(y - r - 1) * w + x];
        if (y >= 0 && y < h) a[size_t(y) * w + x] = acc / float(2 * r + 1);
      }
    }
  }
}
}  // namespace

HdSprite makeFlame(const std::vector<int> &px, int sw, int sh, int seed) {
  const int F = 6;                       // agrandissement
  const int ml = sw / 4, mr = sw / 4, mt = sh * 3 / 4, mb = sh / 6;   // marges (pixels d'origine) : la flamme déborde
  const int W = (sw + ml + mr) * F, H = (sh + mt + mb) * F;
  // chaleur de base : blanc = cœur, jaune = flamme, sur la grille agrandie
  std::vector<float> heat(size_t(W) * H, 0.f);
  for (int y = 0; y < sh; y++)
    for (int x = 0; x < sw; x++) {
      int c = px[size_t(y) * sw + x];
      float v = c == 15 ? 1.0f : c == 3 ? 0.72f : c >= 0 ? 0.35f : 0.f;
      if (v <= 0) continue;
      for (int yy = 0; yy < F; yy++)
        for (int xx = 0; xx < F; xx++) heat[size_t((y + mt) * F + yy) * W + (x + ml) * F + xx] = v;
    }
  std::vector<float> core = heat, glow = heat;
  blur(core, W, H, F);                   // contours adoucis
  blur(glow, W, H, F * 3);               // halo
  Noise nz(seed);
  HdSprite s;
  s.additive = true;
  s.fps = 24;
  s.padL = double(ml) / sw; s.padR = double(mr) / sw; s.padT = double(mt) / sh; s.padB = double(mb) / sh;
  const int frames = 10;
  for (int k = 0; k < frames; k++) {
    HdImage img;
    img.w = W; img.h = H;
    img.px.assign(size_t(W) * H, 0xff000000u);
    float t = float(k) / frames;         // boucle : le bruit défile d'une période complète
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        float fx = float(x) / W * 4, fy = float(y) / H * 2;
        // langues de feu qui montent : la chaleur est prise plus bas (d'autant plus que le bruit
        // est fort), ondule latéralement, et se découpe en mèches
        float n1 = nz.fbm(fx * 2.0f, fy * 2.0f + t * 16.0f);
        float n2 = nz.fbm(fx * 3.1f + 7.3f, fy * 3.0f + t * 32.0f);
        float n3 = nz.fbm(fx * 6.0f + 3.1f, fy * 1.5f + t * 48.0f);
        int sx = std::clamp(int(x + (n2 - 0.5f) * F * 4), 0, W - 1);
        int sy = std::clamp(int(y + F * 1.0f + n1 * n1 * F * 9), 0, H - 1);
        float c = core[size_t(sy) * W + sx], g = glow[size_t(y) * W + x];
        float tongues = std::clamp(0.35f + 1.3f * n3, 0.f, 1.3f);
        float h = c * tongues * (0.6f + 0.8f * n1) * 1.15f + g * 0.22f;
        h = std::clamp(h, 0.f, 1.3f);
        // rampe du feu : rouge sombre -> orange -> jaune -> blanc
        float r = std::clamp(h * 2.0f, 0.f, 1.f), gg = std::clamp(h * 2.0f - 0.8f, 0.f, 1.f),
              b = std::clamp(h * 2.4f - 2.2f, 0.f, 1.f);
        float a = std::clamp(h * 1.4f, 0.f, 1.f);
        img.px[size_t(y) * W + x] = 0xff000000u | (uint32_t(r * a * 255) << 16) | (uint32_t(gg * a * 255) << 8) | uint32_t(b * a * 255);
      }
    s.frames.push_back(std::move(img));
  }
  return s;
}

}  // namespace scr
