// gpu3d.hpp — rendu 3D HD sur la carte graphique (OpenGL 3.3) : même liste de polygones que le rendu
// logiciel (Renderer3D), mêmes textures procédurales, ciel, ombres et fumée, lissage (FXAA) ; l'image
// est relue pour que le cockpit et les sprites HD soient posés dessus comme avant. Contexte OpenGL dans
// une fenêtre cachée : l'affichage du jeu ne change pas. En cas d'échec, le jeu dessine sur le processeur.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "render3d.hpp"

struct SDL_Window;

namespace scr {

class Gpu3D : public GpuBackend {
 public:
  ~Gpu3D() override;
  bool init(std::string &err);       // fenêtre cachée, contexte, programmes ; faux + raison si impossible
  bool ok() const { return ok_; }
  const std::string &name() const { return name_; }   // carte graphique (GL_RENDERER)
  bool render(const Renderer3D &r, uint32_t *out) override;

 private:
  bool build(std::string &err);
  void resize(int W, int H);
  SDL_Window *win_ = nullptr;
  void *ctx_ = nullptr;
  bool ok_ = false, failed_ = false;
  std::string name_;
  unsigned progPoly_ = 0, progSky_ = 0, progFxaa_ = 0, vao_ = 0, vaoFull_ = 0, vbo_ = 0, ibo_ = 0;
  unsigned tex_[5] = {}, fbo_[2] = {}, color_[2] = {}, depth_ = 0;
  int W_ = 0, H_ = 0;
  std::vector<float> vtx_;
  std::vector<unsigned> idx_;
  struct Run { size_t first, count; bool fixed; };
  std::vector<Run> runs_;
};

}  // namespace scr
