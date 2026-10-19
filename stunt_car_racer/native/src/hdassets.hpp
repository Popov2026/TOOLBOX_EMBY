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
//       wind = 1         ; couché par le vent selon la vitesse (flammes : 1 par défaut)
//       pad_top = 0.3    ; l'image déborde de l'emplacement (fraction de sa hauteur / largeur) :
//       pad_bottom = 0   ;   pad_top, pad_bottom, pad_left, pad_right
//       layer = 1        ; ordre de dessin : plus grand = par-dessus (moteur 1, flammes 2, autres 0)
//   hd/cockpit.png              cockpit fixe (cadre, tableau de bord) aux proportions de l'écran 320x200,
//                               fenêtre et vitres transparentes ; afficheurs et compteur sans chiffres
//   hd/cockpit_avant.png        partie du cockpit dessinée par-dessus les roues (base du moteur...),
//                               mêmes proportions ; sous le moteur (sprite 10) et les flammes
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
  // marge de l'image autour de l'emplacement d'origine (fraction de sa largeur / hauteur)
  double padL = 0, padT = 0, padR = 0, padB = 0;
  double wind = -1;          // sensibilité au vent de la course (-1 : 1 pour les flammes, 0 sinon)
  int layer = 0;             // ordre de dessin (plus grand : par-dessus)
  int windLevels = 1;        // > 1 : frames rangées par niveau de vent (flammes calculées), sans cisaillement
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
  HdImage cockpit;           // cockpit fixe (vide : celui du jeu)
  HdImage cockpitFront;      // partie du cockpit au-dessus des roues (facultatif)

  // dessine l'image (instant t en secondes) dans le rectangle [x0,x1)x[y0,y1) de out (W×H)
  // shear : décalage horizontal du haut de l'image, en fraction de la largeur (0 = droit)
  // wind : niveau de vent 0..1 (choix des images si windLevels > 1)
  static void draw(const HdSprite &s, double t, double x0, double y0, double x1, double y1, uint32_t *out, int W, int H,
                   double shear = 0, double wind = 0);

 private:
  std::map<int, HdSprite> sprites_;
};

}  // namespace scr
