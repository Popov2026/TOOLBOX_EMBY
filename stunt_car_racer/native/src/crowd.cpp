// crowd.cpp — voir crowd.hpp
#include "crowd.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace scr {

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double LOOP_S = 8.0;          // durée de la boucle de foule
constexpr double SOUND_SPEED = 8600;    // vitesse du son en unités du monde par seconde (≈ 25 unités / m)
constexpr double NEAR = 1600;           // distance à laquelle le volume est divisé par 2
struct Rng {
  uint32_t s;
  uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  double uni() { return (next() >> 8) / double(1 << 24); }
  double noise() { return uni() * 2 - 1; }
};
}  // namespace

// boucle de foule : brouhaha (bruit filtré qui ondule), cris « ouais » (voix à formants qui montent
// puis retombent), applaudissements et quelques sifflets ; tout est replié modulo la longueur de la
// boucle, qui se raccorde donc sans coupure
bool CrowdSound::loadWav(const std::string &path) {
  wav_.clear(); wavRate_ = 0; loop_.clear(); rate_ = 0; voices_.clear();
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::vector<uint8_t> d;
  uint8_t buf[65536];
  size_t got;
  while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + got);
  std::fclose(f);
  auto u16 = [&](size_t o) { return unsigned(d[o] | d[o + 1] << 8); };
  auto u32 = [&](size_t o) { return uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | uint32_t(d[o + 3]) << 24); };
  if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return false;
  unsigned ch = 0, bits = 0, fmt = 0; uint32_t rate = 0;
  for (size_t o = 12; o + 8 <= d.size();) {
    const uint32_t len = u32(o + 4);
    const size_t body = o + 8;
    if (body + len > d.size() && std::memcmp(d.data() + o, "data", 4)) break;
    if (!std::memcmp(d.data() + o, "fmt ", 4) && len >= 16) { fmt = u16(body); ch = u16(body + 2); rate = u32(body + 4); bits = u16(body + 14); }
    else if (!std::memcmp(d.data() + o, "data", 4)) {
      if (fmt != 1 || bits != 16 || ch < 1 || rate < 4000) return false;
      const size_t frames = std::min<size_t>(len, d.size() - body) / (2 * ch);
      wav_.resize(frames);
      for (size_t i = 0; i < frames; i++) {
        double sum = 0;
        for (unsigned c = 0; c < ch; c++) sum += int16_t(u16(body + (i * ch + c) * 2)) / 32768.0;
        wav_[i] = float(sum / ch);
      }
      wavRate_ = int(rate);
      break;
    }
    o = body + len + (len & 1);
  }
  if (wav_.size() < 4000) { wav_.clear(); wavRate_ = 0; return false; }
  return true;
}

