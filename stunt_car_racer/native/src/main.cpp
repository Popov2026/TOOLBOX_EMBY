// main.cpp — Stunt Car Racer natif (SDL2).
//
// usage : scr [options] DISQUE.st|GAME.PUT
//   --track N        entraînement direct sur le circuit N (1..8) ; sans option : jeu complet
//   --scale N        taille de la fenêtre (× 320×200, défaut 3)
//   --fullscreen     plein écran
//   --speed X        vitesse du jeu (1 = Atari ST)
//   --smooth         lissage de l'image agrandie
//   --gravity X --thrust X --brake X --timestep X --damping X --boostuse X --shock N
//                    réglages de la physique d'origine (multiplicateurs, 1 = original)
//   --ini FICHIER    lit les mêmes options dans un fichier « clé = valeur » (défaut : scr.ini)
//
// Touches : flèches = joystick (haut accélère), Espace/Ctrl/Maj/Alt = bouton (boost),
//           clavier ST pour les menus, F4/F11/Alt+Entrée plein écran, F12 quitter, PageUp/PageDown
//           circuit précédent/suivant (entraînement), F5 détails élevés, F6 recommencer,
//           F7 circuits d'origine / circuits TNT, F8 menu d'options.
#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#define SCR_VERSION "v52"   // même numéro que SCR_RELAY_VERSION (server/scr_relay.cpp)

#include "banner.hpp"
#include "hdview.hpp"
#include "machine.hpp"
#include "net.hpp"
#include "track.hpp"
#include "tnt.hpp"
#include <chrono>
#include <random>
#include <tuple>

using namespace scr;

