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
  // régime : suit directement celui du jeu (pas de boîte de vitesses) ; roues en l'air : il s'emballe
  const double s = std::max(0.0, (gameRev - 270) / 2280);
  double target = 750 + std::min(s, 1.0) * 6200;
  if (s > 1.08) target = std::min(7500.0, 6950 + (s - 1.08) * 2000);
  rpm_ += (target - rpm_) * std::min(1.0, dt * 6);
  load_ += ((throttle ? 1.0 : 0.15) + (boost ? 0.35 : 0) - load_) * std::min(1.0, dt * 6);
  // ralenti d'un V8 à gros arbre à cames : régime qui « pompe », explosions très inégales (le
  // « bloub-bloub »), ralenti grave ; l'effet s'efface en montant dans les tours
  const double idle = std::clamp(1 - (rpm_ - 750) / 1300, 0.0, 1.0);
  t_ += dt;
  const double rpmNow = rpm_ + idle * 55 * std::sin(2 * PI * 1.3 * t_) + idle * 25 * std::sin(2 * PI * 0.47 * t_ + 1);
  r_[0].set(78 + (rpm_ - 750) * 0.012, 2.4, rate);
  static const double PAT[8] = {1.0, 0.78, 0.93, 1.06, 0.82, 0.97, 0.74, 0.9};    // V8 croisé, en régime
  static const double LOPE[8] = {1.3, 0.45, 1.15, 0.9, 1.35, 0.5, 0.8, 1.05};    // ralenti « came »
  const double fire = rpmNow / 60 * 4;   // explosions par seconde
  const double drive = 1.0 + 1.2 * load_ + 0.5 * idle;
  const double low = 1.0 + 0.6 * idle, mid = 0.65, high = (0.25 + 0.35 * load_) * (1 - 0.5 * idle), rasp = (0.08 + 0.22 * load_) * (1 - 0.6 * idle);
  const double decay = std::exp(-1.0 / (0.0007 * rate)), ndecay = std::exp(-1.0 / (0.0022 * rate));
  const double lpa = 1 - std::exp(-2 * PI * 7000 / rate);
  const double gain = volume * (0.35 + 0.25 * load_) * 3.2;
  for (size_t i = 0; i < n; i++) {
    phase_ += fire / rate;
    if (phase_ >= 1) {   // explosion : impulsion (avec un peu d'irrégularité) et souffle
      phase_ -= 1;
      rng_ = rng_ * 1664525u + 1013904223u;
      const double jit = 0.92 + 0.16 * ((rng_ >> 8) & 0xffff) / 65535.0;
      const double amp = PAT[cyl_] * (1 - idle) + LOPE[cyl_] * idle;
      excite_ += amp * jit;
      noise_ += 0.35 * amp * jit;
      cyl_ = (cyl_ + 1) & 7;
      // petites variations du temps entre deux explosions (moteur « vivant » au ralenti)
      phase_ += (((rng_ >> 20) & 255) / 255.0 - 0.5) * (0.04 + 0.16 * idle);
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
