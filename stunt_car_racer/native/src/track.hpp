// track.hpp — géométrie exacte des 8 circuits, décodée depuis le programme du jeu
// (port de la routine $48390, cf. docs/RETRO_INGENIERIE.md §4 et replica/js/scrdata.js).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "scrdata.hpp"

namespace scr {

// section transversale : bord gauche / bord droit. x,z en unités de géométrie
// (case = 0x800), y = hauteur brute du jeu (la physique l'utilise à l'échelle 1/2).
struct Section {
  float lx, ly, lz, rx, ry, rz;
  int flag;    // bit 0 / 1 : rupture de pente à gauche / à droite
  int piece;
};

struct Track {
  std::string name;
  std::vector<Section> secs;    // dans l'ordre de parcours, en commençant à la ligne de départ
  int startPiece = 0;
  int pieces = 0;
};

static constexpr int CELL = 0x800;

// image = TEXT+DATA du jeu relogés à $10100
std::vector<Track> decodeTracks(const Bytes &image);

}  // namespace scr
