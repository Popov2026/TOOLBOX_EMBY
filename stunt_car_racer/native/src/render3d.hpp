// render3d.hpp — rendu 3D logiciel haute définition : polygones convexes pleins,
// tampon de profondeur (1/z), découpage au plan proche, répartition par bandes sur plusieurs cœurs.
#pragma once
#include <cstdint>
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

class Renderer3D {
 public:
  Renderer3D();
  void begin(int W, int H, const Camera &cam, uint32_t background);
  // polygone convexe en coordonnées monde (3 à 8 sommets)
  void poly(const Vec3 *pts, int n, uint32_t color);
  // pour le décor lointain (sol, collines) : profondeur fixée au plus loin
  void polyFar(const Vec3 *pts, int n, uint32_t color, float depth);
  void finish(uint32_t *out);   // rastérisation (multi-cœurs) dans out (W×H ARGB)
  int threads() const { return threads_; }
  // repère caméra
  Vec3 toCam(const Vec3 &p) const;

 private:
  struct SPoly { int first, n; uint32_t color; float ia, ib, ic; float ymin, ymax; float fixedDepth; };
  void addClipped(const Vec3 *cam, int n, uint32_t color, float fixedDepth);
  void rasterBand(uint32_t *out, int y0, int y1);
  int W_ = 0, H_ = 0, threads_ = 1;
  Camera cam_;
  double cyw_, syw_, cp_, sp_, cr_, sr_;
  uint32_t bg_ = 0;
  std::vector<float> zbuf_;
  std::vector<float> sx_, sy_;        // sommets projetés
  std::vector<SPoly> polys_;
};

}  // namespace scr
