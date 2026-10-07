// hdview.hpp — mode HD (touche F1) : la scène 3D est redessinée en haute définition et en 16/9
// à partir de l'état du jeu d'origine (voiture, circuit), interpolée entre deux ticks de la
// logique (8,33 Hz) pour un affichage fluide ; le cockpit d'origine est incrusté par-dessus.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>

#include "hdassets.hpp"
#include "machine.hpp"
#include "render3d.hpp"
#include "track.hpp"

namespace scr {

struct Pose { double x, y, z, yaw, pitch, roll; };

// voiture adverse : position (unités de géométrie, hauteur brute) et repère (droite, haut, avant)
struct OppPose {
  bool valid = false;
  double x = 0, yRaw = 0, z = 0;
  Vec3 right{1, 0, 0}, fwd{0, 0, 1};
};

// réglages de la caméra, exprimés en pixels de l'écran d'origine (320×200)
struct HdParams {
  double focal = 259, cx = 153.2, cy = 82.3;   // projection d'origine (calée sur le jeu)
  double aspect = 0.895;                       // focale verticale / horizontale
  double eyeSide = -27, pitchOffset = -0.0056, yawOffset = 0;    // décalage latéral de l'œil, inclinaison de la caméra (rad)
  double hScale = 0.261;                    // hauteur brute -> unités de géométrie
  double eyeUp = 40, eyeFwd = -27;          // œil par rapport à la position de la voiture (unités de géométrie)
  int pitchSign = 1, rollSign = 1;
  bool cockpit = true;                     // incruster le cockpit d'origine
  double interpDelay = 1.0;                // retard d'affichage en ticks (interpolation)
  double minClearance = 18;                // l'œil reste au moins à cette hauteur au-dessus de la route
  bool builtinFlames = true;               // flammes du boost : sprites d'origine agrandis en HD
  bool flameImages = false;                // utiliser les images de flammes du dossier hd/ (sinon ignorées)
  bool opponent3D = true;                  // voiture adverse redessinée en 3D (sinon : pixels d'origine)
};

class HdView {
 public:
  explicit HdView(const Machine &m);
  HdParams params;
  int clampCount = 0;
  HdAssets assets;
  // charge les images de remplacement (dossier hd/) et demande à la machine de suivre ces sprites
  int loadAssets(Machine &m, const std::string &dir);   // nombre d'images où l'œil a été remonté au-dessus de la route

  // à appeler après chaque trame émulée
  void afterFrame(const Machine &m);
  // vrai si une course est en cours (physique active récemment)
  bool racing() const;
  double now() const { return frame_; }    // trames émulées depuis le début
  double delay() const { return params.interpDelay * tickPeriod_; }
  // horloge de lecture : toujours croissante, rattrape en douceur l'instant visé (t - retard)
  double playTime(double t);
  // image W×H ; t = instant d'affichage en trames émulées (avec fraction)
  void render(const Machine &m, double t, uint32_t *out, int W, int H);
  // pose interpolée à l'instant t (trames)
  Pose poseAt(double t) const;
  static Pose poseFromState(const uint8_t *st);   // 18 octets à partir de $10AC2
  // rendu de la seule scène, avec une projection donnée en pixels de sortie
  void renderScene(const Pose &p, int track, uint32_t *out, int W, int H, double focal, double cx, double cy, const Machine &m,
                   const OppPose *opp = nullptr);
  OppPose opponentAt(double t) const;

 private:
  // hauteur brute de la route sous (x, z), la plus proche de yRef ; faux hors de la route
  bool surfaceRaw(const Track &t, double x, double z, double yRef, double &y);
  int hint_ = -1;
  double eyeLift_ = 0;   // remontée de l'œil au-dessus de la route (lissée)
  struct Snap { double t; Pose p; int track; OppPose opp; };
  OppPose opponentFromGame(const Machine &m, int track);
  double oppY_ = 0, oppVy_ = 0;
  bool oppHave_ = false;
  void addCar(const OppPose &o, double hs, const Machine &m);
  std::vector<Track> tracks_;
  std::deque<Snap> snaps_;
  uint32_t lastTicks_ = 0, lastTickFrame_ = 0, frame_ = 0;
  bool pendingSnap_ = false;
  double tickPeriod_ = 7;              // retard d'affichage (trames)
  std::deque<double> intervals_;
  double playT_ = -1e9, lastT_ = 0;
  Renderer3D r3d_;
  std::vector<uint32_t> overlay_;
  std::vector<SpriteDraw> visible_;
  // flammes du boost en HD, calculées à partir des sprites d'origine (mises en cache par échelle)
  struct FlameImg { double scale = 0, ox = 0, oy = 0; int w = 0, h = 0; std::vector<float> px; };   // a, r, g, b prémultipliés
  std::map<int, FlameImg> flames_;
  const FlameImg &flameFor(int id, double s, const Machine &m);
  void drawFlame(int id, double x0, double y0, double s, const Machine &m, uint32_t *out, int W, int H);
  double windTarget_ = 0, wind_ = 0;   // vent relatif (0 à l'arrêt, 1 à pleine vitesse)
  const HdSprite *spriteFor(int id, const Machine &m);
  std::vector<int> mapX_, mapY_;
  int mapW_ = 0, mapH_ = 0;
};

}  // namespace scr
