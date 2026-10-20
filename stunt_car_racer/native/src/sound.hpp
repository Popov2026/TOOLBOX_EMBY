// sound.hpp — sons ajoutés au jeu d'origine : moteur V8 réaliste (à la place du son moteur de la
// puce YM2149) et klaxon (course à plusieurs, Computer Link)
#pragma once
#include <cstdint>
#include <vector>

namespace scr {

// V8 à vilebrequin croisé : 4 explosions par tour, écart irrégulier entre les deux rangées de
// cylindres (le « glouglou » du V8), ralenti « gros arbre à cames », échappements résonants ; régime
// pris directement sur celui du jeu ($10A1E : environ 270 au ralenti, 2 500 à pleine vitesse, au-delà
// roues en l'air), sans boîte de vitesses
class EngineV8 {
public:
  // ajoute le moteur à une trame (mono, 16 bits) ; gameRev = $10A1E, throttle = accélérateur appuyé
  void mix(int16_t *pcm, size_t n, int rate, double gameRev, bool throttle, bool boost, double volume);
  void reset() { rpm_ = 750; }

private:
  struct Reso { double a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0, g = 0; void set(double f, double q, double rate); double run(double x); };
  double rpm_ = 750, load_ = 0, phase_ = 0, t_ = 0;
  int cyl_ = 0, rate_ = 0;
  uint32_t rng_ = 0x2468ace1u;
  double excite_ = 0, noise_ = 0, lp_ = 0, hp_ = 0, hpIn_ = 0;
  Reso r_[4];
};

// klaxon deux tons (environ 420 et 525 Hz), plusieurs à la fois (le sien et ceux des autres)
class Horn {
public:
  void trigger(double gain, double seconds = 0.55) { voices_.push_back({0, seconds, gain}); }
  void mix(int16_t *pcm, size_t n, int rate, double volume);
  bool active() const { return !voices_.empty(); }

private:
  struct Voice { double t, len, gain; };
  std::vector<Voice> voices_;
  double ph1_ = 0, ph2_ = 0, lp1_ = 0, lp2_ = 0;
};

}  // namespace scr
