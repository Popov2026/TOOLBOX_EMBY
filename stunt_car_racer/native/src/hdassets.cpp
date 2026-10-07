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