namespace {

struct Options {
  std::string disk;
  int track = -1;  // -1 : jeu complet
  int scale = 3;
  bool fullscreen = false, smooth = false, hd = false, sound = true;
  double volume = 0.8;
  int hdW = 1920, hdH = 1080;
  std::string hdDir;
  bool hdFlames = false;   // images de flammes du dossier hd/ (sinon : sprites d'origine agrandis)
  bool highDetail = false; // détails élevés au démarrage (touche F5)
  bool shadows = true, particles = true, antialias = true, sky = true, scenery = true;   // HD : ombres, étincelles et fumée, lissage, ciel, décor
  bool usePad = true, rumble = true;                         // manette, vibrations lors des chocs
  std::string keyNames[5] = {"Up", "Down", "Left", "Right", "Space"};   // touches : accélérer, freiner, gauche, droite, boost
  // multijoueur (câble « Computer Link » par le réseau)
  std::string relay;       // serveur relais : adresse[:port]
  std::string tntPath;     // circuits TNT : fichier SCR-TNT ou archive .lha (sinon cherché à côté du jeu)
  bool tnt = false;        // circuits = tnt : tout le jeu avec les circuits TNT
  std::string playerName;  // nom montré à l'autre joueur
  std::string netMode, netArg;   // au démarrage : host | joinlan | join ADRESSE | online CODE
  int netRate = 400;       // échanges réseau par seconde en multijoueur (lecture / envoi du câble)
  double speed = 1;
  Tuning tuning;
};

// serveur relais par défaut, quand ni la ligne de commande ni scr.ini n'en donnent un ; adresse
// stockée chiffrée (absente du texte du programme) et jamais affichée
static std::string defaultRelay() {
  static const uint8_t enc[] = {0x37, 0x06, 0x07, 0xe2, 0xef, 0xe9, 0xc9, 0xd1, 0xa7, 0xe1, 0xbf, 0x8a};
  std::string r;
  for (size_t i = 0; i < sizeof enc; i++) r += char(enc[i] ^ uint8_t(0x5a + 13 * i));
  return r + ":" + std::to_string(NET_PORT);
}
static const char *const DEFAULT_RELAY_NAME = "serveur officiel";
static std::string relayAddr(const std::string &configured) { return configured.empty() ? defaultRelay() : configured; }
static std::string relayShown(const std::string &configured) { return configured.empty() ? DEFAULT_RELAY_NAME : ""; }

// version annoncée par le serveur (« v49 ») plus récente que ce jeu ?
static bool newerVersion(const std::string &server) {
  if (server.size() < 2 || (server[0] != 'v' && server[0] != 'V')) return false;
  return std::atoi(server.c_str() + 1) > std::atoi(&SCR_VERSION[1]);
}

void setOption(Options &o, const std::string &key, const std::string &val) {
  auto d = [&] { return std::atof(val.c_str()); };
  if (key == "track") o.track = std::atoi(val.c_str()) - 1;
  else if (key == "scale") o.scale = std::atoi(val.c_str());
  else if (key == "fullscreen") o.fullscreen = val != "0";
  else if (key == "smooth") o.smooth = val != "0";
  else if (key == "hd") o.hd = val != "0";
  else if (key == "hddir") o.hdDir = val;
  else if (key == "hdflammes" || key == "hdflames") o.hdFlames = val != "0";
  else if (key == "details" || key == "détails") o.highDetail = val != "0";
  else if (key == "relais" || key == "relay") o.relay = val;
  else if (key == "tnt") o.tntPath = val;
  else if (key == "circuits" || key == "tracks") o.tnt = val == "tnt" || val == "TNT";
  else if (key == "nom" || key == "name") o.playerName = val;
  else if (key == "reseau_cadence" || key == "réseau_cadence" || key == "netrate") o.netRate = std::clamp(std::atoi(val.c_str()), 50, 2000);
  else if (key == "host" || key == "heberger") { if (val != "0") o.netMode = "host"; }
  else if (key == "joinlan") { if (val != "0") o.netMode = "joinlan"; }
  else if (key == "join" || key == "rejoindre") { o.netMode = "join"; o.netArg = val; }
  else if (key == "online" || key == "enligne") { o.netMode = "online"; o.netArg = val; }
  else if (key == "sound" || key == "son") o.sound = val != "0";
  else if (key == "volume") o.volume = std::clamp(d(), 0.0, 2.0);
  else if (key == "hdres") { int w = 0, h = 0; if (std::sscanf(val.c_str(), "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200) { o.hdW = w; o.hdH = h; } }
  else if (key == "speed") o.speed = d();
  else if (key == "gravity") o.tuning.gravity = d();
  else if (key == "thrust") o.tuning.thrust = d();
  else if (key == "brake") o.tuning.brake = d();
  else if (key == "timestep") o.tuning.timeStep = d();
  else if (key == "damping") o.tuning.damping = d();
  else if (key == "boostuse") o.tuning.boostUse = d();
  else if (key == "shock") o.tuning.shockTolerance = std::atoi(val.c_str());
  else if (key == "ombres" || key == "shadows") o.shadows = val != "0";
  else if (key == "particules" || key == "particles") o.particles = val != "0";
  else if (key == "lissage" || key == "antialias") o.antialias = val != "0";
  else if (key == "ciel" || key == "sky") o.sky = val != "0";
  else if (key == "decor" || key == "décor" || key == "scenery") o.scenery = val != "0";
  else if (key == "manette" || key == "gamepad") o.usePad = val != "0";
  else if (key == "vibrations" || key == "rumble") o.rumble = val != "0";
  else if (key == "touche_accelerer") o.keyNames[0] = val;
  else if (key == "touche_freiner") o.keyNames[1] = val;
  else if (key == "touche_gauche") o.keyNames[2] = val;
  else if (key == "touche_droite") o.keyNames[3] = val;
  else if (key == "touche_boost") o.keyNames[4] = val;
  else if (key == "disk") o.disk = val;
  else std::fprintf(stderr, "option inconnue : %s\n", key.c_str());
}

void readIni(Options &o, const std::string &path) {
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    auto h = line.find('#');
    if (h != std::string::npos) line = line.substr(0, h);
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    auto trim = [](std::string s) {
      while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
      while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
      return s;
    };
    setOption(o, trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
  }
}

// SDL_Scancode -> scancode ST
int stScancode(SDL_Scancode s) {
  static std::map<int, int> m;
  if (m.empty()) {
    const char *row1 = "1234567890";
    for (int i = 0; i < 10; i++) m[SDL_GetScancodeFromKey(row1[i])] = 0x02 + i;
    const struct { SDL_Scancode s; int st; } t[] = {
        {SDL_SCANCODE_ESCAPE, 0x01}, {SDL_SCANCODE_MINUS, 0x0c}, {SDL_SCANCODE_EQUALS, 0x0d}, {SDL_SCANCODE_BACKSPACE, 0x0e},
        {SDL_SCANCODE_TAB, 0x0f}, {SDL_SCANCODE_Q, 0x10}, {SDL_SCANCODE_W, 0x11}, {SDL_SCANCODE_E, 0x12}, {SDL_SCANCODE_R, 0x13},
        {SDL_SCANCODE_T, 0x14}, {SDL_SCANCODE_Y, 0x15}, {SDL_SCANCODE_U, 0x16}, {SDL_SCANCODE_I, 0x17}, {SDL_SCANCODE_O, 0x18},
        {SDL_SCANCODE_P, 0x19}, {SDL_SCANCODE_LEFTBRACKET, 0x1a}, {SDL_SCANCODE_RIGHTBRACKET, 0x1b}, {SDL_SCANCODE_RETURN, 0x1c},
        {SDL_SCANCODE_KP_ENTER, 0x72}, {SDL_SCANCODE_LCTRL, 0x1d}, {SDL_SCANCODE_A, 0x1e}, {SDL_SCANCODE_S, 0x1f},
        {SDL_SCANCODE_D, 0x20}, {SDL_SCANCODE_F, 0x21}, {SDL_SCANCODE_G, 0x22}, {SDL_SCANCODE_H, 0x23}, {SDL_SCANCODE_J, 0x24},
        {SDL_SCANCODE_K, 0x25}, {SDL_SCANCODE_L, 0x26}, {SDL_SCANCODE_SEMICOLON, 0x27}, {SDL_SCANCODE_APOSTROPHE, 0x28},
        {SDL_SCANCODE_GRAVE, 0x29}, {SDL_SCANCODE_LSHIFT, 0x2a}, {SDL_SCANCODE_BACKSLASH, 0x2b}, {SDL_SCANCODE_Z, 0x2c},
        {SDL_SCANCODE_X, 0x2d}, {SDL_SCANCODE_C, 0x2e}, {SDL_SCANCODE_V, 0x2f}, {SDL_SCANCODE_B, 0x30}, {SDL_SCANCODE_N, 0x31},
        {SDL_SCANCODE_M, 0x32}, {SDL_SCANCODE_COMMA, 0x33}, {SDL_SCANCODE_PERIOD, 0x34}, {SDL_SCANCODE_SLASH, 0x35},
        {SDL_SCANCODE_RSHIFT, 0x36}, {SDL_SCANCODE_LALT, 0x38}, {SDL_SCANCODE_SPACE, 0x39}, {SDL_SCANCODE_CAPSLOCK, 0x3a},
        {SDL_SCANCODE_F1, 0x3b}, {SDL_SCANCODE_F2, 0x3c}, {SDL_SCANCODE_F3, 0x3d},
        {SDL_SCANCODE_HOME, 0x47}, {SDL_SCANCODE_UP, 0x48}, {SDL_SCANCODE_LEFT, 0x4b},
        {SDL_SCANCODE_RIGHT, 0x4d}, {SDL_SCANCODE_DOWN, 0x50}, {SDL_SCANCODE_INSERT, 0x52}, {SDL_SCANCODE_DELETE, 0x53}};
    for (auto &e : t) m[e.s] = e.st;
  }
  auto it = m.find(s);
  return it == m.end() ? 0 : it->second;
}

void fail(const std::string &msg) {
  std::fprintf(stderr, "%s\n", msg.c_str());
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Stunt Car Racer", msg.c_str(), nullptr);
}

// sans argument (double-clic) : première image .st / GAME.PUT trouvée à côté de l'exécutable ou dans le dossier courant
// dossiers de l'exécutable et courant
std::vector<std::filesystem::path> baseDirs() {
  namespace fs = std::filesystem;
  std::vector<fs::path> dirs;
  if (char *b = SDL_GetBasePath()) { dirs.push_back(fs::u8path(b)); SDL_free(b); }
  std::error_code ec;
  fs::path cwd = fs::current_path(ec);
  if (!ec && (dirs.empty() || !fs::equivalent(dirs[0], cwd, ec))) dirs.push_back(cwd);
  return dirs;
}

// sans argument (double-clic) : première image .st / GAME.PUT trouvée dans DISK/, puis à côté de l'exécutable
std::string findDisk() {
  namespace fs = std::filesystem;
  std::vector<fs::path> dirs;
  for (auto &d : baseDirs()) { dirs.push_back(d / "DISK"); dirs.push_back(d / "disk"); }
  for (auto &d : baseDirs()) dirs.push_back(d);
  for (auto &d : dirs) {
    std::error_code ec;
    std::vector<fs::path> found;
    for (auto &e : fs::directory_iterator(d, ec)) {
      std::string ext = e.path().extension().u8string(), name = e.path().filename().u8string();
      for (auto &c : ext) c = (char)tolower((unsigned char)c);
      for (auto &c : name) c = (char)toupper((unsigned char)c);
      if (ext == ".st" || name == "GAME.PUT") found.push_back(e.path());
    }
    if (!found.empty()) { std::sort(found.begin(), found.end()); return found[0].u8string(); }
  }
  return {};
}

// circuits TNT : option tnt, sinon SCR-TNT ou une archive .lha de Stunt Car Racer TNT (dossier TNT ou
// DISK à côté de l'exécutable, ou à côté de lui)
std::string findTnt(const std::string &opt) {
  if (!opt.empty()) return opt;
  namespace fs = std::filesystem;
  std::vector<fs::path> dirs;
  for (auto &d : baseDirs()) { dirs.push_back(d / "TNT"); dirs.push_back(d / "tnt"); dirs.push_back(d / "DISK"); dirs.push_back(d / "disk"); dirs.push_back(d); }
  for (auto &d : dirs) {
    std::error_code ec;
    std::vector<fs::path> found;
    for (auto &e : fs::directory_iterator(d, ec)) {
      std::string ext = e.path().extension().u8string(), name = e.path().filename().u8string();
      for (auto &c : ext) c = (char)tolower((unsigned char)c);
      for (auto &c : name) c = (char)toupper((unsigned char)c);
      if (name == "SCR-TNT" || (ext == ".lha" && name.find("TNT") != std::string::npos)) found.push_back(e.path());
    }
    if (!found.empty()) { std::sort(found.begin(), found.end()); return found[0].u8string(); }
  }
  return {};
}

// dossier des images HD : option hddir, sinon hd/ à côté de l'exécutable ou dans le dossier courant
std::string findHdDir(const std::string &opt) {
  namespace fs = std::filesystem;
  if (!opt.empty()) return opt;
  std::error_code ec;
  for (auto &d : baseDirs())
    if (fs::is_directory(d / "hd", ec)) return (d / "hd").u8string();
  return {};
}

std::unique_ptr<Machine> start(const Bytes &disk, const Options &o, int track, const TrackSet *ts = nullptr) {
  auto m = std::make_unique<Machine>(disk);
  m->useTrackSet(ts);
  m->setTuning(o.tuning);
  if (track < 0) m->coldStart();
  else m->startPractice(track);
  return m;
}

// réglages de physique en texte (échangés entre les deux jeux) et retour
std::string tuningText(const Tuning &t) {
  char b[160];
  std::snprintf(b, sizeof b, "%.6g %.6g %.6g %.6g %.6g %.6g %d", t.gravity, t.thrust, t.brake, t.timeStep, t.damping,
                t.boostUse, t.shockTolerance);
  return b;
}
bool parseTuning(const std::string &s, Tuning &t) {
  return std::sscanf(s.c_str(), "%lf %lf %lf %lf %lf %lf %d", &t.gravity, &t.thrust, &t.brake, &t.timeStep, &t.damping,
                     &t.boostUse, &t.shockTolerance) == 7;
}

// réglages imposés par le serveur relais : « gravity=1 thrust=1 ... cadence=400 » (clé absente = 1)
bool parseServerParams(const std::string &s, Tuning &t, int &rate) {
  if (s.empty()) return false;
  t = Tuning();
  std::stringstream ss(s); std::string kv;
  while (ss >> kv) {
    size_t e = kv.find('=');
    if (e == std::string::npos) continue;
    std::string k = kv.substr(0, e); double v = std::atof(kv.c_str() + e + 1);
    if (k == "gravity") t.gravity = v;
    else if (k == "thrust") t.thrust = v;
    else if (k == "brake") t.brake = v;
    else if (k == "timestep") t.timeStep = v;
    else if (k == "damping") t.damping = v;
    else if (k == "boostuse") t.boostUse = v;
    else if (k == "shock") t.shockTolerance = int(v);
    else if (k == "cadence") rate = std::clamp(int(v), 50, 2000);
  }
  return true;
}

// résumé lisible des réglages du serveur pour le menu (« standard » si tout vaut 1)
std::string paramsSummary(const std::string &s) {
  Tuning t; int rate = 400;
  if (!parseServerParams(s, t, rate)) return "";
  std::string r;
  auto add = [&](const char *n, double v, double def) {
    if (std::fabs(v - def) > 1e-9) { char b[48]; std::snprintf(b, sizeof b, "%s%s %g", r.empty() ? "" : ", ", n, v); r += b; }
  };
  add("gravité", t.gravity, 1); add("poussée", t.thrust, 1); add("frein", t.brake, 1); add("pas", t.timeStep, 1);
  add("amorti", t.damping, 1); add("boost", t.boostUse, 1); add("chocs", t.shockTolerance, 0);
  return r.empty() ? "physique standard" : r;
}

// découpe un texte en lignes d'au plus w caractères (UTF-8), aux espaces si possible
std::vector<std::string> wrapText(const std::string &s, size_t w) {
  std::vector<std::string> out;
  std::string line, word;
  auto len = [](const std::string &x) { size_t n = 0; for (unsigned char c : x) n += (c & 0xc0) != 0x80; return n; };
  auto flushWord = [&] {
    while (len(word) > w) {   // mot trop long : coupé
      size_t n = 0, i = 0;
      for (; i < word.size() && n < w; i++) n += (static_cast<unsigned char>(word[i]) & 0xc0) != 0x80;
      while (i < word.size() && (static_cast<unsigned char>(word[i]) & 0xc0) == 0x80) i++;
      if (!line.empty()) { out.push_back(line); line.clear(); }
      out.push_back(word.substr(0, i)); word = word.substr(i);
    }
    if (line.empty()) line = word;
    else if (len(line) + 1 + len(word) <= w) line += " " + word;
    else { out.push_back(line); line = word; }
    word.clear();
  };
  for (char c : s) { if (c == ' ') { if (!word.empty()) flushWord(); } else word += c; }
  if (!word.empty()) flushWord();
  if (!line.empty()) out.push_back(line);
  return out;
}

// enregistre « relais = valeur » dans scr.ini : remplace la ligne relais existante (même mise en
// commentaire par un #), sinon l'ajoute à la fin
void saveIniValue(const std::string &path, const std::vector<std::string> &keys, const std::string &value, const std::string &comment0) {
  std::vector<std::string> lines;
  { std::ifstream f(path); std::string l; while (std::getline(f, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); lines.push_back(l); } }
  bool done = false;
  for (auto &l : lines) {
    size_t k = l.find_first_not_of(" \t#");
    if (k == std::string::npos) continue;
    std::string rest = l.substr(k);
    size_t eq = rest.find('=');
    if (eq == std::string::npos) continue;
    std::string key = rest.substr(0, eq);
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
    if (std::find(keys.begin(), keys.end(), key) != keys.end()) {
      std::string tail = rest.substr(eq + 1);
      size_t h = tail.find('#');
      std::string comment = h == std::string::npos ? "" : "   " + tail.substr(h);
      l = keys[0] + " = " + value + comment;
      done = true;
      break;
    }
  }
  if (!done) lines.push_back(keys[0] + " = " + value + "   # " + comment0);
  std::ofstream f(path, std::ios::trunc);
  for (auto &l : lines) f << l << "\n";
}
void saveRelay(const std::string &path, const std::string &value) {
  saveIniValue(path, {"relais", "relay"}, value, "serveur relais (menu 4. Online Multiplayer)");
}

// pseudo du joueur : caractères acceptés par la saisie des noms du jeu, 12 au plus
std::string cleanPseudo(const std::string &s) {
  std::string r;
  for (char ch : bannerText(s))
    if (r.size() < 12 && (std::isalnum(static_cast<unsigned char>(ch)) || ch == ' ' || ch == '.' || ch == '-')) r += ch;
  while (!r.empty() && r.front() == ' ') r.erase(r.begin());
  while (!r.empty() && r.back() == ' ') r.pop_back();
  return r;
}

}  // namespace

int main(int argc, char **argv) {
  Options o;
  // scr.ini : à côté de scr.exe (même si le jeu est lancé depuis un autre dossier), sinon dossier courant
  std::string ini = "scr.ini";
  if (char *bp = SDL_GetBasePath()) {
    std::string cand = std::string(bp) + "scr.ini";
    SDL_free(bp);
    if (std::ifstream(cand).good()) ini = cand;
  }
  for (int i = 1; i < argc; i++)
    if (!std::strcmp(argv[i], "--ini") && i + 1 < argc) ini = argv[i + 1];
  readIni(o, ini);
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--ini") { i++; continue; }
    if (a == "--fullscreen" || a == "--smooth" || a == "--hd" || a == "--host" || a == "--joinlan") { setOption(o, a.substr(2), "1"); continue; }
    if (a == "--help" || a == "-h") {
      std::puts("usage : scr [--track N] [--scale N] [--fullscreen] [--speed X] [--smooth] [--hd] [--hdres 1920x1080]\n"
                "            [--gravity X] [--thrust X] [--brake X] [--timestep X] [--damping X]\n"
                "            [--boostuse X] [--shock N] [--ini FICHIER]\n"
                "            [--host | --joinlan | --join ADRESSE[:PORT] | --online CODE] [--relais SERVEUR[:PORT]] [--nom NOM]\n"
                "            [--tnt SCR-TNT|archive.lha] [--circuits tnt]\n"
                "            DISQUE.st|GAME.PUT");
      return 0;
    }
    if (a.rfind("--", 0) == 0 && i + 1 < argc) { setOption(o, a.substr(2), argv[++i]); continue; }
    o.disk = a;
  }
  if (o.disk.empty()) o.disk = findDisk();
  if (o.disk.empty()) {
    fail("Image disque introuvable.\nPlacez votre image Stunt Car Racer (.st) dans le dossier DISK à côté de scr.exe,\n"
         "glissez-la sur scr.exe, ou lancez : scr Stunt_Car_Racer.st\n(aucune donnée du jeu n'est incluse)");
    return 1;
  }

  Bytes disk;
  std::unique_ptr<Machine> m;
  int track = o.track;
  static TrackSet tntSet;   // circuits TNT (si le fichier de l'utilisateur est trouvé)
  bool haveTnt = false;
  {
    std::string tp = findTnt(o.tntPath), err;
    if (!tp.empty()) {
      haveTnt = loadTrackSet(tp, tntSet, err);
      if (!haveTnt) std::fprintf(stderr, "circuits TNT ignorés (%s) : %s\n", tp.c_str(), err.c_str());
    }
    if (o.tnt && !haveTnt) std::fprintf(stderr, "circuits = tnt demandé, mais fichier SCR-TNT introuvable : circuits d'origine\n");
  }
  const TrackSet *soloSet = o.tnt && haveTnt ? &tntSet : nullptr;   // jeu complet et entraînement (F7 : changer)
  uint32_t gameHash = 0;
  std::string note;          // message bref du jeu dans le bandeau (circuits changés...)
  double noteUntil = 0;
  double quitAsk = 0;        // Échap au titre : quitter si on rappuie avant cette heure
  bool seenMenu = false;     // un menu du jeu a été affiché depuis le démarrage de la machine (sinon : titre)
  const Machine *seenFor = nullptr;
  try {
    disk = readFile(o.disk);
    m = start(disk, o, track, soloSet);
  } catch (std::exception &e) {
    fail(std::string("Erreur : ") + e.what());
    return 1;
  }

  // --- multijoueur : le câble série du jeu passe par le réseau
  NetLink net;
  {
    // empreinte du programme du jeu seulement ; les réglages de physique (scr.ini) sont envoyés en
    // texte et l'esclave adopte ceux du maître pendant la partie
    uint32_t h = fnv1a(m->image().data(), m->image().size());
    if (soloSet) h = fnv1a(tntSet.data.data(), tntSet.data.size(), h);   // duel : les deux avec les mêmes circuits
    gameHash = h;
    std::string nm = cleanPseudo(o.playerName);
    if (nm.empty()) { const char *u = std::getenv("USERNAME"); if (!u) u = std::getenv("USER"); nm = cleanPseudo(u ? u : ""); }
    if (nm.empty()) nm = "Joueur";
    o.playerName = nm;
    net.setIdentity(SCR_VERSION, h, nm, tuningText(o.tuning));
  }
  // menu « Computer Link » : quand le joueur choisit « 3. Computer Link », le jeu est retenu à l'entrée
  // du lien et ce menu propose les adversaires trouvés sur le réseau local, une salle du serveur
  // relais (code) ou une adresse IP. Celui qui choisit l'adversaire devient le maître (il mène les menus).
  enum class Typing { None, Code, Ip, Server, Chat, Pseudo };
  Typing typing = Typing::None;
  std::string typed;
  int menuSel = 0;
  double cancelUntil = -1;       // annulation : Échap transmis au jeu, qui abandonne le lien
  double fireUntil = 0;          // en multijoueur, un appui sur « feu » dure au moins 0,35 s
  double menuOpenedAt = 0;      // un appui encore tenu (boost à l'arrivée...) ne doit rien choisir
  bool menuOpen = false, prevUp = false, prevDown = false, prevFire = false;
  auto wireNet = [&] {
    m->serialOut = [&](uint8_t v) { net.send(v); };
    m->sliceHook = [&] { net.poll(); uint8_t b; while (net.recv(b)) m->serialPush(b); };
    // le maître démarre la poignée de main dès la connexion ; l'esclave attend le premier octet du maître
    // multijoueur online (duel par le serveur relais) : le maître remplit le nom des pilotes avec les
    // pseudos (le sien, puis celui de l'autre joueur) ; le Computer Link en réseau local reste d'origine
    m->linkName = [&](int k) -> std::string {
      if (!net.connected() || net.mode() != NetLink::Mode::Relay) return "";
      return k == 0 ? o.playerName : k == 1 ? cleanPseudo(net.peerName()) : "";
    };
    m->linkGate = [&] { return cancelUntil > 0 || (net.connected() && (net.master() || m->serialPending() > 0)); };
  };
  wireNet();
  if (o.netMode == "host") net.hostLan();
  else if (o.netMode == "joinlan") net.joinLan();
  else if (o.netMode == "join") net.joinDirect(o.netArg);
  else if (o.netMode == "online") net.joinRelay(relayAddr(o.relay), o.netArg == "auto" || o.netArg == "AUTO" ? "" : o.netArg, relayShown(o.relay));

  std::unique_ptr<HdView> view;
  bool hd = o.hd, highDetail = o.highDetail;
  std::string hdDir = findHdDir(o.hdDir);
  int hdImages = 0;
  auto newView = [&] {
    view = std::make_unique<HdView>(*m);
    view->params.flameImages = o.hdFlames;
    view->params.highDetail = highDetail;
    view->params.shadows = o.shadows; view->params.particles = o.particles; view->params.antialias = o.antialias; view->params.sky = o.sky; view->params.scenery = o.scenery;
    m->enableLayers(true);   // couches toujours suivies : F1 bascule sans délai
    hdImages = view->loadAssets(*m, hdDir);
    if (!view->assets.report.empty()) std::fprintf(stderr, "images HD (%s) :\n%s", hdDir.c_str(), view->assets.report.c_str());
  };
  try { newView(); } catch (std::exception &e) { fail(std::string("Erreur : ") + e.what()); return 1; }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
    fail(std::string("SDL : ") + SDL_GetError());
    return 1;
  }
  SDL_Window *win = SDL_CreateWindow("Stunt Car Racer " SCR_VERSION, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 320 * o.scale,
                                     200 * o.scale, SDL_WINDOW_RESIZABLE | (o.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, o.smooth ? "1" : "0");
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 200);
  SDL_Texture *texHd = nullptr;
  std::vector<uint32_t> hdPixels;
  // bascule d'affichage : 320×200 d'origine ou HD 16/9
  auto applyMode = [&](bool resizeWindow) {
    if (hd) {
      if (!texHd) {
        texHd = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, o.hdW, o.hdH);
        hdPixels.assign(size_t(o.hdW) * o.hdH, 0);
      }
      SDL_RenderSetLogicalSize(ren, o.hdW, o.hdH);
    } else {
      SDL_RenderSetLogicalSize(ren, 320, 200);
    }
    if (resizeWindow && !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP)) {
      SDL_Rect ub;
      int w = hd ? o.hdW : 320 * o.scale, h = hd ? o.hdH : 200 * o.scale;
      if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(win), &ub) == 0) {
        double k = std::min({1.0, ub.w * 0.95 / w, ub.h * 0.9 / h});
        w = int(w * k); h = int(h * k);
      }
      SDL_SetWindowSize(win, w, h);
      SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
  };
  applyMode(hd);
  int refresh = 60;
  {
    SDL_DisplayMode dm;
    if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(win), &dm) == 0 && dm.refresh_rate > 0) refresh = dm.refresh_rate;
  }
  // son : puce YM2149 émulée, échantillons de chaque trame mis en file (latence ~60 ms)
  SDL_AudioDeviceID audio = 0;
  const int AUDIO_RATE = 44100;
  bool muted = !o.sound;
  auto setupAudio = [&] {
    if (!audio) {
      SDL_AudioSpec want{}, have{};
      want.freq = AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 1; want.samples = 512;
      audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
      if (audio) {
        std::vector<int16_t> silence(AUDIO_RATE / 50 * 2, 0);
        SDL_QueueAudio(audio, silence.data(), Uint32(silence.size() * 2));
        SDL_PauseAudioDevice(audio, 0);
      }
    }
    m->setAudioRate(audio ? AUDIO_RATE : 0);
    m->setVolume(muted ? 0.0 : o.volume);
  };
  setupAudio();
  auto queueAudio = [&] {
    if (!audio) return;
    const auto &pcm = m->frameAudio();
    // file trop longue (émulation en avance) : on saute la trame pour garder une latence faible
    if (SDL_GetQueuedAudioSize(audio) > Uint32(AUDIO_RATE / 50 * 2 * 6)) return;
    if (!pcm.empty()) SDL_QueueAudio(audio, pcm.data(), Uint32(pcm.size() * 2));
  };
  // circuits du jeu en cours (d'origine ou TNT), pour la course à plusieurs et la grille
  std::vector<Track> trackList = decodeTracks(m->image(), m->trackSet());
  const std::vector<Track> tracksOrig = decodeTracks(m->image());
  const std::vector<Track> tracksTnt = haveTnt ? decodeTracks(m->image(), &tntSet) : std::vector<Track>();
  // relance la machine : -1 = jeu complet (titre), sinon entraînement sur ce circuit ;
  // course à plusieurs : 0-7 circuits d'origine, 8-15 circuits TNT
  auto restartMachine = [&](int trk) {
    view.reset();
    m.reset();
    const TrackSet *ts = trk < 0 ? soloSet : trk >= 8 && haveTnt ? &tntSet : nullptr;
    m = start(disk, o, trk < 0 ? -1 : trk & 7, ts);
    trackList = decodeTracks(m->image(), m->trackSet());
    newView();
    setupAudio();
    wireNet();
  };
  // déplace la voiture du joueur : vers l'avant (fwd) et vers la droite (side), unités de géométrie
  auto moveCar = [&](double fwd, double side) {
    double yaw = m->w(0x10ad0) * 2 * M_PI / 65536;
    double dx = std::sin(yaw) * fwd + std::cos(yaw) * side, dz = std::cos(yaw) * fwd - std::sin(yaw) * side;
    auto add = [&](uint32_t a, double d) {
      uint32_t v = m->l(a) + uint32_t(int32_t(d / 16 * 65536));
      m->ram[a] = uint8_t(v >> 24); m->ram[a + 1] = uint8_t(v >> 16); m->ram[a + 2] = uint8_t(v >> 8); m->ram[a + 3] = uint8_t(v);
    };
    add(0x10ac2, dx); add(0x10aca, dz);
  };
  // F6 recommencer, F7 circuits d'origine <-> TNT (enregistré dans scr.ini) ; entraînement : les
  // touches Page préc./suiv. passent par les 16 circuits (8 d'origine puis 8 TNT)
  auto changeTrack = [&](SDL_Scancode sc) {
    const int n = haveTnt ? 16 : 8;
    int cur = track + (soloSet ? 8 : 0);
    if (sc == SDL_SCANCODE_PAGEUP) cur = (cur + n - 1) % n;
    if (sc == SDL_SCANCODE_PAGEDOWN) cur = (cur + 1) % n;
    if (sc == SDL_SCANCODE_F7) cur = track < 0 ? (soloSet ? -1 : 8) : (cur + 8) % 16;
    bool tntNow = sc == SDL_SCANCODE_F7 ? !soloSet : cur >= 8;
    if (track >= 0) track = cur & 7;
    if (tntNow != (soloSet != nullptr)) {
      soloSet = tntNow ? &tntSet : nullptr;
      o.tnt = tntNow;
      saveIniValue(ini, {"circuits", "tracks"}, tntNow ? "tnt" : "original", "circuits : original ou tnt (touche F7)");
      note = tntNow ? "Circuits TNT (F7 : circuits d'origine)" : "Circuits d'origine (F7 : circuits TNT)"; noteUntil = NetLink::clock() + 4;
      uint32_t h = fnv1a(m->image().data(), m->image().size());
      if (soloSet) h = fnv1a(tntSet.data.data(), tntSet.data.size(), h);
      gameHash = h;
      net.setIdentity(SCR_VERSION, gameHash, o.playerName, tuningText(o.tuning));
    }
    view.reset();
    m.reset();
    m = start(disk, o, track, soloSet);
    trackList = decodeTracks(m->image(), m->trackSet());
    newView();
    setupAudio();
    wireNet();
  };
  uint64_t lastPresent = 0, fpsT0 = SDL_GetPerformanceCounter();
  int fpsN = 0;
  bool fpsLog = std::getenv("SCR_FPS") != nullptr;
  SDL_GameController *pad = nullptr;
  for (int i = 0; i < SDL_NumJoysticks() && !pad; i++)
    if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);

  static uint32_t pixels[320 * 200];
  bool keys[SDL_NUM_SCANCODES] = {};
  const double framePeriod = 1.0 / (50.0 * (o.speed > 0 ? o.speed : 1));
  uint64_t freq = SDL_GetPerformanceFrequency(), last = SDL_GetPerformanceCounter();
  double acc = 0;
  bool running = true;
  // éléments du menu « Computer Link »
  // 0 adversaire, 1 salle privée (code), 2 adresse IP, 3 annuler, 4 réseau local, 5 premier joueur disponible,
  // 6 tester à nouveau le serveur
  struct MenuItem { std::string label; int kind; NetLink::Peer peer; };
  RelayProbe probe;                 // « Online Multiplayer » : le serveur relais répond-il ?
  RelayProbe updateProbe;           // au démarrage : le serveur annonce la dernière version du jeu
  bool updateShown = false;
  updateProbe.start(relayAddr(o.relay));
  int netRate = o.netRate;          // cadence réseau en cours (scr.ini, ou imposée par le serveur relais)
  // 4. Online Multiplayer (sinon 3. Computer Link - Local LAN) ; retenu même si la machine est relancée
  bool onlineSel = false;
  // course à plusieurs (2 à 6 joueurs d'une salle privée) : chaque jeu roule en entraînement sur le même
  // circuit ; les autres joueurs sont des voitures fantômes (sans collision), dessinées en HD
  struct GroupRace {
    bool active = false;      // machine lancée pour une course du groupe (entraînement)
    bool waitingGo = false;   // départ pas encore donné par le serveur
    bool racing = false;      // entre le départ et la fin de la course
    bool menu = false;        // menu du groupe ouvert hors du « Computer Link »
    bool finished = false;
    bool out = false;         // abandon : voiture détruite (« wrecked »)
    int wreckFrames = 0;
    int track = 0, laps = 3;
    double goAt = 0, finishSecs = 0, lapOffset = 0, lastRel = -1, total = 0;
    uint32_t lastTick = 0;
    int slot = 0;             // place sur la grille : rangée slot / 2 (0 devant), côté slot % 2
    bool sideDone = false;    // atterrissage de la grue vu (le décalage sur le côté peut s'appliquer)
    uint16_t dropMode = 0;
    double sideApplied = 0, sideTarget = 0;   // part du décalage déjà appliquée à la voiture (le reste est ajouté à la position envoyée)
    uint32_t sideFrame = 0;
    double viewSide = 0;      // décalage de la vue (et de la position envoyée) : la grue arrive du côté de sa place
    uint32_t collTick = 0;    // collisions avec les autres voitures : une fois par cycle de la physique
    double tickRate = 17, rateT0 = 0; uint32_t rateTicks0 = 0;
    double lastHit = 0, lastSpark = 0;
    int dmgPrev = -1, impSeq = 0, impStr = 0;   // chocs de notre voiture (hausse des dégâts) signalés aux autres
    bool skip = false;        // circuit TNT choisi mais fichier SCR-TNT absent : ne participe pas
  } gr;
  int gTrack = 0, gLaps = 3;      // choix de l'organisateur
  const int nTracks = haveTnt ? 16 : 8;   // choix de l'organisateur : 8 d'origine, puis 8 TNT
  auto trackName = [&](int t) {
    const std::vector<Track> &L = t >= 8 ? tracksTnt : tracksOrig;
    std::string nm = (t & 7) < int(L.size()) ? L[size_t(t & 7)].name : "";
    if (nm.empty()) return "Circuit " + std::to_string(t + 1);
    return t >= 8 ? nm + " (TNT)" : nm;
  };
  auto online = [&] {
    if (net.inGroup() || gr.active) return onlineSel = true;   // course à plusieurs : machine relancée en entraînement
    if (m->selectChoice() == 3) onlineSel = true;
    else if (m->selectChoice() >= 0 && m->selectChoice() != 3) onlineSel = false;
    return onlineSel;
  };
  auto menuItems = [&] {
    std::vector<MenuItem> it;
    bool idle = !net.active() || net.state() == NetLink::State::Failed;
    if (online()) {
      bool inRoom = net.relayRoom() && net.state() == NetLink::State::Waiting;   // salle privée du relais
      if (net.inGroup()) {   // course à plusieurs : réglages de l'organisateur, départ
        bool host = net.groupHost() == net.groupMe(), lobby = net.groupPhase() == "lobby";
        if (host && lobby) {
          it.push_back({"Circuit : " + trackName(gTrack) + "  < >", 10, {}});
          it.push_back({"Tours : " + std::to_string(gLaps) + "  < >", 11, {}});
          if (net.groupMembers().size() >= 2)
            it.push_back({"Lancer la course (" + std::to_string(net.groupMembers().size()) + " joueurs)", 12, {}});
        }
        it.push_back({"Écrire un message (F9)", 8, {}});
        it.push_back({"Quitter la course à plusieurs", 13, {}});
        return it;
      }
      if (inRoom)
        for (auto &p : net.peers()) it.push_back({"Affronter " + p.name.substr(0, 20), 0, p});
      if (inRoom) it.push_back({"Course à plusieurs (2 à 6 joueurs)", 9, {}});
      if (inRoom) it.push_back({"Écrire un message (F9)", 8, {}});
      if (idle) {
        if (probe.ok()) {
          it.push_back({"Premier joueur disponible", 5, {}});
          it.push_back({"Salle privée (code)", 1, {}});
        }
        it.push_back({"Tester à nouveau le serveur", 6, {}});
        it.push_back({"Pseudo : " + o.playerName + "  (changer)", 14, {}});
        it.push_back({"Changer l'adresse du serveur", 7, {}});
      }
    } else {
      bool waiting = net.mode() == NetLink::Mode::Lobby && net.state() == NetLink::State::Waiting;
      if (waiting)
        for (auto &p : net.peers()) it.push_back({"Affronter " + p.name.substr(0, 20), 0, p});
      if (waiting || idle) {
        if (!waiting) it.push_back({"Chercher en réseau local", 4, {}});
        it.push_back({"Par adresse IP", 2, {}});
      }
    }
    it.push_back({"Annuler", 3, {}});
    return it;
  };
  std::function<void()> leaveGroup = [&] {   // quitter la course à plusieurs (retour au titre si une course a eu lieu)
    net.groupCommand("LEAVE");
    bool wasActive = gr.active;
    gr = GroupRace();
    menuSel = 0;
    if (wasActive) restartMachine(-1);
  };
  auto cancelLink = [&] {   // Échap transmis au jeu : « Link abandoned »
    net.close();
    typing = Typing::None; SDL_StopTextInput();
    double t = double(SDL_GetPerformanceCounter()) / double(SDL_GetPerformanceFrequency());
    cancelUntil = t + 0.6;
    m->setKey(0x01, true);
  };
  auto chooseItem = [&](const MenuItem &mi) {
    switch (mi.kind) {
      case 0: net.connectPeer(mi.peer); break;
      case 1: typing = Typing::Code; typed.clear(); SDL_StartTextInput(); break;
      case 2: typing = Typing::Ip; typed.clear(); SDL_StartTextInput(); break;
      case 3: cancelLink(); break;
      case 4: net.lobby(); break;
      case 5: net.joinRelay(relayAddr(o.relay), "", relayShown(o.relay)); menuSel = 0; break;
      case 6: probe.start(relayAddr(o.relay)); menuSel = 0; break;
      case 7: typing = Typing::Server; typed = o.relay; SDL_StartTextInput(); break;
      case 8: if (net.canChat()) { typing = Typing::Chat; typed.clear(); SDL_StartTextInput(); } break;
      case 9: net.groupCommand("ENTER"); menuSel = 0; break;
      case 14: typing = Typing::Pseudo; typed = o.playerName; SDL_StartTextInput(); break;
      case 10: gTrack = (gTrack + 1) % nTracks; break;
      case 11: gLaps = gLaps % 9 + 1; break;
      case 12: net.groupCommand("START " + std::to_string(gTrack) + " " + std::to_string(gLaps)); break;
      case 13: leaveGroup(); break;
    }
  };
  // aide aux tests automatiques (aucun effet sinon) : SCR_AUTOKEYS="trame:Touche,..." simule des appuis
  // (noms SDL : Space, Return, Down, A...) ; SCR_SHOT="trame:fichier.bmp" enregistre une capture
  struct AutoKey { uint32_t frame; SDL_Scancode sc; };
  std::vector<AutoKey> autoKeys;
  std::vector<std::pair<uint32_t, SDL_Scancode>> autoUps;
  std::vector<std::pair<uint32_t, std::string>> autoShots;
  auto parseAuto = [](const char *env, auto fn) {
    if (!env) return;
    std::stringstream ss(env); std::string tok;
    while (std::getline(ss, tok, ',')) { size_t c = tok.find(':'); if (c != std::string::npos) fn(uint32_t(std::atoi(tok.c_str())), tok.substr(c + 1)); }
  };
  parseAuto(std::getenv("SCR_AUTOKEYS"), [&](uint32_t f, const std::string &k) { autoKeys.push_back({f, SDL_GetScancodeFromName(k.c_str())}); });
  parseAuto(std::getenv("SCR_SHOT"), [&](uint32_t f, const std::string &k) { autoShots.push_back({f, k}); });
  // --- menu d'options (F8, ou bouton Start de la manette) : réglages enregistrés aussitôt dans scr.ini ;
  // le jeu est en pause pendant ce menu, sauf en multijoueur
  bool optOpen = false;
  int optSel = 0, optBind = -1;   // optBind : touche en attente (0 accélérer ... 4 boost)
  bool prevLeft = false, prevRight = false, prevStart = false;
  SDL_Scancode bindSc[5];
  static const SDL_Scancode bindDef[5] = {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_SPACE};
  static const char *const bindKeys[5] = {"touche_accelerer", "touche_freiner", "touche_gauche", "touche_droite", "touche_boost"};
  static const char *const bindLabels[5] = {"Accélérer", "Freiner", "Gauche", "Droite", "Boost"};
  for (int k = 0; k < 5; k++) {
    bindSc[k] = SDL_GetScancodeFromName(o.keyNames[k].c_str());
    if (bindSc[k] == SDL_SCANCODE_UNKNOWN) bindSc[k] = bindDef[k];
  }
  auto keyLabel = [](SDL_Scancode sc) -> std::string {
    switch (sc) {
      case SDL_SCANCODE_UP: return "Flèche haut";
      case SDL_SCANCODE_DOWN: return "Flèche bas";
      case SDL_SCANCODE_LEFT: return "Flèche gauche";
      case SDL_SCANCODE_RIGHT: return "Flèche droite";
      case SDL_SCANCODE_SPACE: return "Espace";
      default: { const char *n = SDL_GetScancodeName(sc); return n && *n ? n : "?"; }
    }
  };
  struct OptItem { std::string label; int id; };
  auto optItems = [&] {
    auto yn = [](bool b) { return std::string(b ? "oui" : "non"); };
    std::vector<OptItem> it;
    it.push_back({"Son : " + yn(!muted) + "  (F3)", 0});
    it.push_back({"Volume : " + std::to_string(int(std::lround(o.volume * 100))) + " %  < >", 1});
    it.push_back({"Affichage HD : " + yn(hd) + "  (F1)", 2});
    it.push_back({"Détails élevés : " + yn(highDetail) + "  (F5)", 3});
    it.push_back({"Ombres des voitures : " + yn(o.shadows), 4});
    it.push_back({"Étincelles et fumée : " + yn(o.particles), 5});
    it.push_back({"Lissage des bords : " + yn(o.antialias), 6});
    it.push_back({"Ciel avec nuages et soleil : " + yn(o.sky), 18});
    it.push_back({"Décor (tribunes, panneaux, arbres) : " + yn(o.scenery), 19});
    it.push_back({"Plein écran : " + yn(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) + "  (F11)", 7});
    it.push_back({haveTnt ? std::string("Circuits : ") + (soloSet ? "TNT" : "d'origine") + "  (F7)" : "Circuits TNT : fichier SCR-TNT absent", 8});
    it.push_back({"Manette : " + yn(o.usePad) + (pad ? "  (branchée)" : "  (aucune)"), 9});
    it.push_back({"Vibrations aux chocs : " + yn(o.rumble), 10});
    for (int k = 0; k < 5; k++)
      it.push_back({std::string("Touche ") + bindLabels[k] + " : " + (optBind == k ? "appuyez sur une touche..." : keyLabel(bindSc[k])), 11 + k});
    it.push_back({"Touches par défaut", 16});
    it.push_back({"Fermer", 17});
    return it;
  };
  auto saveFlag = [&](std::vector<std::string> keys, bool v, const char *comment) { saveIniValue(ini, keys, v ? "1" : "0", comment); };
  // dir : -1 / +1 (gauche / droite), 0 (Entrée)
  auto optChange = [&](int id, int dir) {
    switch (id) {
      case 0: muted = !muted; m->setVolume(muted ? 0.0 : o.volume); saveFlag({"son", "sound"}, !muted, "son (touche F3)"); break;
      case 1: {
        o.volume = std::clamp(std::round(o.volume * 10 + (dir < 0 ? -1 : 1)) / 10, 0.0, 2.0);
        if (!muted) m->setVolume(o.volume);
        char b[16]; std::snprintf(b, sizeof b, "%.1f", o.volume);
        saveIniValue(ini, {"volume"}, b, "volume : 0 à 2");
        break;
      }
      case 2: hd = !hd; applyMode(true); if (!hd) SDL_SetWindowTitle(win, "Stunt Car Racer " SCR_VERSION); saveFlag({"hd"}, hd, "affichage HD (touche F1)"); break;
      case 3:
        highDetail = !highDetail; view->params.highDetail = highDetail;
        if (highDetail && !hd) { hd = true; applyMode(true); }
        saveFlag({"details", "détails"}, highDetail, "détails élevés (touche F5)");
        break;
      case 4: o.shadows = !o.shadows; view->params.shadows = o.shadows; saveFlag({"ombres", "shadows"}, o.shadows, "ombres sous les voitures (HD)"); break;
      case 5: o.particles = !o.particles; view->params.particles = o.particles; saveFlag({"particules", "particles"}, o.particles, "étincelles et fumée lors des chocs (HD)"); break;
      case 6: o.antialias = !o.antialias; view->params.antialias = o.antialias; saveFlag({"lissage", "antialias"}, o.antialias, "lissage des bords (HD)"); break;
      case 7: {
        bool fs = !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP);
        SDL_SetWindowFullscreen(win, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        saveFlag({"fullscreen"}, fs, "plein écran (touche F11)");
        break;
      }
      case 8:
        if (!haveTnt) { note = "Circuits TNT : fichier SCR-TNT introuvable (dossier TNT à côté de scr.exe)"; noteUntil = NetLink::clock() + 5; }
        else if (net.inGroup() || gr.active) { note = "Course à plusieurs : le circuit est choisi par l'organisateur"; noteUntil = NetLink::clock() + 4; }
        else { changeTrack(SDL_SCANCODE_F7); optOpen = false; }
        break;
      case 9: o.usePad = !o.usePad; saveFlag({"manette", "gamepad"}, o.usePad, "manette"); break;
      case 10: o.rumble = !o.rumble; saveFlag({"vibrations", "rumble"}, o.rumble, "vibrations de la manette lors des chocs"); break;
      case 11: case 12: case 13: case 14: case 15: if (dir == 0) optBind = id - 11; break;
      case 16:
        for (int k = 0; k < 5; k++) {
          bindSc[k] = bindDef[k];
          saveIniValue(ini, {bindKeys[k]}, SDL_GetScancodeName(bindSc[k]), "touche (menu d'options, F8)");
        }
        break;
      case 17: optOpen = false; break;
      case 19: o.scenery = !o.scenery; view->params.scenery = o.scenery; saveFlag({"decor", "décor", "scenery"}, o.scenery, "décor autour des circuits : tribunes, panneaux, arbres (HD)"); break;
      case 18: o.sky = !o.sky; view->params.sky = o.sky; saveFlag({"ciel", "sky"}, o.sky, "ciel dégradé avec nuages et soleil (HD)"); break;
    }
  };
  int lastDamage = -1;              // vibrations : hausse des dégâts de la voiture
  const Machine *damageOf = nullptr;
  while (running) {
    for (size_t k = 0; k < autoKeys.size();) {
      if (m->frames() < autoKeys[k].frame) { k++; continue; }
      SDL_Event e{}; e.type = SDL_KEYDOWN; e.key.keysym.scancode = autoKeys[k].sc; SDL_PushEvent(&e);
      autoUps.push_back({m->frames() + 5, autoKeys[k].sc});
      autoKeys.erase(autoKeys.begin() + long(k));
    }
    for (size_t k = 0; k < autoUps.size();) {
      if (m->frames() < autoUps[k].first) { k++; continue; }
      SDL_Event e{}; e.type = SDL_KEYUP; e.key.keysym.scancode = autoUps[k].second; SDL_PushEvent(&e);
      autoUps.erase(autoUps.begin() + long(k));
    }
    {
      double t = double(SDL_GetPerformanceCounter()) / double(SDL_GetPerformanceFrequency());
      if (cancelUntil > 0 && t > cancelUntil) { cancelUntil = -1; m->setKey(0x01, false); }
      // menu du groupe : entre les courses, et dès que ce joueur a franchi l'arrivée (il attend les autres ;
      // le jeu d'origine peut finir l'entraînement de son côté et laisser un écran noir derrière)
      gr.menu = net.inGroup() && (!gr.racing || gr.finished) && !gr.waitingGo;
      bool open = (m->linkHeld() && cancelUntil < 0 && !net.connected()) || gr.menu;
      if (open && !menuOpen && !gr.menu) {   // le joueur vient de choisir « Computer Link » ou « Online Multiplayer »
        menuSel = 0;
        if (online()) { if (net.mode() == NetLink::Mode::Lobby) net.close(); if (!net.active()) probe.start(relayAddr(o.relay)); }
        else if (!net.active() || net.mode() == NetLink::Mode::Relay) net.lobby();
      }
      if (!open && menuOpen && typing != Typing::None && typing != Typing::Chat) { typing = Typing::None; SDL_StopTextInput(); }
      if (typing == Typing::Chat && !net.canChat()) { typing = Typing::None; SDL_StopTextInput(); }
      if (open && !menuOpen) { menuSel = 0; menuOpenedAt = NetLink::clock(); }   // à l'ouverture : en haut, touches tenues ignorées
      menuOpen = open;
    }
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) running = false;
      else if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(ev.cdevice.which);
      else if (ev.type == SDL_TEXTINPUT && typing != Typing::None) {   // code de salle ou adresse
        for (const char *c = ev.text.text; *c; c++) {
          unsigned char ch = static_cast<unsigned char>(*c);
          if (typing == Typing::Code && std::isalnum(ch) && typed.size() < 12) typed += char(std::toupper(ch));
          if ((typing == Typing::Ip || typing == Typing::Server) && (std::isalnum(ch) || ch == '.' || ch == ':' || ch == '-') && typed.size() < 60) typed += char(ch);
          if (typing == Typing::Chat && ch >= 32 && typed.size() < 110) typed += char(ch);
          if (typing == Typing::Pseudo && (std::isalnum(ch) || ch == ' ' || ch == '.' || ch == '-') && typed.size() < 12) typed += char(ch);
        }
      }
      else if (ev.type == SDL_KEYDOWN && typing == Typing::None && !ev.key.repeat && ev.key.keysym.scancode == SDL_SCANCODE_F8 && optBind < 0) {
        optOpen = !optOpen; optSel = 0;
      }
      else if (ev.type == SDL_KEYDOWN && optOpen && typing == Typing::None) {   // menu d'options
        SDL_Scancode sc = ev.key.keysym.scancode;
        auto items = optItems();
        int n = int(items.size());
        optSel = std::clamp(optSel, 0, n - 1);
        if (optBind >= 0) {   // nouvelle touche (Échap : annuler)
          if (sc != SDL_SCANCODE_ESCAPE && !ev.key.repeat) {
            bindSc[optBind] = sc;
            saveIniValue(ini, {bindKeys[optBind]}, SDL_GetScancodeName(sc), "touche (menu d'options, F8)");
          }
          optBind = -1;
        }
        else if (sc == SDL_SCANCODE_ESCAPE) optOpen = false;
        else if (sc == SDL_SCANCODE_UP) optSel = (optSel + n - 1) % n;
        else if (sc == SDL_SCANCODE_DOWN) optSel = (optSel + 1) % n;
        else if (sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT) {
          int id = items[size_t(optSel)].id;
          if (id <= 10 || id >= 18) optChange(id, sc == SDL_SCANCODE_RIGHT ? 1 : -1);
        }
        else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER || sc == SDL_SCANCODE_SPACE) optChange(items[size_t(optSel)].id, 0);
        else if (sc == SDL_SCANCODE_F12) running = false;
      }
      else if (ev.type == SDL_KEYDOWN && typing == Typing::None && ev.key.keysym.scancode == SDL_SCANCODE_F9) {
        if (net.canChat()) { typing = Typing::Chat; typed.clear(); SDL_StartTextInput(); }   // discussion
      }
      else if (ev.type == SDL_KEYDOWN && typing != Typing::None) {
        SDL_Scancode sc = ev.key.keysym.scancode;
        if (sc == SDL_SCANCODE_ESCAPE) { typing = Typing::None; SDL_StopTextInput(); }
        else if (sc == SDL_SCANCODE_BACKSPACE && !typed.empty()) {   // un caractère (UTF-8) entier
          while (!typed.empty() && (static_cast<unsigned char>(typed.back()) & 0xc0) == 0x80) typed.pop_back();
          if (!typed.empty()) typed.pop_back();
        }
        else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
          if (typing == Typing::Code) {
            if (typed.empty()) {   // pas de code : on en invente un à donner aux autres joueurs
              std::mt19937 rng(unsigned(std::chrono::steady_clock::now().time_since_epoch().count()));
              typed = std::to_string(1000 + rng() % 9000);
            }
            net.joinRelay(relayAddr(o.relay), typed, relayShown(o.relay));
          } else if (typing == Typing::Pseudo) {   // pseudo : enregistré dans scr.ini, annoncé aux autres
            std::string p = cleanPseudo(typed);
            if (!p.empty()) { o.playerName = p; saveIniValue(ini, {"nom", "name"}, p, "pseudo (multijoueur)"); net.setName(p); }
          } else if (typing == Typing::Chat) {   // message envoyé ; F9 pour le suivant
            net.sendChat(typed);
          } else if (typing == Typing::Server) {   // adresse du serveur relais : enregistrée dans scr.ini
            if (!typed.empty() && typed.find(':') == std::string::npos) typed += ":" + std::to_string(NET_PORT);
            if (typed != o.relay) {   // vide : retour au serveur par défaut
              o.relay = typed;
              saveRelay(ini, typed);
            }
            probe.start(relayAddr(o.relay));
          } else if (!typed.empty()) net.joinDirect(typed);
          typing = Typing::None; SDL_StopTextInput(); menuSel = 0;
        }
      }
      else if (ev.type == SDL_KEYDOWN && menuOpen) {   // menu « Computer Link »
        SDL_Scancode sc = ev.key.keysym.scancode;
        auto items = menuItems();
        int n = int(items.size());
        int kind = n ? items[size_t(std::min(menuSel, n - 1))].kind : -1;
        if ((sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT) && (kind == 10 || kind == 11)) {   // circuit, tours
          int d = sc == SDL_SCANCODE_RIGHT ? 1 : -1;
          if (kind == 10) gTrack = (gTrack + nTracks + d) % nTracks; else gLaps = std::clamp(gLaps + d, 1, 9);
        }
        else if (sc == SDL_SCANCODE_ESCAPE && net.inGroup()) leaveGroup();
        else if (sc == SDL_SCANCODE_UP) menuSel = (menuSel + n - 1) % n;
        else if (sc == SDL_SCANCODE_DOWN) menuSel = (menuSel + 1) % n;
        else if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_F10) cancelLink();
        else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER || sc == SDL_SCANCODE_SPACE)
          { if (NetLink::clock() - menuOpenedAt > 0.8) chooseItem(items[size_t(std::min(menuSel, n - 1))]); }
        else if (sc == SDL_SCANCODE_F12) running = false;
        else if (sc == SDL_SCANCODE_F11 || sc == SDL_SCANCODE_F4) {
          bool fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
          SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        }
      }
      else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
        bool down = ev.type == SDL_KEYDOWN;
        SDL_Scancode sc = ev.key.keysym.scancode;
        keys[sc] = down;
        if (down && !ev.key.repeat && sc == SDL_SCANCODE_F10) net.close("déconnexion");   // multijoueur : couper
        if (down && !ev.key.repeat) {
          if (sc == SDL_SCANCODE_F12) running = false;
          else if (sc == SDL_SCANCODE_F3) {   // couper / rétablir le son
            muted = !muted;
            m->setVolume(muted ? 0.0 : o.volume);
          }
          else if (sc == SDL_SCANCODE_F2) {   // recharge le dossier hd/ (images modifiées pendant le jeu)
            if (hdDir.empty()) hdDir = findHdDir(o.hdDir);
            hdImages = view->loadAssets(*m, hdDir);
          }
          else if (sc == SDL_SCANCODE_F1) {
            hd = !hd;
            applyMode(true);
            if (!hd) SDL_SetWindowTitle(win, "Stunt Car Racer " SCR_VERSION);
          }
          else if (sc == SDL_SCANCODE_F11 || sc == SDL_SCANCODE_F4 ||
                   ((sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) && (ev.key.keysym.mod & KMOD_ALT))) {   // plein écran
            bool fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
            SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
          } else if (sc == SDL_SCANCODE_F5) {   // détails élevés (textures, roues rondes) ; passe en HD si besoin
            highDetail = !highDetail;
            view->params.highDetail = highDetail;
            if (highDetail && !hd) { hd = true; applyMode(true); }
          } else if (sc == SDL_SCANCODE_F7 && !haveTnt) {
            note = "Circuits TNT : fichier SCR-TNT introuvable (dossier TNT à côté de scr.exe)"; noteUntil = NetLink::clock() + 5;
          } else if (sc == SDL_SCANCODE_F6 || (sc == SDL_SCANCODE_F7 && !net.inGroup() && !gr.active) ||
                     ((sc == SDL_SCANCODE_PAGEUP || sc == SDL_SCANCODE_PAGEDOWN) && track >= 0)) {
            changeTrack(sc);
          }
        }
        // Échap dans un menu du jeu : retour au menu précédent (la touche n'est alors pas transmise au jeu)
        // (pendant « NAME? », elle l'est : elle réveille la lecture du clavier qui attend une touche)
        bool back = down && sc == SDL_SCANCODE_ESCAPE && !menuOpen && (ev.key.repeat || m->menuBack()) && m->inMenu();
        // Échap là où le jeu d'origine n'a pas de retour : titre et menu de départ -> quitter (après
        // confirmation) ; menu de saison -> retour au titre ; entraînement direct -> titre ; course à
        // plusieurs -> abandon (menu du groupe). En course normale, Échap garde son effet d'origine.
        if (down && !ev.key.repeat && sc == SDL_SCANCODE_ESCAPE && !menuOpen && !back && m->ram[0x4537a] == 0 && !net.connected()) {
          const double now = NetLink::clock();
          const int id = m->menuId();
          const bool racingNow = view && view->racing();
          bool handled = true;
          if (gr.active) {
            if (gr.racing && !gr.finished) { gr.finished = gr.out = true; gr.finishSecs = now - gr.goAt; net.groupCommand("OUT"); }
            else handled = false;
          } else if (id == 0) {   // « Hall of Fame / Practise / Start the Racing Season / Load-Save »
            restartMachine(-1); track = -1;
          } else if (track >= 0 && racingNow) {   // entraînement lancé directement (--track, Page préc./suiv.)
            track = -1; restartMachine(-1);
          } else if (id == 0x10 || (id < 0 && !seenMenu && !racingNow)) {   // titre, menu de départ
            if (now < quitAsk) running = false;
            else { quitAsk = now + 3; note = "Échap encore une fois pour quitter le jeu"; noteUntil = quitAsk; }
          } else handled = false;
          if (handled) continue;
        }
        // touche de conduite choisie dans le menu d'options : pas transmise au jeu pendant la course
        bool driveKey = view && view->racing() && down && std::find(std::begin(bindSc), std::end(bindSc), sc) != std::end(bindSc) &&
                        std::find(std::begin(bindDef), std::end(bindDef), sc) == std::end(bindDef);
        if (int st = stScancode(sc)) if ((!menuOpen || !down) && !back && !driveKey) m->setKey(st, down);
      }
    }
    if (m.get() != seenFor) { seenFor = m.get(); seenMenu = false; }
    if (!updateShown && updateProbe.state() != RelayProbe::State::Probing) {   // nouvelle version : prévenu une fois
      updateShown = true;
      if (updateProbe.ok() && newerVersion(updateProbe.version())) {
        note = "Nouvelle version " + updateProbe.version() + " de Stunt Car Racer disponible (vous avez " SCR_VERSION ")";
        noteUntil = NetLink::clock() + 12;
      }
    }
    if (m->inMenu()) seenMenu = true;
    Joystick j;
    // flèches toujours actives (menus du jeu) ; touches choisies dans le menu d'options en plus
    j.up = keys[SDL_SCANCODE_UP] || keys[bindSc[0]]; j.down = keys[SDL_SCANCODE_DOWN] || keys[bindSc[1]];
    j.left = keys[SDL_SCANCODE_LEFT] || keys[bindSc[2]]; j.right = keys[SDL_SCANCODE_RIGHT] || keys[bindSc[3]];
    // boost : plusieurs touches, car beaucoup de claviers ne lisent pas Espace + deux flèches à la fois
    // (la direction semblait alors bloquée pendant le boost) ; Maj et Alt n'ont pas ce problème
    j.fire = keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL] || keys[SDL_SCANCODE_LSHIFT] ||
             keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT] || keys[bindSc[4]];
    // en multijoueur, les menus lisent le joystick à travers le lien : un appui bref pourrait être manqué
    double tNow = double(SDL_GetPerformanceCounter()) / double(SDL_GetPerformanceFrequency());
    bool padStart = false;
    if (pad && o.usePad) {
      padStart = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START);
      int ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
      j.left |= ax < -12000 || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
      j.right |= ax > 12000 || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
      j.up |= ay < -12000 || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) ||
              SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000;
      j.down |= ay > 12000 || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) ||
                SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000;
      j.fire |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X);
    }
    if (padStart && !prevStart && typing == Typing::None && optBind < 0) { optOpen = !optOpen; optSel = 0; }
    prevStart = padStart;
    if (optOpen && optBind < 0) {   // manette dans le menu d'options
      auto items = optItems();
      int n = int(items.size());
      if (j.up && !prevUp) optSel = (optSel + n - 1) % n;
      if (j.down && !prevDown) optSel = (optSel + 1) % n;
      int id = items[size_t(std::clamp(optSel, 0, n - 1))].id;
      if (((j.left && !prevLeft) || (j.right && !prevRight)) && (id <= 10 || id >= 18)) optChange(id, j.right ? 1 : -1);
      if (j.fire && !prevFire && (id < 11 || id > 15)) optChange(id, 0);   // touches : au clavier
    }
    else if (menuOpen && typing == Typing::None) {   // manette dans le menu « Computer Link »
      auto items = menuItems();
      int n = int(items.size());
      if (j.up && !prevUp) menuSel = (menuSel + n - 1) % n;
      if (j.down && !prevDown) menuSel = (menuSel + 1) % n;
      if (j.fire && !prevFire && pad && NetLink::clock() - menuOpenedAt > 0.8) chooseItem(items[size_t(std::min(menuSel, n - 1))]);
    }
    prevUp = j.up; prevDown = j.down; prevFire = j.fire; prevLeft = j.left; prevRight = j.right;
    if (menuOpen || optOpen || typing != Typing::None) j = Joystick();
    if (net.active()) {
      if (j.fire) fireUntil = tNow + 0.35;
      j.fire = j.fire || tNow < fireUntil;
    }
    m->setJoystick(j);
    net.poll();   // aussi entre les trames (connexion, annonces), pas seulement pendant l'émulation
    {   // course à plusieurs : départ (machine relancée sur le circuit), top départ, fin
      std::string ev;
      while (net.takeGroupEvent(ev)) {
        if (ev.rfind("START", 0) == 0) {
          int t = 0, l = 3;
          std::sscanf(ev.c_str() + 5, "%d %d", &t, &l);
          {   // ordre sur la grille de départ (numéros des joueurs après circuit et tours)
            std::stringstream ss(ev.substr(5)); int skip; ss >> skip >> skip;
            std::vector<uint32_t> order; uint32_t id;
            while (ss >> id) order.push_back(id);
            if (order.empty())   // serveur antérieur à la v28 (pas d'ordre) : ordre des joueurs du groupe, le même partout
              for (auto &gm : net.groupMembers()) order.push_back(gm.id);
            gr.slot = 0;
            for (size_t k = 0; k < order.size(); k++) if (order[k] == net.groupMe()) gr.slot = int(k);
          }
          if ((t & 15) >= 8 && !haveTnt) {   // circuit TNT sans le fichier SCR-TNT : on ne peut pas rouler dessus
            net.localNote("Circuit TNT choisi : fichier SCR-TNT absent (dossier TNT à côté de scr.exe), course ignorée");
            gr = GroupRace(); gr.skip = true;
            net.groupCommand("LOADED");
            continue;
          }
          gr.track = t & 15; gr.laps = l; gr.active = true; gr.waitingGo = true; gr.racing = false; gr.finished = false; gr.out = false; gr.wreckFrames = 0;
          gr.lastRel = -1; gr.total = 0; gr.lapOffset = 0; gr.lastTick = 0; gr.goAt = 0;
          if (typing != Typing::None && typing != Typing::Chat) { typing = Typing::None; SDL_StopTextInput(); }
          if (!hd) { hd = true; applyMode(true); }   // les autres voitures ne sont visibles qu'en HD
          restartMachine(gr.track);
          // l'entraînement d'origine s'arrête après 3 tours ($10983 = 4 : fin quand le compteur $1090A
          // l'atteint) : désactivé, c'est la course à plusieurs qui décide de l'arrivée
          m->ram[0x10983] = 0xff;
          // grille : 3 rangées de 2 ; les rangées 2 et 3 reculent avant la descente de la grue
          // (vérifié sur les 8 circuits : la voiture se pose sur la route), le côté est réglé à l'atterrissage
          moveCar(-250.0 * (gr.slot / 2), 0);
          gr.sideDone = false; gr.dropMode = m->w(0x109d4); gr.sideApplied = 0; gr.sideTarget = 0; gr.sideFrame = 0;
          net.groupCommand("LOADED");
          acc = 0;
        } else if (ev == "GO" && gr.skip) {
          net.groupCommand("OUT"); gr.skip = false;
        } else if (ev == "GO") {
          gr.waitingGo = false; gr.racing = true; gr.goAt = NetLink::clock(); acc = 0;
        } else if (ev == "END") {
          gr.racing = false; gr.waitingGo = false;
        }
      }
      if (!net.inGroup() && gr.active) {   // groupe perdu (connexion coupée...) : retour au titre, la raison reste affichée
        gr = GroupRace();
        restartMachine(-1);
      }
    }
    {   // réglages de physique : l'esclave prend ceux du maître le temps de la partie (course équitable)
      static std::string adopted;
      static const Machine *adoptedOn = nullptr;
      if (adoptedOn != m.get()) { adoptedOn = m.get(); adopted.clear(); }   // F6 : nouvelle machine, réglages de scr.ini
      Tuning t;
      std::string want;
      int rate = o.netRate;
      // en ligne : le serveur relais impose ses réglages à tous ; en réseau local : ceux du maître
      if (net.mode() == NetLink::Mode::Relay && parseServerParams(net.serverParams(), t, rate)) want = tuningText(t);
      else if (net.connected() && !net.master() && parseTuning(net.peerTuning(), t)) want = net.peerTuning();
      netRate = rate;
      if (want == tuningText(o.tuning)) want.clear();
      if (want != adopted) {
        if (!want.empty()) { m->setTuning(t); std::fprintf(stderr, "réglages de physique appliqués (%s) : %s\n", net.mode() == NetLink::Mode::Relay ? "serveur" : "maître", want.c_str()); }
        else m->setTuning(o.tuning);
        adopted = want;
      }
    }

    uint64_t now = SDL_GetPerformanceCounter();
    acc += double(now - last) / double(freq);
    last = now;
    if (acc > 0.25) acc = 0.25;
    if (gr.waitingGo) acc = 0;   // course à plusieurs : la voiture attend le départ commun
    if (optOpen && !net.active() && !gr.active && autoKeys.empty() && autoShots.empty()) acc = 0;   // menu d'options : pause (sauf en multijoueur ; tests automatiques : pas de pause)
    int n = 0;
    // multijoueur : chaque trame est émulée en tranches réparties dans le temps réel, le câble est lu
    // et écrit à chaque tranche (reseau_cadence fois par seconde) ; sinon, trames entières
    const int parts = net.active() ? std::clamp((netRate + 25) / 50, 1, 40) : 0;
    {
      static bool vsyncOff = false;   // la synchro verticale bloquerait la boucle entre deux tranches
      if (vsyncOff != (parts > 0)) { vsyncOff = parts > 0; SDL_RenderSetVSync(ren, vsyncOff ? 0 : 1); }
    }
    if (parts) {
      while (n < 10 * parts && acc >= (m->part() + 1) * framePeriod / parts) {
        if (m->runPart(parts)) {
          view->afterFrame(*m);
          queueAudio();
          acc -= framePeriod;
        }
        n++;
      }
    } else
      while (acc >= framePeriod && n < 10) {
        m->runFrame();
        view->afterFrame(*m);
        queueAudio();
        acc -= framePeriod; n++;
      }
    {   // vibrations de la manette : choc ou réception brutale (hausse des dégâts $10939-$1093B)
      int d = m->b(0x10939) + m->b(0x1093a) + m->b(0x1093b);
      if (damageOf == m.get() && d > lastDamage && pad && o.usePad && o.rumble) {
        double k = std::min(1.0, 0.35 + (d - lastDamage) / 8.0);
        SDL_GameControllerRumble(pad, Uint16(0xffff * k), Uint16(0xc000 * k), Uint32(150 + 250 * k));
      }
      lastDamage = d; damageOf = m.get();
    }

    // grille de départ : la grue repose la voiture près du milieu de la route ; après l'atterrissage, la
    // voiture est placée au milieu de sa moitié de route (gauche ou droite), mesuré sur le tracé du circuit,
    // progressivement (15 unités par image). D'ici là, le décalage restant est ajouté à la position envoyée :
    // les autres voient la grille dès le départ.
    // position en travers de la route (+ : à droite du milieu) et place visée sur la grille
    auto gridLat = [&](double &lat, double &target) -> bool {
      if (trackList.empty()) return false;
      const Track &T = trackList[size_t(m->b(0x1112d) & 7) % trackList.size()];
      const int ns = int(T.secs.size());
      int piece = m->b(0x10906), sec = m->b(0x108f4), first = -1;
      for (int i = 0; i < ns; i++) if (T.secs[size_t(i)].piece == piece) { first = i; break; }
      if (first < 0) return false;
      int g = ((first - 1 + sec) % ns + ns) % ns;
      const Section &A = T.secs[size_t(g)], &B = T.secs[size_t((g + 1) % ns)];
      double f = m->b(0x108f5) / 256.0;
      double lx = A.lx + (B.lx - A.lx) * f, lz = A.lz + (B.lz - A.lz) * f, rx = A.rx + (B.rx - A.rx) * f, rz = A.rz + (B.rz - A.rz) * f;
      double w = std::hypot(rx - lx, rz - lz);
      if (w < 1) return false;
      Pose p = HdView::poseFromState(m->displayedCarState());
      lat = ((p.x - (lx + rx) / 2) * (rx - lx) + (p.z - (lz + rz) / 2) * (rz - lz)) / w;
      target = (gr.slot % 2 ? 1 : -1) * w * 0.28;   // à droite / à gauche, assez loin du bord (bord de la route à 0,5)
      return true;
    };
    auto gridShift = [&]() -> double {   // décalage latéral encore à faire pour être sur sa file
      double lat, target;
      return gridLat(lat, target) ? target - lat : 0;
    };
    // pendant la descente, la grue du jeu amène la voiture de la droite de la route vers le milieu (pose
    // à ~40 à droite du milieu) ; on montre (et on envoie aux autres) cette trajectoire décalée sur sa
    // place, en miroir pour une place à gauche : la voiture arrive de son côté sans traverser les autres.
    // Après l'atterrissage : seulement la part du glissement vers sa place pas encore faite.
    auto craneView = [&]() -> double {
      if (gr.sideDone) return gr.sideTarget - gr.sideApplied;
      double lat, target;
      if (!gridLat(lat, target)) return 0;
      const double LAND = 40, sign = gr.slot % 2 ? 1 : -1;
      return target + sign * (lat - LAND) - lat;
    };
    if (gr.active && gr.racing && !gr.sideDone && m->w(0x109d4) != gr.dropMode) {
      gr.sideDone = true;
      gr.sideTarget = gridShift() + 11;   // la voiture glisse encore de 9 à 13 vers la gauche en se posant (8 circuits)
      if (std::getenv("SCR_GRIDLOG")) std::fprintf(stderr, "grille : place %d, atterrissage à %.1f s, décalage %.0f\n", gr.slot, NetLink::clock() - gr.goAt, gr.sideTarget);
    }
    if (gr.active && gr.racing && gr.sideDone && std::fabs(gr.sideTarget - gr.sideApplied) > 0.5 && m->frames() != gr.sideFrame) {
      gr.sideFrame = m->frames();
      double step = std::clamp(gr.sideTarget - gr.sideApplied, -15.0, 15.0);
      moveCar(0, step);
      gr.sideApplied += step;
    }
    // course à plusieurs : progression sur le circuit (tours, arrivée) et position envoyée aux autres
    if (gr.active && gr.racing && m->ticks() != gr.lastTick && !trackList.empty()) {
      gr.lastTick = m->ticks();
      const Track &T = trackList[size_t(m->b(0x1112d) & 7) % trackList.size()];
      const int ns = int(T.secs.size());
      int piece = m->b(0x10906), sec = m->b(0x108f4), first = -1;
      for (int i = 0; i < ns; i++) if (T.secs[size_t(i)].piece == piece) { first = i; break; }
      if (first >= 0 && ns > 0) {
        int startSec = 0;   // ligne de départ : dernière frontière de la pièce de départ (comme le jeu)
        while (startSec + 1 < ns && T.secs[size_t(startSec + 1)].piece == T.startPiece) startSec++;
        int g = ((first - 1 + sec) % ns + ns) % ns;
        double rel = std::fmod(double(g - startSec + ns), double(ns)) + m->b(0x108f5) / 256.0;
        if (gr.lastRel < 0) gr.lapOffset = rel > ns / 2.0 ? -ns : 0;   // voiture placée derrière la ligne
        else if (gr.lastRel > 0.75 * ns && rel < 0.25 * ns) gr.lapOffset += ns;   // ligne franchie
        else if (gr.lastRel < 0.25 * ns && rel > 0.75 * ns) gr.lapOffset -= ns;   // marche arrière
        gr.lastRel = rel;
        gr.total = gr.lapOffset + rel;
        if (const char *ff = std::getenv("SCR_FAKEFINISH"))   // test : arrivée simulée après N secondes
          if (NetLink::clock() - gr.goAt > std::atof(ff)) gr.total = double(gr.laps) * ns;
        if (!gr.finished && gr.total >= double(gr.laps) * ns) {
          gr.finished = true;
          gr.finishSecs = NetLink::clock() - gr.goAt;
          net.groupCommand("FINISH " + std::to_string(long(gr.finishSecs * 1000)));
        }
        Pose p = HdView::poseFromState(m->displayedCarState());
        {   // part du décalage de grille pas encore appliquée à la voiture : ajoutée à la position envoyée
          double rest = craneView();
          p.x += std::cos(p.yaw) * rest; p.z -= std::sin(p.yaw) * rest;
        }
        float v[7] = {float(p.x), float(p.y), float(p.z), float(p.yaw), float(p.pitch), float(p.roll), float(gr.total / ns)};
        bool boost = false;   // boost : flammes d'origine présentes dans l'image (sprites 6/7/49 et 8/9/50)
        {
          static std::vector<uint32_t> ov(320 * 200);
          std::vector<SpriteDraw> vis;
          m->overlayARGB(ov.data(), &vis);
          for (auto &sd : vis) boost = boost || sd.id == 6 || sd.id == 7 || sd.id == 49 || sd.id == 8 || sd.id == 9 || sd.id == 50;
        }
        {   // choc : numéro (bits 2-3, change à chaque choc) et force / 4 (bits 4-7), répétés dans chaque envoi
          int d = m->b(0x10939) + m->b(0x1093a) + m->b(0x1093b);
          if (gr.dmgPrev >= 0 && d > gr.dmgPrev) { gr.impSeq = (gr.impSeq + 1) & 3; gr.impStr = std::clamp((d - gr.dmgPrev) * 4, 4, 63); }
          gr.dmgPrev = d;
        }
        net.sendGhost(v, uint8_t(std::clamp(int(gr.total / ns) + 1, 0, 255)),
                      uint8_t((gr.finished ? 1 : 0) | (boost ? 2 : 0) | (gr.impSeq << 2) | ((gr.impStr / 4) << 4)));
      }
    }
    // collisions avec les autres voitures (course à plusieurs) : chaque jeu ne déplace que sa propre voiture.
    // Voitures vues comme des rectangles orientés (≈ 148 × 100 unités, modèle 3D de la voiture) ; à
    // chevauchement, la voiture est repoussée de la moitié de l'enfoncement (l'autre jeu fait l'autre
    // moitié) sur l'axe le moins enfoncé, et les vitesses le long de cet axe sont mises en commun
    // (choc mou, léger rebond). Vitesse du jeu ($10AD4 x, $10AD8 z) : déplacement par cycle = v × 0,0145.
    if (gr.active && gr.racing && gr.sideDone && !gr.finished && m->ticks() != gr.collTick) {
      gr.collTick = m->ticks();
      const double now = NetLink::clock();
      if (gr.rateT0 == 0 || now - gr.rateT0 > 2) {   // cycles de physique par seconde (vitesse des autres -> par cycle)
        if (gr.rateT0 > 0 && m->ticks() > gr.rateTicks0) gr.tickRate = std::clamp((m->ticks() - gr.rateTicks0) / (now - gr.rateT0), 4.0, 60.0);
        gr.rateT0 = now; gr.rateTicks0 = m->ticks();
      }
      const double K = 0.0145, LH = 74, WH = 50;
      Pose me = HdView::poseFromState(m->ram + 0x10ac2);
      const double fx = std::sin(me.yaw), fz = std::cos(me.yaw), rx = std::cos(me.yaw), rz = -std::sin(me.yaw);
      auto addL = [&](uint32_t a, double geom) {
        uint32_t v = m->l(a) + uint32_t(int32_t(geom / 16 * 65536));
        m->ram[a] = uint8_t(v >> 24); m->ram[a + 1] = uint8_t(v >> 16); m->ram[a + 2] = uint8_t(v >> 8); m->ram[a + 3] = uint8_t(v);
      };
      auto addW = [&](uint32_t a, double dv) {
        int v = std::clamp(int(std::lround(int16_t(m->w(a)) + dv)), -32000, 32000);
        m->ram[a] = uint8_t(uint16_t(v) >> 8); m->ram[a + 1] = uint8_t(v);
      };
      for (auto &kv : net.ghosts()) {
        const auto &q = kv.second;
        if (q.empty() || now - q.back().t > 1) continue;
        const NetLink::GhostSample &b = q.back(), &a = q.size() > 1 ? q[q.size() - 2] : q.back();
        const double dt = b.t - a.t;
        const double vgx = dt > 0.01 ? (b.v[0] - a.v[0]) / dt : 0, vgz = dt > 0.01 ? (b.v[2] - a.v[2]) / dt : 0;   // unités / s
        const double ext = std::clamp(now - b.t + 0.03, 0.0, 0.25);   // position estimée maintenant (retard du réseau)
        const double gx = b.v[0] + vgx * ext, gz = b.v[2] + vgz * ext;
        if (std::fabs(b.v[1] - me.y) * 0.522 > 70) continue;   // pas à la même hauteur (saut, route au-dessus)
        const double dx = gx - me.x, dz = gz - me.z;
        if (dx * dx + dz * dz > 400 * 400) continue;
        const double df = dx * fx + dz * fz, ds = dx * rx + dz * rz;
        const double d = std::remainder(b.v[3] - me.yaw, 2 * M_PI);
        const double ef = LH * std::fabs(std::cos(d)) + WH * std::fabs(std::sin(d)), es = LH * std::fabs(std::sin(d)) + WH * std::fabs(std::cos(d));
        const double of = LH + ef - std::fabs(df), os = WH + es - std::fabs(ds);
        if (of <= 0 || os <= 0) continue;
        double nx, nz, pen;   // normale du choc, de ma voiture vers l'autre
        if (of < os) { double sg = df > 0 ? 1 : -1; nx = fx * sg; nz = fz * sg; pen = of; }
        else { double sg = ds > 0 ? 1 : -1; nx = rx * sg; nz = rz * sg; pen = os; }
        const double push = std::min(pen * 0.5 + 1, 40.0);
        addL(0x10ac2, -nx * push); addL(0x10aca, -nz * push);
        const double vmx = int16_t(m->w(0x10ad4)) * K, vmz = int16_t(m->w(0x10ad8)) * K;   // unités / cycle
        const double vn = vmx * nx + vmz * nz, vo = (vgx * nx + vgz * nz) / gr.tickRate;
        if (vn > vo) {   // on se rapproche : vitesses mises en commun, petit rebond
          const double target = (vn + vo) / 2 - 0.15 * (vn - vo), dv = target - vn;
          addW(0x10ad4, dv * nx / K); addW(0x10ad8, dv * nz / K);
          const double closing = (vn - vo) * gr.tickRate;   // unités / s
          if (closing > 300 && view && now - gr.lastSpark > 0.25) {   // choc léger : quelques étincelles au contact
            gr.lastSpark = now;
            view->impactAt(me.x + nx * WH, me.y / 0.5, me.z + nz * WH, int(std::min(30.0, closing / 100)));
          }
          if (closing > 1200 && now - gr.lastHit > 0.5) {   // choc violent : dégâts, comme une mauvaise réception
            gr.lastHit = now;
            int dmg = std::clamp(int((closing - 1200) / 150), 1, 24);
            for (uint32_t ad : {0x10939u, 0x1093au, 0x1093bu}) m->ram[ad] = uint8_t(std::min(255, m->b(ad) + dmg));
          }
        }
        if (std::getenv("SCR_COLLLOG")) std::fprintf(stderr, "choc avec %u : enfoncement %.0f (%s), vitesse %.1f -> autre %.1f\n", kv.first, pen, of < os ? "avant/arrière" : "côté", vn, vo);
      }
    }
    // voiture détruite : le jeu lance le compte à rebours de fin ($10956) et arrête la physique ; le joueur abandonne
    if (gr.active && gr.racing && !gr.finished && gr.sideDone) {
      if (const char *fh = std::getenv("SCR_FAKEHIT")) {   // test : choc simulé (dégâts) après N secondes
        static bool hit = false;
        if (!hit && NetLink::clock() - gr.goAt > std::atof(fh)) { hit = true; m->ram[0x10939] = uint8_t(m->b(0x10939) + 6); }
      }
      if (const char *fw = std::getenv("SCR_FAKEWRECK"))   // test : casse simulée après N secondes
        if (NetLink::clock() - gr.goAt > std::atof(fw) && !m->b(0x10956)) m->ram[0x10956] = 0x3c;
      gr.wreckFrames = m->b(0x10956) != 0 ? gr.wreckFrames + 1 : 0;
      if (gr.wreckFrames > 25) {
        gr.finished = gr.out = true;
        gr.finishSecs = NetLink::clock() - gr.goAt;
        net.groupCommand("OUT");
      }
    }

    // bandeau du multijoueur (visible aussi en plein écran) : saisie du code, état de la connexion
    std::string bannerMsg;
    {
      static std::string lastStatus;
      static double statusSince = 0;
      std::string st = net.status();
      if (st != lastStatus) { lastStatus = st; statusSince = tNow; }
      if (menuOpen) bannerMsg.clear();   // le menu affiche lui-même l'état
      else if (NetLink::clock() < noteUntil) bannerMsg = note;
      else if (!st.empty() && (net.state() != NetLink::State::Connected || tNow - statusSince < 6))
        bannerMsg = st + (net.state() == NetLink::State::Failed ? "   (F10 : effacer)" : "");
    }
    auto drawBanner = [&](int logicalW) {
      static std::string cachedMsg; static int cachedScale = 0, bw = 0, bh = 0;
      static SDL_Texture *btex = nullptr;
      if (bannerMsg.empty()) return;
      int scale = std::max(1, logicalW / 640);
      if (bannerMsg != cachedMsg || scale != cachedScale || !btex) {
        if (btex) SDL_DestroyTexture(btex);
        auto img = renderBanner(bannerMsg, scale, bw, bh);
        btex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, bw, bh);
        SDL_SetTextureBlendMode(btex, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(btex, nullptr, img.data(), bw * 4);
        cachedMsg = bannerMsg; cachedScale = scale;
      }
      int rw = std::min(bw, logicalW), rh = bw > logicalW ? bh * logicalW / bw : bh;   // réduit sans déformer
      SDL_Rect r{(logicalW - rw) / 2, 0, rw, rh};
      SDL_RenderCopy(ren, btex, nullptr, &r);
    };
    // texte dessiné à une position (cache de textures) ; align : 0 gauche, 1 centre, 2 droite
    auto drawText = [&](const std::string &txt, int x, int y, int scale, uint32_t fg, uint32_t bg, int align, int logicalW) {
      static std::map<std::string, std::tuple<SDL_Texture *, int, int>> cache;
      if (cache.size() > 120) { for (auto &kv : cache) SDL_DestroyTexture(std::get<0>(kv.second)); cache.clear(); }
      std::string key = txt + "|" + std::to_string(fg) + "|" + std::to_string(bg) + "|" + std::to_string(scale);
      auto it = cache.find(key);
      if (it == cache.end()) {
        int bw, bh;
        auto img = renderBanner(txt, scale, bw, bh, fg, bg);
        SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, bw, bh);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(t, nullptr, img.data(), bw * 4);
        it = cache.emplace(key, std::make_tuple(t, bw, bh)).first;
      }
      auto [t, bw, bh] = it->second;
      int rw = std::min(bw, logicalW), rh = bw > logicalW ? bh * logicalW / bw : bh;
      SDL_Rect r{align == 0 ? x : align == 1 ? x - rw / 2 : x - rw, y, rw, rh};
      SDL_RenderCopy(ren, t, nullptr, &r);
      return rh;
    };
    // course à plusieurs : départ, tour, temps, classement en direct, noms au-dessus des voitures
    auto drawRaceHud = [&](int logicalW, int logicalH) {
      if (!gr.active || !net.inGroup() || menuOpen) return;
      int scale = std::max(1, logicalW / 640), big = std::max(2, logicalW / 240);
      double now = NetLink::clock();
      const bool placed = gr.sideDone && std::fabs(gr.sideTarget - gr.sideApplied) <= 0.5;
      const std::string place = "Grille : rangée " + std::to_string(gr.slot / 2 + 1) + (gr.slot % 2 ? ", à droite" : ", à gauche") +
                                (placed ? "" : " (placement à la fin de la descente de la grue)");
      if (gr.waitingGo) {
        int yy = logicalH / 3;
        yy += drawText("Course à plusieurs : départ dès que tous les joueurs sont prêts...", logicalW / 2, yy, scale,
                       0xffffffffu, 0xc0000000u, 1, logicalW);
        drawText(place, logicalW / 2, yy + 2 * scale, scale, 0xffffff60u, 0xc0000000u, 1, logicalW);
        return;
      }
      if (gr.racing && now - gr.goAt < 15)   // place sur la grille, rappelée pendant la descente de la grue
        drawText(place, logicalW / 2, logicalH / 3 + 20 * scale, scale, 0xffffff60u, 0xc0000000u, 1, logicalW);
      for (auto &g : view->ghosts)   // noms au-dessus des voitures (HD)
        if (hd && g.onScreen && g.dist < 20000)
          drawText(g.name, int(g.sx), int(g.sy) - 10 * scale, scale, 0xffffff60u, 0x90000000u, 1, logicalW);
      if (gr.racing && now - gr.goAt < 2) drawText("GO !", logicalW / 2, logicalH / 4, big, 0xff60ff60u, 0x80000000u, 1, logicalW);
      // classement : arrivés (par temps), puis par distance parcourue
      struct Row { std::string name; double total; long ms; bool me; bool out; };
      std::vector<Row> rows;
      const int ns = trackList.empty() ? 1 : int(trackList[size_t(gr.track) % trackList.size()].secs.size());
      for (auto &gm : net.groupMembers()) {
        Row r{gm.name, -1e9, -1, gm.id == net.groupMe(), gm.state == "out"};
        if (gm.state.rfind("finished:", 0) == 0) r.ms = std::atol(gm.state.c_str() + 9);
        if (r.me) r.total = gr.total / std::max(1, ns);
        else { auto it = net.ghosts().find(gm.id); if (it != net.ghosts().end() && !it->second.empty()) r.total = it->second.back().v[6]; }
        if (r.me && gr.out) r.out = true;
        if (r.me && gr.finished && !gr.out && r.ms < 0) r.ms = long(gr.finishSecs * 1000);
        rows.push_back(r);
      }
      std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.out != b.out) return b.out;   // abandons en dernier
        if ((a.ms >= 0) != (b.ms >= 0)) return a.ms >= 0;
        if (a.ms >= 0) return a.ms < b.ms;
        return a.total > b.total;
      });
      auto fmt = [](double secs) { char b[32]; int m = int(secs) / 60; std::snprintf(b, sizeof b, "%d:%04.1f", m, secs - m * 60); return std::string(b); };
      int y = 2 * scale;
      double el = gr.finished ? gr.finishSecs : now - gr.goAt;
      int lap = std::clamp(int(std::floor(gr.total / std::max(1, ns))) + 1, 1, gr.laps);
      y += drawText(gr.out ? std::string("Abandon : voiture détruite") : gr.finished ? "Arrivé en " + fmt(gr.finishSecs) : "Tour " + std::to_string(lap) + "/" + std::to_string(gr.laps) + "   " + fmt(el),
                    logicalW - 2 * scale, y, scale, 0xffffffffu, 0xc0000000u, 2, logicalW);
      for (size_t k = 0; k < rows.size(); k++) {
        const Row &r = rows[k];
        std::string t = std::to_string(k + 1) + ". " + r.name.substr(0, 14);
        if (r.out) t += "  abandon";
        else if (r.ms >= 0) t += "  " + fmt(r.ms / 1000.0);
        else if (r.total > -1e8) t += "  tour " + std::to_string(std::clamp(int(std::floor(r.total)) + 1, 1, gr.laps));
        y += drawText(t, logicalW - 2 * scale, y, scale, r.me ? 0xffa0e0ffu : 0xffffffa0u, 0xa0000000u, 2, logicalW);
      }
    };
    // discussion hors du menu (pendant le jeu) : derniers messages en bas de l'image, saisie (F9)
    auto drawChat = [&](int logicalW, int logicalH) {
      if (menuOpen) return;   // dans le menu, la discussion est dans le panneau
      double now = NetLink::clock();
      std::vector<std::pair<std::string, uint32_t>> ls;
      int scale = std::max(1, logicalW / 640);
      size_t cols = size_t(std::max(20, logicalW / (6 * scale) - 2));
      for (auto &c : net.chat())
        if (now - c.t < 10 || typing == Typing::Chat)
          for (auto &w : wrapText(c.from + " : " + c.text, cols)) ls.push_back({w, c.mine ? 0xffa0e0ffu : 0xffffffa0u});
      if (ls.size() > 5) ls.erase(ls.begin(), ls.end() - 5);
      if (typing == Typing::Chat)
        for (auto &w : wrapText("Message (Entrée : envoyer, Échap : fermer) : " + typed + "_", cols)) ls.push_back({w, 0xffffffffu});
      if (ls.empty()) return;
      static std::map<std::string, std::tuple<SDL_Texture *, int, int>> cache;
      if (cache.size() > 60) { for (auto &kv : cache) SDL_DestroyTexture(std::get<0>(kv.second)); cache.clear(); }
      int lineH = 13 * scale, y = logicalH - int(ls.size()) * lineH - 2 * scale;
      for (auto &l : ls) {
        std::string key = l.first + "|" + std::to_string(l.second) + "|" + std::to_string(scale);
        auto it = cache.find(key);
        if (it == cache.end()) {
          int bw, bh;
          auto img = renderBanner(l.first, scale, bw, bh, l.second, 0xc0000000u);
          SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, bw, bh);
          SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
          SDL_UpdateTexture(t, nullptr, img.data(), bw * 4);
          it = cache.emplace(key, std::make_tuple(t, bw, bh)).first;
        }
        auto [t, bw, bh] = it->second;
        int rw = std::min(bw, logicalW), rh = bw > logicalW ? bh * logicalW / bw : bh;
        SDL_Rect r{0, y, rw, rh};
        SDL_RenderCopy(ren, t, nullptr, &r);
        y += lineH;
      }
    };
    // menu « Computer Link », dessiné par-dessus le jeu (même allure que les menus d'origine)
    auto drawMenu = [&](int logicalW, int logicalH) {
      if (!menuOpen) return;
      static std::map<std::string, std::tuple<SDL_Texture *, int, int>> cache;
      if (cache.size() > 80) { for (auto &kv : cache) SDL_DestroyTexture(std::get<0>(kv.second)); cache.clear(); }
      int scale = std::max(1, logicalW / 400);
      struct Line { std::string text; uint32_t fg, bg; };
      std::vector<Line> lines;
      lines.push_back({net.inGroup() ? "COURSE A PLUSIEURS - SALLE " + net.room()
                       : online() ? "ONLINE MULTIPLAYER - SERVEUR RELAIS" : "COMPUTER LINK - RESEAU LOCAL (LAN)", 0xffffff00u, 0xe0202020u});
      std::string st = net.status();
      std::vector<Line> groupLines;
      if (net.inGroup()) {   // joueurs du groupe (avec les temps de la dernière course)
        std::string hostName;
        for (auto &gm : net.groupMembers()) if (gm.id == net.groupHost()) hostName = gm.name;
        bool host = net.groupHost() == net.groupMe();
        st = net.groupPhase() != "lobby" ? (gr.out ? "Abandon (voiture détruite). Attente des autres..."
                                                : gr.finished ? "Arrivé ! En attente des autres joueurs..." : "Course en cours...")
             : host ? (net.groupMembers().size() < 2 ? "En attente d'autres joueurs (2 à 6)" : "Choisissez le circuit puis lancez")
                    : "En attente du départ donné par " + hostName;
        std::vector<NetLink::GroupMember> ms = net.groupMembers();
        std::stable_sort(ms.begin(), ms.end(), [](const NetLink::GroupMember &a, const NetLink::GroupMember &b) {
          bool fa = a.state.rfind("finished:", 0) == 0, fb = b.state.rfind("finished:", 0) == 0;
          if (fa != fb) return fa;
          return fa && std::atol(a.state.c_str() + 9) < std::atol(b.state.c_str() + 9);
        });
        int rank = 0;
        for (auto &gm : ms) {
          std::string t = gm.name.substr(0, 18);
          if (gm.state.rfind("finished:", 0) == 0) {
            long msv = std::atol(gm.state.c_str() + 9);
            char b[48]; std::snprintf(b, sizeof b, "%d. ", ++rank); t = b + t;
            std::snprintf(b, sizeof b, "  %ld:%04.1f", msv / 60000, (msv % 60000) / 1000.0); t += b;
          } else t = "-  " + t + (gm.state == "out" ? "  abandon" : "");
          if (gm.id == net.groupHost()) t += "  (organisateur)";
          groupLines.push_back({t, gm.id == net.groupMe() ? 0xffa0e0ffu : 0xffffffffu, 0xe0202020u});
        }
      }
      uint32_t stCol = 0xffffffffu;
      if (online() && (!net.active() || net.state() == NetLink::State::Failed)) {
        if (net.state() == NetLink::State::Failed) lines.push_back({st, 0xffff8080u, 0xe0202020u});
        st = probe.text();
        stCol = probe.state() == RelayProbe::State::Ok ? 0xff80ff80u : probe.state() == RelayProbe::State::Failed ? 0xffff8080u : 0xffffffffu;
        if (probe.ok() && !probe.params().empty()) {   // réglages que le serveur imposera pendant la partie
          lines.push_back({st, stCol, 0xe0202020u});
          st = "Réglages du serveur : " + paramsSummary(probe.params());
          stCol = 0xffc0c0c0u;
        }
        lines.push_back({"Serveur : " + (o.relay.empty() ? std::string(DEFAULT_RELAY_NAME) : o.relay),
                         0xffc0c0c0u, 0xe0202020u});
        if (probe.ok() && newerVersion(probe.version()))
          lines.push_back({"Nouvelle version " + probe.version() + " disponible (vous : " SCR_VERSION ")", 0xffffc040u, 0xe0202020u});
      }
      if (net.mode() == NetLink::Mode::Lobby && net.state() == NetLink::State::Waiting)
        st = net.peers().empty() ? "Recherche de joueurs sur le réseau local..." : "Joueurs trouvés sur le réseau local :";
      if (net.relayRoom() && net.state() == NetLink::State::Waiting && !net.inGroup())
        st = net.peers().empty() ? "Salle " + net.room() + " : personne d'autre pour l'instant"
                                 : "Salle " + net.room() + " : joueurs présents";
      lines.push_back({st, stCol, 0xe0202020u});
      lines.insert(lines.end(), groupLines.begin(), groupLines.end());
      bool lobbyWait = net.mode() == NetLink::Mode::Lobby && net.state() == NetLink::State::Waiting;
      if (lobbyWait) {   // adresse de ce PC (pour « Par adresse IP » si la détection ne passe pas)
        static std::string addrs; static double addrsAt = -1e9;
        double tn = double(SDL_GetTicks()) / 1000.0;
        if (tn - addrsAt > 5) {
          addrsAt = tn; addrs.clear();
          for (auto &a : net.localAddresses()) {
            if (addrs.size() > 30) break;
            addrs += (addrs.empty() ? "" : "  ") + a;
          }
        }
        if (!addrs.empty()) lines.push_back({"Ce PC : " + addrs, 0xffc0c0c0u, 0xe0202020u});
        if (net.peers().empty() && net.sinceStart() > 8)
          lines.push_back({"Personne ? Pare-feu : autoriser scr.exe", 0xffffc040u, 0xe0202020u});
      }
      auto items = menuItems();
      if (menuSel >= int(items.size())) menuSel = int(items.size()) - 1;
      for (size_t k = 0; k < items.size(); k++) {
        std::string lab = items[k].label;
        if (typing != Typing::None && typing != Typing::Chat && int(k) == menuSel)
          lab = (typing == Typing::Code ? "Code de salle : " : typing == Typing::Server ? "Serveur : "
                 : typing == Typing::Pseudo ? "Pseudo : " : "Adresse : ") + typed + "_";
        bool sel = int(k) == menuSel;
        lines.push_back({lab, sel ? 0xff000000u : 0xff202020u, sel ? 0xffff9a00u : 0xffb6b6b6u});
      }
      if (net.canChat() || !net.chat().empty()) {   // discussion de la salle (3 derniers messages)
        const auto &ch = net.chat();
        std::vector<Line> cl;
        for (size_t k = ch.size() > 3 ? ch.size() - 3 : 0; k < ch.size(); k++)
          for (auto &w : wrapText(ch[k].from + " : " + ch[k].text, 44))
            cl.push_back({w, ch[k].mine ? 0xffa0e0ffu : 0xffffffa0u, 0xe0101830u});
        if (cl.size() > 4) cl.erase(cl.begin(), cl.end() - 4);   // au plus 4 lignes dans le panneau
        lines.insert(lines.end(), cl.begin(), cl.end());
        if (typing == Typing::Chat) {
          auto in = wrapText("Message : " + typed + "_", 44);
          for (size_t k = in.size() > 2 ? in.size() - 2 : 0; k < in.size(); k++) lines.push_back({in[k], 0xffffffffu, 0xe0303860u});
        }
      }
      lines.push_back({typing == Typing::Chat ? "Entrée : envoyer     Échap : fermer"
                       : typing == Typing::Code ? "Entrée : valider (vide = code au hasard)"
                       : typing == Typing::Ip ? "Entrée : valider     Échap : retour"
                       : typing == Typing::Server ? "Adresse[:port], Entrée : enregistrer"
                       : typing == Typing::Pseudo ? "12 caractères, Entrée : enregistrer"
                                              : "Flèches + Entrée : choisir  Échap : annuler", 0xffc0c0c0u, 0xe0202020u});
      int lineH = 15 * scale, total = int(lines.size()) * lineH;
      int y = std::max(0, (logicalH - total) / 2);
      {   // panneau opaque gris (couleur des menus d'origine) qui masque le menu du jeu en dessous
        int pw = std::min(logicalW, (44 * 6 + 6) * scale + 16 * scale), ph = total + 12 * scale;
        SDL_Rect pr{(logicalW - pw) / 2, std::max(0, y - 6 * scale), pw, ph};
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(ren, 0x48, 0x48, 0x48, 255);
        SDL_RenderFillRect(ren, &pr);
        SDL_SetRenderDrawColor(ren, 0x24, 0x24, 0x24, 255);
        SDL_RenderDrawRect(ren, &pr);
      }
      for (auto &ln : lines) {
        std::string key = ln.text + "|" + std::to_string(ln.fg) + "|" + std::to_string(ln.bg) + "|" + std::to_string(scale);
        auto it = cache.find(key);
        if (it == cache.end()) {
          int bw, bh;
          auto img = renderBanner(ln.text, scale, bw, bh, ln.fg, ln.bg, 44);
          SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, bw, bh);
          SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
          SDL_UpdateTexture(t, nullptr, img.data(), bw * 4);
          it = cache.emplace(key, std::make_tuple(t, bw, bh)).first;
        }
        auto [t, bw, bh] = it->second;
        int rw = std::min(bw, logicalW), rh = bw > logicalW ? bh * logicalW / bw : bh;
        SDL_Rect r{(logicalW - rw) / 2, y, rw, rh};
        SDL_RenderCopy(ren, t, nullptr, &r);
        y += lineH;
      }
    };
    // menu d'options (F8), même allure que le menu du multijoueur
    auto drawOptions = [&](int logicalW, int logicalH) {
      if (!optOpen) return;
      static std::map<std::string, std::tuple<SDL_Texture *, int, int>> cache;
      if (cache.size() > 80) { for (auto &kv : cache) SDL_DestroyTexture(std::get<0>(kv.second)); cache.clear(); }
      auto items = optItems();
      int scale = std::max(1, std::min(logicalW / 400, logicalH / ((int(items.size()) + 2) * 13 + 16)));
      struct Line { std::string text; uint32_t fg, bg; };
      std::vector<Line> lines;
      lines.push_back({"OPTIONS" + std::string(!net.active() && !gr.active ? "  (jeu en pause)" : ""), 0xffffff00u, 0xe0202020u});
      optSel = std::clamp(optSel, 0, int(items.size()) - 1);
      for (size_t k = 0; k < items.size(); k++) {
        bool sel = int(k) == optSel;
        lines.push_back({items[k].label, sel ? 0xff000000u : 0xff202020u, sel ? 0xffff9a00u : 0xffb6b6b6u});
      }
      lines.push_back({optBind >= 0 ? "Nouvelle touche ?   (Échap : annuler)"
                                    : "Flèches + Entrée : changer   Échap : fermer", 0xffc0c0c0u, 0xe0202020u});
      int lineH = 13 * scale, total = int(lines.size()) * lineH;
      int y = std::max(0, (logicalH - total) / 2);
      {
        int pw = std::min(logicalW, (44 * 6 + 6) * scale + 16 * scale), ph = total + 12 * scale;
        SDL_Rect pr{(logicalW - pw) / 2, std::max(0, y - 6 * scale), pw, ph};
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(ren, 0x48, 0x48, 0x48, 255);
        SDL_RenderFillRect(ren, &pr);
        SDL_SetRenderDrawColor(ren, 0x24, 0x24, 0x24, 255);
        SDL_RenderDrawRect(ren, &pr);
      }
      for (auto &ln : lines) {
        std::string key = ln.text + "|" + std::to_string(ln.fg) + "|" + std::to_string(ln.bg) + "|" + std::to_string(scale);
        auto it = cache.find(key);
        if (it == cache.end()) {
          int bw, bh;
          auto img = renderBanner(ln.text, scale, bw, bh, ln.fg, ln.bg, 44);
          SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, bw, bh);
          SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
          SDL_UpdateTexture(t, nullptr, img.data(), bw * 4);
          it = cache.emplace(key, std::make_tuple(t, bw, bh)).first;
        }
        auto [t, bw, bh] = it->second;
        int rw = std::min(bw, logicalW), rh = bw > logicalW ? bh * logicalW / bw : bh;
        SDL_Rect r{(logicalW - rw) / 2, y, rw, rh};
        SDL_RenderCopy(ren, t, nullptr, &r);
        y += lineH;
      }
    };
    {   // titre de la fenêtre : état du réseau (mode d'origine ; en HD, avec le compteur d'images)
      static double lastTitle = 0;
      if (!hd && tNow - lastTitle > 0.5) {
        lastTitle = tNow;
        std::string t = std::string("Stunt Car Racer " SCR_VERSION) + (net.active() ? " - " + net.status() : "");
        SDL_SetWindowTitle(win, t.c_str());
      }
    }

    if (hd) {
      // une image par rafraîchissement de l'écran (la synchro verticale cadence la boucle ; sinon limiteur)
      uint64_t t = SDL_GetPerformanceCounter();
      if (lastPresent && double(t - lastPresent) / double(freq) < 0.9 / refresh) { SDL_Delay(1); continue; }
      lastPresent = t;
      view->ghosts.clear();
      if (gr.active && net.inGroup()) {   // autres joueurs : position interpolée 0,3 s en arrière
        const double tt = NetLink::clock() - 0.3;
        for (auto &kv : net.ghosts()) {
          const auto &q = kv.second;
          if (q.empty() || NetLink::clock() - q.back().t > 3) continue;
          const NetLink::GhostSample *a = &q.front(), *b = &q.back();
          for (size_t k = 0; k + 1 < q.size(); k++)
            if (q[k].t <= tt && q[k + 1].t >= tt) { a = &q[k]; b = &q[k + 1]; break; }
          double f = (b->t > a->t && tt > a->t) ? std::min(1.0, (tt - a->t) / (b->t - a->t)) : (tt >= b->t ? 1.0 : 0.0);
          auto lerpA = [&](double x, double y) { double d = std::remainder(y - x, 2 * M_PI); return x + d * f; };
          Pose ps{a->v[0] + (b->v[0] - a->v[0]) * f, a->v[1] + (b->v[1] - a->v[1]) * f, a->v[2] + (b->v[2] - a->v[2]) * f,
                  lerpA(a->v[3], b->v[3]), lerpA(a->v[4], b->v[4]), lerpA(a->v[5], b->v[5])};
          HdView::Ghost g;
          g.pose = view->ghostFromPose(ps);
          g.boost = (b->flags & 2) != 0;
          g.impactStamp = (q.back().flags >> 2) & 3;
          g.impact = (q.back().flags >> 4) * 4;
          g.id = kv.first;
          for (auto &gm : net.groupMembers()) if (gm.id == kv.first) g.name = gm.name;
          view->ghosts.push_back(g);
        }
      }
      view->viewSide = gr.active ? craneView() : 0;   // course à plusieurs : grue du côté de sa place
      view->render(*m, view->now() + acc / framePeriod, hdPixels.data(), o.hdW, o.hdH);
      SDL_UpdateTexture(texHd, nullptr, hdPixels.data(), o.hdW * 4);
      SDL_RenderClear(ren);
      SDL_RenderCopy(ren, texHd, nullptr, nullptr);
      drawBanner(o.hdW);
      drawMenu(o.hdW, o.hdH);
      drawRaceHud(o.hdW, o.hdH);
      drawChat(o.hdW, o.hdH);
      drawOptions(o.hdW, o.hdH);
      if (gr.active && gr.goAt > 0 && std::getenv("SCR_RACESHOT")) {   // test : captures « secondes:fichier » après le départ
        static std::vector<std::pair<double, std::string>> rs;
        static bool parsed = false;
        if (!parsed) { parsed = true; parseAuto(std::getenv("SCR_RACESHOT"), [&](uint32_t f, const std::string &k) { rs.push_back({double(f), k}); }); }
        for (size_t k = 0; k < rs.size();)
          if (NetLink::clock() - gr.goAt >= rs[k].first) {
            autoShots.push_back({0, rs[k].second}); rs.erase(rs.begin() + long(k));
            Pose me = HdView::poseFromState(m->displayedCarState());
            std::fprintf(stderr, "t=%.0f s  moi : x=%.0f z=%.0f place %d appliqué %.0f/%.0f reste %.0f", NetLink::clock() - gr.goAt, me.x, me.z,
                         gr.slot, gr.sideApplied, gr.sideTarget, gridShift());
            for (auto &kv : net.ghosts()) if (!kv.second.empty()) std::fprintf(stderr, "  | autre : x=%.0f z=%.0f", kv.second.back().v[0], kv.second.back().v[2]);
            std::fprintf(stderr, "\n");
          } else k++;
      }
      for (size_t k = 0; k < autoShots.size();)   // captures de test (SCR_SHOT), aussi en HD
        if (m->frames() >= autoShots[k].first) {
          int w, h; SDL_GetRendererOutputSize(ren, &w, &h);
          SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
          SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch);
          SDL_SaveBMP(sf, autoShots[k].second.c_str()); SDL_FreeSurface(sf);
          autoShots.erase(autoShots.begin() + long(k));
        } else k++;
      SDL_RenderPresent(ren);
      // compteur d'images par seconde dans le titre
      fpsN++;
      double el = double(SDL_GetPerformanceCounter() - fpsT0) / double(freq);
      if (el >= 1) {
        char title[400];
        std::snprintf(title, sizeof title, "Stunt Car Racer " SCR_VERSION " - HD %dx%d%s - %.0f i/s%s%s", o.hdW,
                      o.hdH, highDetail ? " - détails élevés (F5)" : "", fpsN / el,
                      net.active() ? " - " : "", net.active() ? net.status().c_str() : "");
        SDL_SetWindowTitle(win, title);
        if (fpsLog) std::fprintf(stderr, "%s\n", title);
        fpsN = 0; fpsT0 = SDL_GetPerformanceCounter();
      }
    } else {
      // sans synchro verticale (multijoueur) : une image par rafraîchissement de l'écran au plus
      static uint64_t lastLow = 0;
      uint64_t tl = SDL_GetPerformanceCounter();
      if (parts && lastLow && double(tl - lastLow) / double(freq) < 0.9 / refresh) { SDL_Delay(1); continue; }
      lastLow = tl;
      m->screenARGB(pixels);
      SDL_UpdateTexture(tex, nullptr, pixels, 320 * 4);
      SDL_RenderClear(ren);
      SDL_RenderCopy(ren, tex, nullptr, nullptr);
      drawBanner(320);
      drawMenu(320, 200);
      drawRaceHud(320, 200);
      drawChat(320, 200);
      if (optOpen) {   // le menu d'options ne tient pas en 320×200 : dessiné en 640×400
        SDL_RenderSetLogicalSize(ren, 640, 400);
        drawOptions(640, 400);
        SDL_RenderSetLogicalSize(ren, 320, 200);
      }
      for (size_t k = 0; k < autoShots.size();)
        if (m->frames() >= autoShots[k].first) {
          int w, h; SDL_GetRendererOutputSize(ren, &w, &h);
          SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
          SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch);
          SDL_SaveBMP(sf, autoShots[k].second.c_str()); SDL_FreeSurface(sf);
          autoShots.erase(autoShots.begin() + long(k));
        } else k++;
      SDL_RenderPresent(ren);
      if (!n) SDL_Delay(1);
    }
  }
  if (pad) SDL_GameControllerClose(pad);
  SDL_DestroyTexture(tex);
  if (texHd) SDL_DestroyTexture(texHd);
  if (audio) SDL_CloseAudioDevice(audio);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
