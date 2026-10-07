// ym2149.cpp — voir ym2149.hpp
#include "ym2149.hpp"

#include <cmath>

namespace scr {

static constexpr double STEP_RATE = 2000000.0 / 8;   // fréquence des compteurs internes

YM2149::YM2149() {
  // ~1,5 dB par pas d'enveloppe (32 pas) ; volume fixe v = pas 2v+1
  amp_[0] = 0;
  for (int i = 1; i < 32; i++) amp_[i] = float(std::pow(10.0, -(31 - i) * 1.5 / 20.0));
  r_[7] = 0xff;
}

void YM2149::write(int reg, int v) {
  reg &= 15;
  static const uint8_t mask[16] = {0xff, 0x0f, 0xff, 0x0f, 0xff, 0x0f, 0x1f, 0xff, 0x1f, 0x1f, 0x1f, 0xff, 0xff, 0x0f, 0xff, 0xff};
  r_[reg] = uint8_t(v & mask[reg]);
  if (reg == 13) {   // nouvelle forme d'enveloppe : redémarre
    bool att = r_[13] & 4;
    envStep_ = att ? 0 : 31; envDir_ = att ? 1 : -1; envHold_ = false; envCnt_ = 0;
  }
}

void YM2149::step() {
  for (int c = 0; c < 3; c++) {
    int tp = r_[c * 2] | (r_[c * 2 + 1] << 8);
    if (tp < 1) tp = 1;
    if (++toneCnt_[c] >= tp) { toneCnt_[c] = 0; toneOut_[c] ^= 1; }
  }
  int np = r_[6] ? r_[6] : 1;
  if (++noiseCnt_ >= np * 2) {   // bruit cadencé à horloge / 16
    noiseCnt_ = 0;
    uint32_t bit = (lfsr_ ^ (lfsr_ >> 3)) & 1;
    lfsr_ = (lfsr_ >> 1) | (bit << 16);
    noiseOut_ = lfsr_ & 1;
  }
  int ep = r_[11] | (r_[12] << 8);
  if (ep < 1) ep = 1;
  if (!envHold_ && ++envCnt_ >= ep) {
    envCnt_ = 0;
    envStep_ += envDir_;
    if (envStep_ < 0 || envStep_ > 31) {   // fin d'un cycle
      bool cont = r_[13] & 8, att = r_[13] & 4, alt = r_[13] & 2, hold = r_[13] & 1;
      if (!cont) { envStep_ = 0; envHold_ = true; }
      else if (hold) { envHold_ = true; envStep_ = (att != alt) ? 31 : 0; }
      else if (alt) { envDir_ = -envDir_; envStep_ = envDir_ > 0 ? 0 : 31; }
      else envStep_ = envDir_ > 0 ? 0 : 31;
    }
  }
}

void YM2149::render(float *out, int n, int rate) {
  const double per = STEP_RATE / rate;
  for (int i = 0; i < n; i++) {
    // moyenne des pas internes compris dans l'échantillon (filtre anti-repliement simple)
    frac_ += per;
    int k = int(frac_);
    frac_ -= k;
    float acc = 0;
    for (int s = 0; s < k; s++) {
      step();
      float v = 0;
      for (int c = 0; c < 3; c++) {
        bool toneOn = !(r_[7] & (1 << c)), noiseOn = !(r_[7] & (8 << c));
        bool on = (toneOut_[c] || !toneOn) && (noiseOut_ || !noiseOn);
        if (!on) continue;
        int vol = r_[8 + c];
        v += (vol & 0x10) ? amp_[envStep_] : amp_[(vol & 15) ? (vol & 15) * 2 + 1 : 0];
      }
      acc += v;
    }
    float smp = k ? acc / k / 3.0f : 0;
    dc_ += (smp - dc_) * 0.0015f;   // filtre passe-haut : retire la composante continue
    out[i] += (smp - dc_) * 0.9f;
  }
}

}  // namespace scr
