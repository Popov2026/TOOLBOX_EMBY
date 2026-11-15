// webeditor.cpp — fonctions de l'éditeur de circuits (page editor.html, version navigateur) :
// la page donne la disquette du joueur (gardée dans le navigateur), le moteur fournit les formes des
// pièces, les profils de hauteur, les circuits d'origine et le décodage + la vérification d'un fichier .trk
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#include <cstdio>
#include <string>

#include "machine.hpp"
#include "tnt.hpp"
#include "track.hpp"

namespace {
scr::Bytes g_image;
std::string g_out;
}

extern "C" {
// disquette du joueur : 1 si le jeu est reconnu
EMSCRIPTEN_KEEPALIVE int scr_ed_init(const char *disk) {
  try { scr::Machine m(scr::readFile(disk)); g_image = m.image(); return 1; } catch (...) { g_image.clear(); return 0; }
}
EMSCRIPTEN_KEEPALIVE const char *scr_ed_shapes() {
  try { g_out = scr::shapesJson(g_image); } catch (...) { g_out = "[]"; }
  return g_out.c_str();
}
EMSCRIPTEN_KEEPALIVE const char *scr_ed_profiles() {
  try { g_out = scr::profilesJson(g_image); } catch (...) { g_out = "[]"; }
  return g_out.c_str();
}
// les 8 circuits de la disquette dans un fichier .trk (pour partir d'eux)
EMSCRIPTEN_KEEPALIVE int scr_ed_export(const char *path) {
  try { std::string err; return scr::saveTrackFile(path, scr::originalTrackSet(g_image), err) ? 1 : 0; } catch (...) { return 0; }
}
// décodage d'un fichier .trk : vérification et géométrie (sections par pièce) de chaque circuit, en JSON
EMSCRIPTEN_KEEPALIVE const char *scr_ed_decode(const char *path) {
  scr::TrackSet ts;
  std::string err;
  if (!scr::loadTrackSet(path, ts, err)) { g_out = "{\"ok\":false,\"report\":\"" + err + "\",\"tracks\":[]}"; return g_out.c_str(); }
  std::string report;
  const bool ok = scr::checkTrackSet(g_image, ts, report);
  for (auto &ch : report) if (ch == '"' || ch == '\\') ch = '\''; else if (ch == '\n') ch = '|';
  g_out = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"report\":\"" + report + "\",\"tracks\":[";
  try {
    auto tracks = scr::decodeTracks(g_image, &ts);
    char b[96];
    for (size_t k = 0; k < tracks.size(); k++) {
      const scr::Track &t = tracks[k];
      std::snprintf(b, sizeof b, "%s{\"pieces\":%d,\"start\":%d,\"gap\":%.0f,\"gapPiece\":%d,\"close\":%.0f,\"closeH\":%.0f,\"secs\":[",
                    k ? "," : "", t.pieces, t.startPiece, t.maxGap, t.gapPiece, t.closeGap, t.closeHeight);
      g_out += b;
      for (size_t i = 0; i < t.secs.size(); i++) {
        const scr::Section &s = t.secs[i];
        std::snprintf(b, sizeof b, "%s[%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%d]", i ? "," : "", s.lx, s.ly, s.lz, s.rx, s.ry, s.rz, s.piece);
        g_out += b;
      }
      g_out += "]}";
    }
  } catch (...) {}
  g_out += "]}";
  return g_out.c_str();
}
}
#endif