void CrowdSound::build(int rate) {
  rate_ = rate;
  if (!wav_.empty()) {   // son fourni : mis à la fréquence de sortie, raccord de la boucle en fondu
    const size_t L = size_t(double(wav_.size()) * rate / wavRate_);
    std::vector<double> b(L);
    for (size_t i = 0; i < L; i++) {
      const double p = double(i) * wavRate_ / rate;
      const size_t i0 = std::min(size_t(p), wav_.size() - 1), i1 = std::min(i0 + 1, wav_.size() - 1);
      b[i] = wav_[i0] + (wav_[i1] - wav_[i0]) * (p - double(i0));
    }
    const size_t X = std::min(L / 4, size_t(rate / 4));   // fondu enchaîné fin -> début sur 0,25 s
    std::vector<float> out(L - X);
    for (size_t i = 0; i < L - X; i++) out[i] = float(b[i]);
    for (size_t i = 0; i < X; i++) { const double t = double(i) / X; out[i] = float(b[i] * t + b[L - X + i] * (1 - t)); }
    double rms = 0;
    for (float x : out) rms += double(x) * x;
    rms = std::sqrt(rms / double(out.size())) + 1e-9;
    for (float &x : out) x = float(std::clamp(double(x) / rms * 0.22, -1.0, 1.0));   // même niveau que la foule synthétisée
    loop_ = std::move(out);
    return;
  }
  const size_t L = size_t(LOOP_S * rate);
  std::vector<double> b(L, 0.0);
  Rng r{0x5eed1234u};
  {   // brouhaha : bruit passe-bande (250 Hz - 2,5 kHz), amplitude modulée lentement
    const double ah = std::exp(-2 * PI * 250 / rate), al = 1 - std::exp(-2 * PI * 2500 / rate);
    double hp = 0, prev = 0, lp = 0;
    for (int pass = 0; pass < 2; pass++)   // 2 passes : filtres en régime établi au raccord de la boucle
      for (size_t i = 0; i < L; i++) {
        const double x = r.noise();
        hp = ah * (hp + x - prev); prev = x;
        lp += al * (hp - lp);
        const double t = double(i) / L;
        const double am = 0.75 + 0.15 * std::sin(2 * PI * 3 * t) + 0.1 * std::sin(2 * PI * 7 * t + 1.3);
        if (pass) b[i] += lp * am * 0.9;
      }
  }
  {   // cris : une période d'onde par voix (harmoniques pondérées par les formants de « a »), lue en boucle
    const int TAB = 2048, VOICES = 90;
    std::vector<double> tab(TAB);
    for (int v = 0; v < VOICES; v++) {
      const bool high = r.uni() < 0.45;
      const double f0 = high ? 300 + r.uni() * 220 : 170 + r.uni() * 120;
      const double F1 = 700 + r.uni() * 150, F2 = 1150 + r.uni() * 250;
      for (int k = 0; k < TAB; k++) tab[size_t(k)] = 0;
      for (int h = 1; h <= 16; h++) {
        const double fh = f0 * h;
        const double a = (std::exp(-std::pow((fh - F1) / 180, 2)) + 0.7 * std::exp(-std::pow((fh - F2) / 260, 2)) + 0.08) / h;
        const double ph = r.uni() * 2 * PI;
        for (int k = 0; k < TAB; k++) tab[size_t(k)] += a * std::sin(2 * PI * h * k / TAB + ph);
      }
      double mx = 1e-9;
      for (double x : tab) mx = std::max(mx, std::fabs(x));
      const double dur = 0.5 + r.uni() * 1.6, t0 = r.uni() * LOOP_S, amp = 0.25 + r.uni() * 0.35;
      const double vib = 4.5 + r.uni() * 2.5, rise = 0.12 + r.uni() * 0.18;
      const size_t n = size_t(dur * rate), s0 = size_t(t0 * rate);
      double ph = 0;
      for (size_t i = 0; i < n; i++) {
        const double t = double(i) / rate, u = t / dur;
        const double f = f0 * (1 + rise * std::sin(PI * std::min(1.0, u * 1.3))) * (1 + 0.012 * std::sin(2 * PI * vib * t));
        ph += f / rate * TAB;
        if (ph >= TAB) ph -= TAB;
        const double env = std::min(1.0, t / 0.07) * std::min(1.0, (dur - t) / 0.35);
        const size_t k = size_t(ph) % size_t(TAB);
        b[(s0 + i) % L] += (tab[k] / mx * 0.85 + r.noise() * 0.15) * env * amp * 0.5;
      }
    }
  }
  {   // applaudissements : claquements brefs (bruit qui s'éteint en quelques millisecondes), passe-haut
    std::vector<double> c(L, 0.0);
    const int claps = int(LOOP_S * 38);
    for (int k = 0; k < claps; k++) {
      const size_t s0 = size_t(r.uni() * L);
      const double amp = 0.3 + r.uni() * 0.7, tau = 0.002 + r.uni() * 0.003;
      for (size_t i = 0; i < size_t(0.02 * rate); i++) c[(s0 + i) % L] += r.noise() * amp * std::exp(-double(i) / rate / tau);
    }
    const double ah = std::exp(-2 * PI * 900 / rate);
    double hp = 0, prev = 0;
    for (int pass = 0; pass < 2; pass++)
      for (size_t i = 0; i < L; i++) { hp = ah * (hp + c[i] - prev); prev = c[i]; if (pass) b[i] += hp * 0.35; }
  }
  for (int w = 0; w < 7; w++) {   // sifflets
    const double f0 = 1900 + r.uni() * 1100, dur = 0.25 + r.uni() * 0.5, t0 = r.uni() * LOOP_S, glide = r.uni() < 0.5 ? 0.25 : -0.15;
    double ph = 0;
    for (size_t i = 0; i < size_t(dur * rate); i++) {
      const double t = double(i) / rate, u = t / dur;
      ph += 2 * PI * f0 * (1 + glide * u) / rate;
      const double env = std::min(1.0, t / 0.03) * std::min(1.0, (dur - t) / 0.06);
      b[(size_t(t0 * rate) + i) % L] += std::sin(ph) * env * 0.12;
    }
  }
  double rms = 0;
  for (double x : b) rms += x * x;
  rms = std::sqrt(rms / double(L)) + 1e-9;
  loop_.resize(L);
  for (size_t i = 0; i < L; i++) loop_[i] = float(std::tanh(b[i] / rms * 0.22));
}

