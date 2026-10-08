// banner.hpp — petit texte (police 5×7) dessiné dans une image ARGB : bandeau d'information du
// multijoueur, visible aussi en plein écran (où le titre de la fenêtre est caché)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace scr {

// UTF-8 -> ASCII affichable (accents retirés : é -> e, « » -> ")
std::string bannerText(const std::string &utf8);

// texte sur fond semi-transparent ; renvoie une image ARGB de largeur w, hauteur h (agrandie ×scale)
std::vector<uint32_t> renderBanner(const std::string &utf8, int scale, int &w, int &h, uint32_t fg = 0xffffffffu,
                                   uint32_t bg = 0xb0000000u, int minChars = 0);

}  // namespace scr
