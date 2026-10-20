// sound.cpp — voir sound.hpp
#include "sound.hpp"

#include <algorithm>
#include <cmath>

namespace scr {

namespace {
constexpr double PI = 3.14159265358979323846;
}

void EngineV8::Reso::set(double f, double q, double rate) {
  const double r = std::exp(-PI * f / (q * rate));
  a1 = 2 * r * std::cos(2 * PI * f / rate); a2 = r * r; g = 1 - r;
}
double EngineV8::Reso::run(double x) {   // passe-bande résonant (zéros au continu et à Nyquist)
  const double y = g * (x - x2) + a1 * y1 - a2 * y2;
  x2 = x1; x1 = x; y2 = y1; y1 = y;
  return y;
}

void EngineV8::mix(int16_t *pcm, size_t n, int rate, double gameRev, bool throttle, bool boost, double volume) {
  if (!n || rate <= 0 || volume <= 0) return;
  if (rate != rate_) {
    rate_ = rate;
    r_[0].set(105, 2.2, rate); r_[1].set(310, 3.0, rate); r_[2].set(880, 3.5, rate); r_[3].set(2300, 2.0, rate);
  }
  const double dt = double(n) / rate;
  // régime : vitesse du jeu -> boîte à 4 rapports (montée et descente avec hystérésis)
  const double s = std::max(0.0, (gameRev - 270) / 2280);
  static const double UP[3] = {0.22, 0.45, 0.70}, RATIO[4] = {26000, 14000, 9500, 7200};
  if (gear_ < 3 && s > UP[gear_]) { gear_++; shift_ = 0.12; }
  if (gear_ > 0 && s < UP[gear_ - 1] - 0.04) gear_--;
  double target = std::max(850.0, s * RATIO[gear_]);
  if (s > 1.08) target = std::min(7600.0, 6200 + (s - 1.08) * 3000);   // roues en l'air : le moteur s'emballe
  if (shift_ > 0) { shift_ -= dt; target *= 0.97; }                    // passage de rapport : coupure brève
  target = std::min(target, 7600.0);
  rpm_ += (target - rpm_) * std::min(1.0, dt * (shift_ > 0 ? 14 : 7));
  load_ += ((throttle ? 1.0 : 0.15) + (boost ? 0.35 : 0) - load_) * std::min(1.0, dt * 6);
  // ordre d'allumage du V8 croisé : intensité inégale d'une explosion à l'autre (deux rangées)
  static const double PAT[8] = {1.0, 0.78, 0.93, 1.06, 0.82, 0.97, 0.74, 0.9};
  const double fire = rpm_ / 60 * 4;   // explosions par seconde
  const double drive = 1.0 + 1.2 * load_;
  const double low = 1.0, mid = 0.65, high = 0.25 + 0.35 * load_, rasp = 0.08 + 0.22 * load_;
  const double decay = std::exp(-1.0 / (0.0007 * rate)), ndecay = std::exp(-1.0 / (0.0022 * rate));
  const double lpa = 1 - std::exp(-2 * PI * 7000 / rate);
  const double gain = volume * (0.35 + 0.25 * load_) * 3.2;
  for (size_t i = 0; i < n; i++) {
    phase_ += fire / rate;
    if (phase_ >= 1) {   // explosion : impulsion (avec un peu d'irrégularité) et souffle
      phase_ -= 1;
      rng_ = rng_ * 1664525u + 1013904223u;
      const double jit = 0.92 + 0.16 * ((rng_ >> 8) & 0xffff) / 65535.0;
      excite_ += PAT[cyl_] * jit;
      noise_ += 0.35 * PAT[cyl_] * jit;
      cyl_ = (cyl_ + 1) & 7;
      // petites variations du temps entre deux explosions (moteur « vivant » au ralenti)
      phase_ += (((rng_ >> 20) & 255) / 255.0 - 0.5) * 0.06 * std::max(0.0, 1 - rpm_ / 3000);
    }
    rng_ = rng_ * 1664525u + 1013904223u;
    const double wn = ((rng_ >> 9) & 0xffff) / 32768.0 - 1;
    const double x = excite_ + noise_ * wn;
    excite_ *= decay; noise_ *= ndecay;
    double y = (r_[0].run(x) * low + r_[1].run(x) * mid * 0.8 + r_[2].run(x) * high * 0.7 + r_[3].run(x) * rasp * 0.7 + x * 0.04) * 0.3;
    y = std::tanh(y * drive);
    hp_ = 0.996 * (hp_ + y - hpIn_); hpIn_ = y;   // retire la composante continue
    lp_ += lpa * (hp_ - lp_);
    const double out = pcm[i] + lp_ * gain * 32767.0;
    pcm[i] = int16_t(std::clamp(out, -32768.0, 32767.0));
  }
}

void Horn::mix(int16_t *pcm, size_t n, int rate, double volume) {
  if (voices_.empty() || rate <= 0) return;
  const double lpa = 1 - std::exp(-2 * PI * 2600 / rate);
  for (size_t i = 0; i < n; i++) {
    double g = 0;
    for (auto &v : voices_) {
      const double t = v.t, e = std::min(1.0, t / 0.015) * std::clamp((v.len - t) / 0.06, 0.0, 1.0);
      g += v.gain * e;
      v.t += 1.0 / rate;
    }
    if (g <= 0) continue;
    // deux trompes : ondes carrées adoucies, légèrement désaccordées
    ph1_ += 420.0 / rate; ph2_ += 525.0 / rate;
    ph1_ -= std::floor(ph1_); ph2_ -= std::floor(ph2_);
    const double s1 = ph1_ < 0.5 ? 1 : -1, s2 = ph2_ < 0.5 ? 1 : -1;
    lp1_ += lpa * (s1 - lp1_); lp2_ += lpa * (s2 - lp2_);
    const double y = std::tanh((lp1_ + lp2_) * 0.9) * std::min(1.2, g);
    pcm[i] = int16_t(std::clamp(pcm[i] + y * volume * 0.3 * 32767.0, -32768.0, 32767.0));
  }
  voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice &v) { return v.t >= v.len; }), voices_.end());
}

}  // namespace scr
