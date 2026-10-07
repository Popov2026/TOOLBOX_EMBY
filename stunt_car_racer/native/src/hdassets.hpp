// hdassets.hpp — images de remplacement du mode HD (dossier hd/ à côté de scr.exe).
//
//   hd/sprite_06.png            remplace le sprite n° 6 (numéros : voir la planche des sprites)
//   hd/sprite_06_1.png, _2 ...  plusieurs images = animation (cadence : fps)
//   hd/hd.ini                   réglages facultatifs par sprite :
//       [sprite_06]
//       blend = add      ; add (feu, lumière sur fond noir) ou alpha (PNG transparent)
//       scale = 1.4      ; taille par rapport à l'emplacement d'origine
//       anchor = bottom  ; point fixe pour scale : bottom, center ou top
//       dx = 0           ; décalage en pixels de l'écran d'origine (320x200)
//       dy = -4
//       fps = 15
//   Sans réglage : alpha si l'image a de la transparence, sinon add (image sur fond noir).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace scr {

struct HdImage {
  int w = 0, h = 0;
  std::vector<uint32_t> px;   // ARGB, alpha non prémultiplié
};

struct HdSprite {
  std::vector<HdImage> frames;
  bool additive = false;
  double scale = 1, dx = 0, dy = 0, fps = 15;
  int anchor = 1;            // 0 haut, 1 bas, 2 centre
};

class HdAssets {
 public:
  // charge le dossier (absent : rien à remplacer) ; renvoie le nombre de sprites chargés
  int load(const std::string &dir);
  const HdSprite *sprite(int id) const {
    auto it = sprites_.find(id);
    return it == sprites_.end() ? nullptr : &it->second;
  }
  const std::map<int, HdSprite> &sprites() const { return sprites_; }
  std::string report;        // résumé du chargement (fichiers, erreurs)

  // dessine l'image (instant t en secondes) dans le rectangle [x0,x1)x[y0,y1) de out (W×H)
  static void draw(const HdSprite &s, double t, double x0, double y0, double x1, double y1, uint32_t *out, int W, int H);

 private:
  std::map<int, HdSprite> sprites_;
};

}  // namespace scr
