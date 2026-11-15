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
  // vérification (circuits personnels) : plus grand écart entre deux pièces qui se suivent (unités
  // de géométrie, case = $800) et pièce concernée ; écart de fermeture (position, hauteur brute)
  float maxGap = 0, closeGap = 0, closeHeight = 0;
  int gapPiece = -1;
};

static constexpr int CELL = 0x800;

struct TrackSet;
// image = TEXT+DATA du jeu relogés à $10100 ; ts : circuits TNT à la place des circuits d'origine
std::vector<Track> decodeTracks(const Bytes &image, const TrackSet *ts = nullptr);
// circuits personnels : décodage et raccords vérifiés ; vrai si jouables, report = explication
bool checkTrackSet(const Bytes &image, const TrackSet &ts, std::string &report);

}  // namespace scr
