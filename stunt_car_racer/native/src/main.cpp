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
//           circuit précédent/suivant (entraînement), F5 détails élevés, F6 recommencer.
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
#include <sstream>
#include <string>
#include <vector>

#define SCR_VERSION "v18"

#include "banner.hpp"
#include "hdview.hpp"
#include "machine.hpp"
#include "net.hpp"
#include <chrono>
#include <random>

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
  // multijoueur (câble « Computer Link » par le réseau)
  std::string relay;       // serveur relais : adresse[:port]
  std::string playerName;  // nom montré à l'autre joueur
  std::string netMode, netArg;   // au démarrage : host | joinlan | join ADRESSE | online CODE
  double speed = 1;
  Tuning tuning;
};

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
  else if (key == "nom" || key == "name") o.playerName = val;
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

// dossier des images HD : option hddir, sinon hd/ à côté de l'exécutable ou dans le dossier courant
std::string findHdDir(const std::string &opt) {
  namespace fs = std::filesystem;
  if (!opt.empty()) return opt;
  std::error_code ec;
  for (auto &d : baseDirs())
    if (fs::is_directory(d / "hd", ec)) return (d / "hd").u8string();
  return {};
}

std::unique_ptr<Machine> start(const Bytes &disk, const Options &o, int track) {
  auto m = std::make_unique<Machine>(disk);
  m->setTuning(o.tuning);
  if (track < 0) m->coldStart();
  else m->startPractice(track);
  return m;
}

}  // namespace

