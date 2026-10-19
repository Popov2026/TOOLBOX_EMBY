// render3d.hpp — rendu 3D logiciel haute définition : polygones convexes pleins,
// tampon de profondeur (1/z), découpage au plan proche, répartition par bandes sur plusieurs cœurs.
#pragma once
#include <cstdint>
#include <utility>
#include <vector>

namespace scr {

struct Vec3 { double x, y, z; };

// caméra : position (unités de géométrie, y vers le haut), lacet (0 = +z, 90° = +x), tangage, roulis (radians)
struct Camera {
  Vec3 pos{0, 0, 0};
  double yaw = 0, pitch = 0, roll = 0;
  double focal = 1000;     // distance focale en pixels de sortie (horizontale)
  double focalY = 1000;    // verticale
  double cx = 0, cy = 0;   // centre de projection en pixels de sortie
  double nearZ = 4;
};

// textures procédurales (mode détails élevés) : calculées au pixel à partir du point du monde touché
enum TexKind { TEX_NONE = -1, TEX_GROUND = 0, TEX_ROAD, TEX_WALL, TEX_PAINT, TEX_TIRE, TEX_RIM };
struct TexInfo {
  int kind = TEX_NONE;
  Vec3 o{0, 0, 0}, u{1, 0, 0}, v{0, 0, 1};   // repère local (origine, axes unitaires)
  double s0 = 0, s1 = 1, w0 = 0, w1 = 1;     // étendue de la face dans ce repère (peinture : joints)
  double r = 1;                              // rayon (roue)
  double blur = 0;                           // roue : angle parcouru pendant l'image (flou de mouvement), radians
  double contrast = 1;                       // roue : netteté du motif (baisse à grande vitesse)
};

class Renderer3D {
 public:
  Renderer3D();
  void begin(int W, int H, const Camera &cam, uint32_t background);
  // polygone convexe en coordonnées monde (3 à 32 sommets)
  void poly(const Vec3 *pts, int n, uint32_t color);
  // polygone texturé : la couleur de base est modulée par la texture procédurale
  void polyTex(const Vec3 *pts, int n, uint32_t color, const TexInfo &t);
  // sol lointain texturé (plan horizontal y = h)
  void polyFarTex(const Vec3 *pts, int n, uint32_t color, float depth, const TexInfo &t, double h);
  // pour le décor lointain (sol, collines) : profondeur fixée au plus loin
  void polyFar(const Vec3 *pts, int n, uint32_t color, float depth);
  // polygone semi-transparent (ombres, fumée) : dessiné après les surfaces opaques, testé contre leur
  // profondeur sans l'écrire, mélangé à alpha (0 à 1) dans l'ordre d'envoi
  void polyAlpha(const Vec3 *pts, int n, uint32_t color, float alpha);
  // lissage des bords (anticrénelage de type FXAA) appliqué à l'image à la fin de finish()
  void setAntialias(bool on) { aa_ = on; }
  void finish(uint32_t *out);   // rastérisation (multi-cœurs) dans out (W×H ARGB)
  int threads() const { return threads_; }
  static void warmTextures();   // précalcule les textures (sinon au premier rendu détaillé)
  // repère caméra
  Vec3 toCam(const Vec3 &p) const;

 private:
  struct SPoly { int first, n; uint32_t color; float ia, ib, ic; float ymin, ymax; float fixedDepth; int tex; float alpha; };
  struct TexPlane { TexInfo t; Vec3 n; double d; };   // plan du polygone dans le monde : n·p = d
  void addClipped(const Vec3 *cam, int n, uint32_t color, float fixedDepth, int tex = -1, float alpha = 1.f);
  void fxaaBand(const uint32_t *src, int k, int n);
  bool aa_ = false;
  std::vector<std::vector<std::pair<size_t, uint32_t>>> aaFix_;   // pixels lissés, par fil de calcul
  std::vector<SPoly> trans_;   // polygones semi-transparents
  int addTex(const TexInfo &t, const Vec3 &n, double d);
  uint32_t shade(const TexPlane &tp, uint32_t color, const Vec3 &p, double foot) const;
  double M_[3][3];   // monde -> caméra (rotation)
  std::vector<TexPlane> texs_;
  void rasterBand(uint32_t *out, int k, int n);   // lignes y ≡ k (mod n)
  void shadeBand(uint32_t *out, int k, int n);    // ombrage différé des surfaces texturées
  int W_ = 0, H_ = 0, threads_ = 1;
  Camera cam_;
  double cyw_, syw_, cp_, sp_, cr_, sr_;
  uint32_t bg_ = 0;
  std::vector<float> zbuf_;
  std::vector<int32_t> idbuf_;        // surface texturée visible par pixel (-1 : couleur unie)
  std::vector<float> sx_, sy_;        // sommets projetés
  std::vector<SPoly> polys_;
};

}  // namespace scr
