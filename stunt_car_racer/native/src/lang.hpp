// lang.hpp — langue des textes ajoutés au jeu (menus, messages réseau, bandeaux) : anglais par
// défaut, français au choix (menu d'options F1, langue = fr dans scr.ini). Les écrans du jeu
// d'origine restent tels quels.
#pragma once

namespace scr {

inline bool g_french = false;
inline const char *L(const char *fr, const char *en) { return g_french ? fr : en; }

}  // namespace scr
