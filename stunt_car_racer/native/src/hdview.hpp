// hdview.hpp — mode HD (touche F1) : la scène 3D est redessinée en haute définition et en 16/9
// à partir de l'état du jeu d'origine (voiture, circuit), interpolée entre deux ticks de la
// logique (8,33 Hz) pour un affichage fluide ; le cockpit d'origine est incrusté par-dessus.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hdassets.hpp"
#include "machine.hpp"
#include "render3d.hpp"
#include "scenery.hpp"
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
  double eyeSide = -4, pitchOffset = -0.0056, yawOffset = 0;     // décalage latéral de l'œil, inclinaison de la caméra (rad)
  double hScale = 0.261;                    // hauteur brute -> unités de géométrie
  double eyeUp = 34, eyeFwd = -27;          // œil par rapport à la voiture (calé sur les bords de route d'origine, 4 circuits)
  int pitchSign = 1, rollSign = 1;
  bool cockpit = true;                     // incruster le cockpit d'origine
  double interpDelay = 0.75;               // retard d'affichage en ticks (interpolation ; 0,75 : bords de route proches de l'original en roulant, mouvement fluide)
  double minClearance = 18;                // l'œil reste au moins à cette hauteur au-dessus de la route
  bool builtinFlames = true;               // flammes du boost : sprites d'origine agrandis en HD
  bool flameImages = false;                // utiliser les images de flammes du dossier hd/ (sinon ignorées)
  bool opponent3D = true;                  // voiture adverse redessinée en 3D (sinon : pixels d'origine)
  bool highDetail = false;                 // détails élevés (F5) : sols et voiture texturés, roues rondes
  bool antialias = true;                   // lissage des bords (FXAA) de la scène 3D
  bool shadows = true;                     // ombres sous les voitures (posées sur la route, même en l'air)
  bool particles = true;                   // étincelles et fumée aux chocs et réceptions brutales
  bool scenery = true;                     // décor autour des circuits : tribunes, panneaux, arbres
  bool sky = true;                         // ciel dégradé avec soleil et nuages (sinon couleur unie d'origine)
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

  // course à plusieurs : voitures des autres joueurs (fantômes : dessinées, sans collision),
  // remplies par l'appelant avant render() ; render() calcule la place de leur nom à l'écran
  struct Ghost {
    OppPose pose; std::string name; bool boost = false; double sx = 0, sy = 0, dist = 0; bool onScreen = false; uint32_t id = 0;
    int impact = 0; double impactStamp = 0;   // dernier choc signalé par ce joueur : force (1-63) et numéro (0-3)
  };
  std::vector<Ghost> ghosts;
  double viewSide = 0;   // décalage latéral de la vue (+ : à droite) : départ de la grue du côté de sa place
  // voiture d'un joueur (état de la physique, comme poseFromState) -> repère de voiture affichable
  OppPose ghostFromPose(const Pose &p) const;
  // étincelles et fumée à ce point (x, z : unités du monde ; yRaw : hauteur brute), force 1 à 63
  void impactAt(double x, double yRaw, double z, int strength);
  void stepParticles(double dt);   // dt en trames (60 par seconde)

 private:
  // hauteur brute de la route sous (x, z), la plus proche de yRef ; faux hors de la route
  bool surfaceRaw(const Track &t, double x, double z, double yRef, double &y);
  int hint_ = -1;
  double eyeLift_ = 0;   // remontée de l'œil au-dessus de la route (lissée)
  struct Snap { double t; Pose p; int track; OppPose opp; int oppImpact = 0; };   // oppImpact : l'adversaire retombe (force)
  OppPose opponentFromGame(const Machine &m, int track);
  double oppY_ = 0, oppVy_ = 0, oppAir_ = 0, oppPx_ = 0, oppPz_ = 0;   // hauteur, vitesse verticale, hauteur au-dessus de la route
  double oppRaw_ = 0; int oppStale_ = 0;   // hauteur lue dans le jeu, nombre d'états où elle n'a pas bougé
  int oppImpact_ = 0;                // atterrissage brutal vu au dernier état
  double oppImpactT_ = -1;           // dernier atterrissage déjà montré (étincelles)
  // roues de l'adversaire : distance parcourue (unités du monde) -> angle de rotation
  double oppRoll_ = 0, oppRollStep_ = 0, oppRollContrast_ = 1;   // angle affiché, pas par image, netteté
  OppPose oppPrev_;
  struct GhostWheels { OppPose prev; double roll = 0; double impactStamp = -1; };   // roues (et chocs) de chaque autre joueur
  std::map<uint32_t, GhostWheels> ghostWheels_;
  bool oppHave_ = false;
  void addCar(const OppPose &o, double hs, const Machine &m, bool closed = false);
  std::map<int, std::vector<SceneObj>> scenery_;   // décor de chaque circuit (calculé à la première vue)
  void drawScenery(const Camera &c, int track, double hs);   // closed : flanc gauche dessiné aussi
  void addFlames(const OppPose &o, double hs, int seed);   // flammes du boost d'une voiture fantôme
  void addShadow(const OppPose &o, double hs, int track);   // ombre portée sur la route (ou le sol)
  // particules : étincelles (opaques, en traînée) et fumée (semi-transparente)
  struct Particle { Vec3 p, v; float life, maxLife, size; int kind; };
  std::vector<Particle> particles_;
  double partT_ = -1;
  int ownDamage_ = -1, ownImpact_ = 0;   // dégâts de la voiture du joueur ($10939-$1093B) : nouveaux chocs
  uint32_t partSeed_ = 12345;
  void spawnImpact(const Vec3 &at, int strength, const Vec3 &dir);
  void drawParticles(const Camera &c);
  uint32_t flameTick_ = 0;
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
