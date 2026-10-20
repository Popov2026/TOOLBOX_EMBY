// ym2149.hpp — puce sonore de l'Atari ST (Yamaha YM2149, horloge 2 MHz) : 3 voix carrées,
// générateur de bruit, enveloppe sur 32 pas, volumes logarithmiques. Sortie mono.
#pragma once
#include <cstdint>
#include <vector>

namespace scr {

class YM2149 {
 public:
  YM2149();
  void write(int reg, int value);
  // produit n échantillons à la fréquence rate (Hz), ajoutés à out (valeurs ~[-1, 1])
  void render(float *out, int n, int rate);
  // voix coupées (bit 0 : A, 1 : B, 2 : C) : le son moteur d'origine (voix B et C) remplacé par le V8
  void setMute(int mask) { mute_ = mask & 7; }

 private:
  void step();
  uint8_t r_[16] = {};
  // compteurs à 250 kHz (horloge / 8)
  int toneCnt_[3] = {}, toneOut_[3] = {};
  int noiseCnt_ = 0, noiseOut_ = 1;
  uint32_t lfsr_ = 1;
  int envCnt_ = 0, envStep_ = 0, envDir_ = 1;
  bool envHold_ = false;
  double frac_ = 0;
  float dc_ = 0;
  float amp_[32];
  int mute_ = 0;
};

}  // namespace scr
