// crowd.hpp — encouragements de la foule des tribunes (mode HD, décor activé) : bruit de foule
// synthétisé (brouhaha, cris, applaudissements), un par tribune, atténué avec la distance et
// décalé en hauteur par l'effet Doppler (plus aigu en approchant, plus grave en s'éloignant)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace scr {

class CrowdSound {
public:
  struct Source { double x, z, halfLen, fx, fz; };   // tribune : milieu de la façade, demi-longueur, axe

  // ajoute la foule à une trame de son (mono, 16 bits) ; auditeur en (x, z), vitesse en unités / s
  void mix(int16_t *pcm, size_t n, int rate, double volume, const std::vector<Source> &src,
           double x, double z, double vx, double vz);
  void reset() { voices_.clear(); }
  // son de foule fourni (hd/foule.wav : WAV PCM 16 bits, mono ou stéréo, joué en boucle) ;
  // sans fichier, la foule est synthétisée
  bool loadWav(const std::string &path);

private:
  struct Voice { double pos = 0, gain = 0, ratio = 1, lp = 0; bool init = false; };
  void build(int rate);
  std::vector<float> wav_; int wavRate_ = 0;   // son fourni (avant mise à la fréquence de sortie)
  std::vector<float> loop_;   // boucle de foule (quelques secondes, sans raccord audible)
  int rate_ = 0;
  std::vector<Voice> voices_;
};

}  // namespace scr
