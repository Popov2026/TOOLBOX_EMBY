// scenery.hpp — décor autour des circuits (mode HD) : tribunes avec public, panneaux publicitaires,
// arbres, voitures garées. Placement déterministe calculé à partir du tracé : jamais sur la route ni sous elle.
#pragma once
#include <cstdint>
#include <vector>

#include "track.hpp"

namespace scr {

enum SceneKind { SCENE_STAND = 0, SCENE_BOARD, SCENE_PINE, SCENE_TREE, SCENE_CAR };

struct SceneObj {
  int kind;
  double x, z;        // pied de l'objet (milieu de la façade pour tribunes et panneaux)
  double fx, fz;      // direction de la façade (vers le circuit), unitaire
  double len, size;   // longueur (tribune, panneau) ; échelle (arbres)
  uint32_t seed;      // couleurs et motifs
};

// décor d'un circuit (même résultat à chaque appel pour le même tracé) ; density 1 à 4 : nombre de
// tribunes, panneaux, arbres et voitures garées (parkings derrière les tribunes)
std::vector<SceneObj> buildScenery(const Track &t, int density = 1);

}  // namespace scr