int main(int argc, char **argv) {
  Options o;
  std::string ini = "scr.ini";
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
  try {
    disk = readFile(o.disk);
    m = start(disk, o, track);
  } catch (std::exception &e) {
    fail(std::string("Erreur : ") + e.what());
    return 1;
  }

  // --- multijoueur : le câble série du jeu passe par le réseau
  NetLink net;
  {
    uint32_t h = fnv1a(m->image().data(), m->image().size());
    h = fnv1a(&o.tuning, sizeof o.tuning, h);   // mêmes réglages de physique des deux côtés
    std::string nm = o.playerName;
    if (nm.empty()) { const char *u = std::getenv("USERNAME"); if (!u) u = std::getenv("USER"); nm = u ? u : "Joueur"; }
    net.setIdentity(SCR_VERSION, h, nm);
  }
  auto wireNet = [&] {
    m->serialOut = [&](uint8_t v) { net.send(v); };
    m->sliceHook = [&] { net.poll(); uint8_t b; while (net.recv(b)) m->serialPush(b); };
  };
  wireNet();
  if (o.netMode == "host") net.hostLan();
  else if (o.netMode == "joinlan") net.joinLan();
  else if (o.netMode == "join") net.joinDirect(o.netArg);
  else if (o.netMode == "online") net.joinRelay(o.relay, o.netArg);
  bool typingCode = false;       // saisie du code de salle (F9)
  std::string code;
  double fireUntil = 0;          // en multijoueur, un appui sur « feu » dure au moins 0,35 s

  std::unique_ptr<HdView> view;
  bool hd = o.hd, highDetail = o.highDetail;
  std::string hdDir = findHdDir(o.hdDir);
  int hdImages = 0;
  auto newView = [&] {
    view = std::make_unique<HdView>(*m);
    view->params.flameImages = o.hdFlames;
    view->params.highDetail = highDetail;
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
  while (running) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) running = false;
      else if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(ev.cdevice.which);
      else if (ev.type == SDL_TEXTINPUT && typingCode) {   // code de salle : lettres et chiffres
        for (const char *c = ev.text.text; *c; c++)
          if (std::isalnum(static_cast<unsigned char>(*c)) && code.size() < 12) code += char(std::toupper(static_cast<unsigned char>(*c)));
      }
      else if (ev.type == SDL_KEYDOWN && typingCode) {
        SDL_Scancode sc = ev.key.keysym.scancode;
        if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_F9) { typingCode = false; SDL_StopTextInput(); }
        else if (sc == SDL_SCANCODE_BACKSPACE && !code.empty()) code.pop_back();
        else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
          if (code.empty()) {   // pas de code : on en invente un à donner à l'autre joueur
            std::mt19937 rng(unsigned(std::chrono::steady_clock::now().time_since_epoch().count()));
            code = std::to_string(1000 + rng() % 9000);
          }
          typingCode = false; SDL_StopTextInput();
          net.joinRelay(o.relay, code);
        }
      }
      else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
        bool down = ev.type == SDL_KEYDOWN;
        SDL_Scancode sc = ev.key.keysym.scancode;
        keys[sc] = down;
        if (down && !ev.key.repeat) {   // multijoueur
          if (sc == SDL_SCANCODE_F7) net.hostLan();
          else if (sc == SDL_SCANCODE_F8) net.joinLan();
          else if (sc == SDL_SCANCODE_F9) { typingCode = true; code.clear(); SDL_StartTextInput(); }
          else if (sc == SDL_SCANCODE_F10) net.close("déconnexion");
        }
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
          } else if (sc == SDL_SCANCODE_F6 || ((sc == SDL_SCANCODE_PAGEUP || sc == SDL_SCANCODE_PAGEDOWN) && track >= 0)) {
            if (sc == SDL_SCANCODE_PAGEUP) track = (track + 7) % 8;
            if (sc == SDL_SCANCODE_PAGEDOWN) track = (track + 1) % 8;
            view.reset();
            m.reset();
            m = start(disk, o, track);
            newView();
            setupAudio();
            wireNet();
          }
        }
        if (int st = stScancode(sc)) m->setKey(st, down);
      }
    }
    Joystick j;
    j.up = keys[SDL_SCANCODE_UP]; j.down = keys[SDL_SCANCODE_DOWN];
    j.left = keys[SDL_SCANCODE_LEFT]; j.right = keys[SDL_SCANCODE_RIGHT];
    // boost : plusieurs touches, car beaucoup de claviers ne lisent pas Espace + deux flèches à la fois
    // (la direction semblait alors bloquée pendant le boost) ; Maj et Alt n'ont pas ce problème
    j.fire = keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL] || keys[SDL_SCANCODE_LSHIFT] ||
             keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
    // en multijoueur, les menus lisent le joystick à travers le lien : un appui bref pourrait être manqué
    double tNow = double(SDL_GetPerformanceCounter()) / double(SDL_GetPerformanceFrequency());
    if (pad) {
      int ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
      j.left |= ax < -12000 || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
      j.right |= ax > 12000 || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
      j.up |= ay < -12000 || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) ||
              SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000;
      j.down |= ay > 12000 || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) ||
                SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000;
      j.fire |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X);
    }
    if (typingCode) j = Joystick();
    if (net.active()) {
      if (j.fire) fireUntil = tNow + 0.35;
      j.fire = j.fire || tNow < fireUntil;
    }
    m->setJoystick(j);
    net.poll();   // aussi entre les trames (connexion, annonces), pas seulement pendant l'émulation

    uint64_t now = SDL_GetPerformanceCounter();
    acc += double(now - last) / double(freq);
    last = now;
    if (acc > 0.25) acc = 0.25;
    int n = 0;
    while (acc >= framePeriod && n < 10) {
      m->runFrame();
      view->afterFrame(*m);
      queueAudio();
      acc -= framePeriod; n++;
    }

    // bandeau du multijoueur (visible aussi en plein écran) : saisie du code, état de la connexion
    std::string bannerMsg;
    {
      static std::string lastStatus;
      static double statusSince = 0;
      std::string st = net.status();
      if (st != lastStatus) { lastStatus = st; statusSince = tNow; }
      if (typingCode)
        bannerMsg = "Code de salle : " + code + "_   (Entrée : valider ; vide = nouveau code ; Échap : annuler)";
      else if (!st.empty() && (net.state() != NetLink::State::Connected || tNow - statusSince < 6))
        bannerMsg = st + (net.state() == NetLink::State::Failed ? "   (F10 : effacer)" : "");
      else if (o.relay.empty() && false) bannerMsg.clear();
    }
    auto drawBanner = [&](int logicalW) {
      static std::string cachedMsg; static int cachedScale = 0, bw = 0, bh = 0;
      static SDL_Texture *btex = nullptr;
      if (bannerMsg.empty()) return;
      int scale = std::max(1, logicalW / 480);
      if (bannerMsg != cachedMsg || scale != cachedScale || !btex) {
        if (btex) SDL_DestroyTexture(btex);
        auto img = renderBanner(bannerMsg, scale, bw, bh);
        btex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, bw, bh);
        SDL_SetTextureBlendMode(btex, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(btex, nullptr, img.data(), bw * 4);
        cachedMsg = bannerMsg; cachedScale = scale;
      }
      SDL_Rect r{std::max(0, (logicalW - bw) / 2), 0, std::min(bw, logicalW), bh};
      SDL_RenderCopy(ren, btex, nullptr, &r);
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
      view->render(*m, view->now() + acc / framePeriod, hdPixels.data(), o.hdW, o.hdH);
      SDL_UpdateTexture(texHd, nullptr, hdPixels.data(), o.hdW * 4);
      SDL_RenderClear(ren);
      SDL_RenderCopy(ren, texHd, nullptr, nullptr);
      drawBanner(o.hdW);
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
      m->screenARGB(pixels);
      SDL_UpdateTexture(tex, nullptr, pixels, 320 * 4);
      SDL_RenderClear(ren);
      SDL_RenderCopy(ren, tex, nullptr, nullptr);
      drawBanner(320);
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