void CrowdSound::mix(int16_t *pcm, size_t n, int rate, double volume, const std::vector<Source> &src,
                     double x, double z, double vx, double vz) {
  if (!n || rate <= 0 || volume <= 0) return;
  if (rate != rate_ || loop_.empty()) build(rate);
  if (voices_.size() != src.size()) {
    voices_.assign(src.size(), Voice{});
    for (size_t k = 0; k < voices_.size(); k++) voices_[k].pos = std::fmod(double(k) * 0.381966 * double(loop_.size()), double(loop_.size()));
  }
  const double L = double(loop_.size());
  for (size_t k = 0; k < src.size(); k++) {
    const Source &s = src[k];
    Voice &v = voices_[k];
    // point de la façade le plus proche de l'auditeur (la tribune est une source étendue)
    const double ax = -s.fz, az = s.fx;
    const double along = std::clamp((x - s.x) * ax + (z - s.z) * az, -s.halfLen, s.halfLen);
    const double px = s.x + ax * along, pz = s.z + az * along;
    const double d = std::max(1.0, std::hypot(px - x, pz - z));
    // Doppler (auditeur en mouvement, source immobile) : vers la source = plus aigu
    const double toward = (vx * (px - x) + vz * (pz - z)) / d;
    const double ratio = std::clamp((SOUND_SPEED + toward) / SOUND_SPEED, 0.6, 1.6);
    const double gain = 1 / (1 + (d / NEAR) * (d / NEAR));
    if (!v.init) { v.gain = gain; v.ratio = ratio; v.init = true; }
    const double lpa = 1 - std::exp(-2 * PI * std::min(0.45 * rate, 14000 / (1 + d / 1800)) / rate);   // loin : plus sourd
    if (gain < 0.002 && v.gain < 0.002) {   // inaudible : la lecture avance quand même
      v.pos = std::fmod(v.pos + ratio * double(n), L); v.gain = gain; v.ratio = ratio;
      continue;
    }
    // gain et hauteur interpolés sur la trame (pas de craquement quand ils changent)
    const double g0 = v.gain, r0 = v.ratio, dg = (gain - g0) / double(n), dr = (ratio - r0) / double(n);
    for (size_t i = 0; i < n; i++) {
      const double g = g0 + dg * double(i), rr = r0 + dr * double(i);
      const size_t i0 = size_t(v.pos), i1 = (i0 + 1) % loop_.size();
      const double fr = v.pos - double(i0);
      const double smp = loop_[i0] * (1 - fr) + loop_[i1] * fr;
      v.lp += lpa * (smp - v.lp);
      const double out = pcm[i] + v.lp * g * volume * 0.9 * 32767.0;
      pcm[i] = int16_t(std::clamp(out, -32768.0, 32767.0));
      v.pos += rr;
      if (v.pos >= L) v.pos -= L;
    }
    v.gain = gain; v.ratio = ratio;
  }
}

}  // namespace scr
