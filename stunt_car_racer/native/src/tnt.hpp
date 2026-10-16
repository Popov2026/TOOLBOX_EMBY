// tnt.hpp — circuits de « Stunt Car Racer TNT » (The New Tracks, version Amiga), lus dans le
// fichier SCR-TNT de l'utilisateur (ou directement dans l'archive WHDLoad .lha qui le contient).
// Les 8 nouveaux circuits ont le même format que ceux d'origine et utilisent les mêmes pièces ;
// le chargeur de la version TNT remplace la table des circuits, la base de leurs données, les noms,
// les initiales, et deux petites tables par circuit (cf. docs/RETRO_INGENIERIE.md §12).
#pragma once
#include <cstdint>
#include <string>

#include "scrdata.hpp"

namespace scr {

struct TrackSet {
  std::string names[8];      // 16 caractères, complétés par des espaces
  uint8_t initials[16] = {};  // deux lettres par circuit (ordre des divisions)
  uint8_t counts[8] = {};     // une valeur par circuit (table $4E7E4 de la version ST)
  uint8_t params[64] = {};    // réglages par circuit (table $1451A de la version ST)
  uint16_t offsets[8] = {};   // début de chaque tracé dans data ; pointeur 6502 = $B100 + offset
  Bytes data;                 // tracés, format d'origine
};

// adresses (RAM de la machine émulée, zone jamais utilisée par le jeu) des tracés et de leur table
constexpr uint32_t TNT_DATA = 0xE0000, TNT_TABLE = 0xDFF00;

// fichier SCR-TNT (exécutable Amiga) ou archive .lha le contenant ; faux + err si non reconnu
bool loadTrackSet(const std::string &path, TrackSet &ts, std::string &err);

}  // namespace scr
