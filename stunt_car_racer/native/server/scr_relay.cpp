// scr_relay.cpp — serveur relais pour le mode « Computer Link » en ligne de Stunt Car Racer.
//
// Les jeux se connectent au serveur, qui met deux joueurs en relation puis transmet leurs trames de
// l'un à l'autre. Aucun port à ouvrir chez les joueurs : seul ce serveur écoute.
// Deux façons de se trouver :
//   - automatique : le serveur met le joueur en course à deux avec le premier autre joueur en
//     attente (chacun mène son jeu ; circuit tiré au sort, accepté par les deux ou « autre circuit ») ;
//   - salle privée : les joueurs qui tapent le même code voient la liste des présents et
//     choisissent leur adversaire (même course à deux), ou font une course à plusieurs (2 à 6).
//   Le Computer Link (une seule partie menée par le maître) reste pour le réseau local.
// La fenêtre du serveur affiche en direct les joueurs connectés (option --journal : simple journal).
// Trafic : environ 300 octets/s par joueur pendant une course.
//
// Compilation  Linux   : g++ -O2 -std=c++17 scr_relay.cpp -o scr_relay
//              Windows : g++ (MinGW) -O2 -std=c++17 -static scr_relay.cpp -o scr_relay.exe -lws2_32
//                        ou cl /O2 /EHsc /std:c++17 scr_relay.cpp ws2_32.lib
// Lancement           : scr_relay [--port 27420] [--log fichier.txt] [--journal] [--config scr_relay.ini]
//
// Réglages imposés aux jeux (fichier scr_relay.ini à côté du programme ou --config) : physique
// (gravity, thrust, brake, timestep, damping, boostuse, shock) et cadence réseau (reseau_cadence).
// Tous les joueurs connectés à ce serveur jouent avec les mêmes réglages.
//
// Protocole (trames : type 1 octet, longueur 2 octets gros-boutiste, données) :
//   client -> serveur : JOIN2 (12) "AUTO|SALLE \0 code \0 version \0 nom \0 tnt" (tnt : circuits TNT présents)
//                       PICK (14) "numéro"  (salle privée : choix de l'adversaire)
//                       CHAT (17) "texte"   (salle privée : à tous les joueurs libres de la salle,
//                                            reçu « nom\ttexte » ; en partie : retransmis à l'adversaire)
//                       GCMD (19) "ENTER" | "LEAVE" | "START circuit tours" | "LOADED" | "FINISH ms" | "OUT"
//                                            (course à plusieurs, 2 à 6 joueurs d'une salle privée)
//                                 | "ACCEPT" | "REJECT"   (course à deux : circuit proposé accepté / autre circuit)
//                       GPOS (21) position de la voiture (binaire), retransmise aux autres membres
//                                 précédée du numéro du joueur (4 octets)
//                       HORN (22) ""        (klaxon : à l'adversaire, ou aux autres membres de la course,
//                                            reçu précédé du numéro du joueur sur 4 octets)
//                       PROBE (15) ""       (test du serveur par le jeu : réponse INFO "SERVER joueurs parties;réglages;version=vNN")
//                       JOIN (10) "code\0version"  (ancienne version : appariement par code)
//   serveur -> client : INFO (11) "WAIT" | "PAIRED 1" (maître) | "PAIRED 2" | "GONE" | "BUSY" | "ERR message"
//                       LIST (13) "numéro\tnom\n..." (salle privée : joueurs présents et libres)
//                       GSTATE (18) état du groupe : "me N\nhost N\nphase lobby|vote|loading|racing|gone\n
//                                   track T laps L\nm N état nom\n..." (état : lobby, loading, loaded,
//                                   racing, finished:ms, out = abandon)
//                       GEVENT (20) "START circuit tours n1 n2 ..." (ordre sur la grille) | "GO" | "END"
//                       PARAMS (16) "gravity=1 thrust=1 ... cadence=400" (réglages imposés, après l'inscription)
//   ensuite, les trames de chaque joueur sont retransmises telles quelles à l'autre.
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS   // inet_ntoa
#endif
#ifndef NOMINMAX
#define NOMINMAX   // std::min / std::max
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET Sock;
typedef int socklen_t;
typedef WSAPOLLFD PollFd;
static int sockErr() { return WSAGetLastError(); }
static bool wouldBlock(int e) { return e == WSAEWOULDBLOCK; }
static void closeSock(Sock s) { closesocket(s); }
static void nonBlock(Sock s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
static int pollSocks(PollFd *p, size_t n, int ms) { return WSAPoll(p, ULONG(n), ms); }
static const char *errText(int e) { static char b[64]; std::snprintf(b, sizeof b, "erreur Winsock %d", e); return b; }
static bool isConsole() {   // console Windows 10+ : séquences d'échappement pour l'affichage en direct
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE); DWORD m = 0;
  if (!GetConsoleMode(h, &m)) return false;
  return SetConsoleMode(h, m | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */) != 0;
}
#define SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Sock;
static bool isConsole() { return isatty(1); }
typedef pollfd PollFd;
static int sockErr() { return errno; }
static bool wouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
static void closeSock(Sock s) { ::close(s); }
static void nonBlock(Sock s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
static int pollSocks(PollFd *p, size_t n, int ms) { return poll(p, nfds_t(n), ms); }
static const char *errText(int e) { return std::strerror(e); }
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif
#endif

// version du serveur : toujours la même que celle du jeu (SCR_VERSION de src/main.cpp)
#define SCR_RELAY_VERSION "v65"

namespace {

constexpr uint8_t NF_JOIN = 10, NF_INFO = 11, NF_JOIN2 = 12, NF_LIST = 13, NF_PICK = 14, NF_PROBE = 15, NF_PARAMS = 16, NF_CHAT = 17,
                  NF_GSTATE = 18, NF_GCMD = 19, NF_GEVENT = 20, NF_GPOS = 21, NF_HORN = 22;
constexpr size_t GROUP_MAX = 6;
constexpr size_t MAX_CLIENTS = 512, MAX_PER_IP = 16, MAX_BUFFER = 1 << 20;
const Sock NONE = Sock(-1);
constexpr double JOIN_TIMEOUT = 20, PAIR_TIMEOUT = 3600, IDLE_TIMEOUT = 90;

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ------------------------------------------------------------------ journal et console
FILE *logFile = nullptr;      // --log : copie du journal dans un fichier
bool live = false;            // affichage en direct des joueurs (console)
bool dirty = true;            // à redessiner
std::vector<std::string> events;   // derniers événements (affichage en direct)

std::string stamp(const char *fmt) {
  char ts[32]; time_t t = time(nullptr); strftime(ts, sizeof ts, fmt, localtime(&t));
  return ts;
}

void logf(const char *fmt, ...) {
  char msg[512];
  va_list ap; va_start(ap, fmt); std::vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
  std::string ts = stamp("%Y-%m-%d %H:%M:%S");
  if (logFile) { std::fprintf(logFile, "%s  %s\n", ts.c_str(), msg); std::fflush(logFile); }
  if (live) {
    events.push_back(stamp("%H:%M:%S") + "  " + msg);
    if (events.size() > 12) events.erase(events.begin());
    dirty = true;
  } else { std::printf("%s  %s\n", ts.c_str(), msg); std::fflush(stdout); }
}

// largeur affichée d'un texte UTF-8, et alignement en colonnes
size_t width(const std::string &s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xc0) != 0x80;
  return n;
}
std::string col(std::string s, size_t w) {
  while (width(s) > w) { s.pop_back(); while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xc0) == 0x80) s.pop_back(); }
  return s + std::string(w - width(s), ' ');
}
std::string duration(double sec) {
  int t = int(sec); char b[32];
  if (t >= 3600) std::snprintf(b, sizeof b, "%dh%02d", t / 3600, t / 60 % 60);
  else std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
  return b;
}

// ------------------------------------------------------------------ joueurs
enum class Mode { None, Auto, Room, Code };   // Code : ancienne version (appariement par code)

struct Client {
  Sock fd = NONE;
  uint32_t id = 0;               // numéro montré aux autres joueurs et dans la console
  std::string ip, name, room, version;
  Mode mode = Mode::None;
  std::vector<uint8_t> in, out;
  Sock peer = NONE;              // partenaire une fois apparié
  bool joined = false, closing = false, probe = false;
  bool overflow = false;         // ne lit plus ce qu'on lui envoie : déconnecté
  double closingSince = 0;
  double since = 0, lastRx = 0, pairedAt = 0, lastChat = 0;
  bool inGroup = false;          // course à plusieurs
  bool tnt = false;              // le jeu a les circuits TNT (tirage des courses à deux parmi 16)
  double lastHorn = 0;
  std::string gkey;              // groupe : code de la salle, ou « DUEL n » (course à deux par appariement)
  std::string gstate = "lobby";  // lobby, loading, loaded, racing, finished:ms, out (voiture détruite)
  uint64_t rxBytes = 0, rxMark = 0;
  double rate = 0;               // octets reçus par seconde
  std::string lastList;          // dernière liste envoyée (salle privée)
};

std::map<Sock, Client> clients;
uint32_t nextId = 1;
uint64_t totalConnections = 0, totalGames = 0;
double startedAt = 0;
int listenPort = 27420;
std::string machineAddresses;

// réglages imposés aux jeux
struct Params {
  double gravity = 1, thrust = 1, brake = 1, timestep = 1, damping = 1, boostuse = 1;
  int shock = 0, cadence = 400;
} params;

std::string paramsText() {
  char b[256];
  std::snprintf(b, sizeof b, "gravity=%.6g thrust=%.6g brake=%.6g timestep=%.6g damping=%.6g boostuse=%.6g shock=%d cadence=%d",
                params.gravity, params.thrust, params.brake, params.timestep, params.damping, params.boostuse,
                params.shock, params.cadence);
  return b;
}

// lecture de scr_relay.ini : « clé = valeur », # commentaire
bool readConfig(const std::string &path, int &port) {
  FILE *f = std::fopen(path.c_str(), "r");
  if (!f) return false;
  char line[512];
  while (std::fgets(line, sizeof line, f)) {
    std::string l = line;
    size_t h = l.find('#'); if (h != std::string::npos) l.resize(h);
    size_t eq = l.find('=');
    if (eq == std::string::npos) continue;
    auto trim = [](std::string x) {
      while (!x.empty() && std::isspace(static_cast<unsigned char>(x.back()))) x.pop_back();
      while (!x.empty() && std::isspace(static_cast<unsigned char>(x.front()))) x.erase(x.begin());
      return x;
    };
    std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
    if (v.empty()) continue;
    double d = std::atof(v.c_str());
    if (k == "port") port = std::atoi(v.c_str());
    else if (k == "gravity") params.gravity = d;
    else if (k == "thrust") params.thrust = d;
    else if (k == "brake") params.brake = d;
    else if (k == "timestep") params.timestep = d;
    else if (k == "damping") params.damping = d;
    else if (k == "boostuse") params.boostuse = d;
    else if (k == "shock") params.shock = std::atoi(v.c_str());
    else if (k == "reseau_cadence" || k == "cadence") params.cadence = std::max(50, std::min(2000, std::atoi(v.c_str())));
  }
  std::fclose(f);
  return true;
}

// dossier du programme (pour trouver scr_relay.ini à côté de lui)
std::string exeDir() {
#ifdef _WIN32
  char b[MAX_PATH]; DWORD n = GetModuleFileNameA(nullptr, b, MAX_PATH);
  std::string p(b, n);
  size_t k = p.find_last_of("\\/");
#else
  char b[4096]; ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
  std::string p = n > 0 ? std::string(b, size_t(n)) : "";
  size_t k = p.find_last_of('/');
#endif
  return k == std::string::npos ? "" : p.substr(0, k + 1);
}

void frame(Client &c, uint8_t type, const std::string &s) {
  if (c.out.size() > MAX_BUFFER) { c.overflow = true; return; }   // client qui ne lit plus : mémoire bornée
  size_t n = std::min<size_t>(s.size(), 0xffff);
  c.out.push_back(type); c.out.push_back(uint8_t(n >> 8)); c.out.push_back(uint8_t(n));
  c.out.insert(c.out.end(), s.begin(), s.begin() + long(n));
}

std::string who(const Client &c) { return c.name.empty() ? c.ip : c.name + " (" + c.ip + ")"; }

bool free_(const Client &c) { return c.joined && !c.closing && c.peer == NONE && !c.inGroup; }

// ------------------------------------------------------------------ course à plusieurs (groupe d'une salle)
struct Group {
  std::vector<Sock> members;     // ordre d'arrivée ; le premier est l'organisateur
  std::string phase = "lobby";   // lobby, loading, racing
  int track = 0, laps = 3;
  bool duel = false;             // course à deux (premier joueur disponible, ou « Affronter » dans une salle) :
                                 // chacun mène son jeu, circuit proposé et accepté par les deux (phase vote)
  double deadline = 0, firstFinish = 0, startedAt = 0;
  std::string lastState;
};
std::map<std::string, Group> groups;   // salle -> groupe

void groupSend(Group &g, uint8_t type, const std::string &msg) {
  for (Sock f : g.members) { auto it = clients.find(f); if (it != clients.end()) frame(it->second, type, msg); }
}

void groupState(const std::string &room, bool force = false) {
  auto git = groups.find(room);
  if (git == groups.end()) return;
  Group &g = git->second;
  std::string body = "host " + std::to_string(g.members.empty() ? 0 : clients[g.members[0]].id) + "\nphase " + g.phase +
                     "\ntrack " + std::to_string(g.track) + " laps " + std::to_string(g.laps) + "\n" + (g.duel ? "duel 1\n" : "");
  for (Sock f : g.members) {
    Client &m = clients[f];
    body += "m " + std::to_string(m.id) + " " + m.gstate + " " + (m.name.empty() ? "Joueur" + std::to_string(m.id) : m.name) + "\n";
  }
  if (body == g.lastState && !force) return;
  g.lastState = body;
  for (Sock f : g.members) frame(clients[f], NF_GSTATE, "me " + std::to_string(clients[f].id) + "\n" + body);
  dirty = true;
}

void matchAuto(Client &c);
void duelDissolve(const std::string &key);

// circuit tiré au sort pour une course à deux : 8 d'origine, ou 16 si tous ont les circuits TNT
int duelDraw(const Group &g, int avoid) {
  bool tnt = !g.members.empty();
  for (Sock f : g.members) { auto it = clients.find(f); tnt = tnt && it != clients.end() && it->second.tnt; }
  const int n = tnt ? 16 : 8;
  int t = avoid;
  while (t == avoid) t = std::rand() % n;
  return t;
}

void groupLeave(Client &c) {
  if (!c.inGroup) return;
  c.inGroup = false;
  const std::string key = c.gkey;
  auto git = groups.find(key);
  if (git == groups.end()) return;
  Group &g = git->second;
  g.members.erase(std::remove(g.members.begin(), g.members.end(), c.fd), g.members.end());
  logf("%s quitte la course %s", who(c).c_str(), key.c_str());
  if (g.members.empty()) { groups.erase(git); return; }
  // course à deux : seul, le joueur restant retrouve la salle ou attend un autre adversaire (hors course)
  if (g.duel && g.phase != "racing" && g.phase != "loading") { duelDissolve(key); return; }
  groupState(key);
}

// fin d'une course à deux : les joueurs restants redeviennent libres (salle) ou attendent (automatique)
void duelDissolve(const std::string &key) {
  auto git = groups.find(key);
  if (git == groups.end()) return;
  std::vector<Sock> rest = git->second.members;
  groups.erase(git);
  for (Sock f : rest) {
    auto it = clients.find(f);
    if (it == clients.end()) continue;
    Client &m = it->second;
    m.inGroup = false; m.gkey.clear(); m.gstate = "lobby";
    frame(m, NF_GSTATE, "me " + std::to_string(m.id) + "\nhost 0\nphase gone\n");   // plus de groupe
    frame(m, NF_INFO, "WAIT");
    if (m.mode == Mode::Auto) matchAuto(m);
  }
}

// course à deux : groupe des deux joueurs, circuit tiré au sort et proposé aux deux
void duelCreate(Client &a, Client &b) {
  const std::string key = "DUEL" + std::to_string(a.id);
  Group &g = groups[key];
  g = Group();
  g.duel = true; g.phase = "vote"; g.laps = 3;
  for (Client *m : {&a, &b}) { m->inGroup = true; m->gkey = key; m->gstate = "vote"; g.members.push_back(m->fd); }
  g.track = duelDraw(g, -1);
  totalGames++;
  logf("course à deux : %s contre %s (circuit %d proposé)", who(a).c_str(), who(b).c_str(), g.track + 1);
  groupState(key, true);
}

void groupEnter(Client &c) {
  if (c.mode != Mode::Room || c.inGroup || c.peer != NONE) return;
  Group &g = groups[c.room];
  c.gkey = c.room;
  if (g.phase != "lobby") { frame(c, NF_INFO, "ERR une course est en cours dans cette salle, réessayez à la fin"); return; }
  if (g.members.size() >= GROUP_MAX) { frame(c, NF_INFO, "ERR course complète (6 joueurs)"); return; }
  c.inGroup = true; c.gstate = "lobby";
  g.members.push_back(c.fd);
  logf("%s rejoint la course à plusieurs de la salle %s (%zu joueur(s))", who(c).c_str(), c.room.c_str(), g.members.size());
  groupState(c.gkey, true);
}

// départ de la course d'un groupe : grille tirée au sort, chargement
void groupStart(const std::string &key, Group &g) {
  g.phase = "loading"; g.deadline = now() + 30; g.firstFinish = 0;
  for (Sock f : g.members) clients[f].gstate = "loading";
  // grille de départ (3 rangées de 2) : ordre tiré au sort à chaque course
  std::vector<uint32_t> grid;
  for (Sock f : g.members) grid.push_back(clients[f].id);
  for (size_t i = grid.size(); i > 1; i--) std::swap(grid[i - 1], grid[size_t(std::rand()) % i]);
  std::string startMsg = "START " + std::to_string(g.track) + " " + std::to_string(g.laps);
  for (uint32_t id : grid) startMsg += " " + std::to_string(id);
  groupSend(g, NF_GEVENT, startMsg);
  logf("%s : course à %zu joueurs lancée (circuit %d, %d tour(s))", key.c_str(), g.members.size(), g.track + 1, g.laps);
}

void groupCmd(Client &c, const std::string &cmd) {
  if (cmd == "ENTER") { groupEnter(c); return; }
  if (!c.inGroup) return;
  if (cmd == "LEAVE") {
    if (c.mode == Mode::Auto) c.mode = Mode::None;   // quitte la course à deux : le jeu se déconnecte, pas de nouvel adversaire
    groupLeave(c);
    return;
  }
  const std::string key = c.gkey;
  Group &g = groups[key];
  if (cmd.rfind("START", 0) == 0) {
    if (g.duel || g.members.empty() || g.members[0] != c.fd || g.phase != "lobby" || g.members.size() < 2) return;
    int t = 0, l = 3;
    std::sscanf(cmd.c_str() + 5, "%d %d", &t, &l);
    g.track = std::max(0, std::min(15, t)); g.laps = std::max(1, std::min(9, l));   // 8-15 : circuits TNT
    groupStart(key, g);
  } else if (cmd == "ACCEPT") {   // course à deux : circuit proposé accepté ; départ quand les deux acceptent
    if (!g.duel || g.phase != "vote") return;
    c.gstate = "accept";
    bool all = g.members.size() >= 2;
    for (Sock f : g.members) all = all && clients[f].gstate == "accept";
    if (all) groupStart(key, g);
  } else if (cmd == "REJECT") {   // « Autre circuit » : nouveau tirage, les deux doivent de nouveau accepter
    if (!g.duel || g.phase != "vote") return;
    g.track = duelDraw(g, g.track);
    for (Sock f : g.members) clients[f].gstate = "vote";
    logf("%s : %s demande un autre circuit (circuit %d proposé)", key.c_str(), c.name.c_str(), g.track + 1);
  } else if (cmd == "LOADED") {
    if (g.phase == "loading") c.gstate = "loaded";
  } else if (cmd.rfind("FINISH", 0) == 0) {
    if (g.phase == "racing" && c.gstate == "racing") {
      long ms = std::max(0L, std::min(86400000L, std::atol(cmd.c_str() + 6)));
      c.gstate = "finished:" + std::to_string(ms);
      if (!g.firstFinish) g.firstFinish = now();
      logf("%s : %s arrive en %ld,%03ld s", key.c_str(), c.name.c_str(), ms / 1000, ms % 1000);
    }
  } else if (cmd == "OUT") {   // voiture détruite : abandon
    if (g.phase == "racing" && c.gstate == "racing") {
      c.gstate = "out";
      logf("%s : %s abandonne (voiture détruite)", key.c_str(), c.name.c_str());
    }
  }
  if (groups.count(key)) groupState(key);
}

// avancement des groupes : départ quand tous sont prêts, fin quand tous sont arrivés
void groupTick() {
  double t = now();
  std::vector<std::string> duelDoomed;   // courses à deux réduites à un joueur : dissoutes après la boucle
  for (auto &kv : groups) {
    Group &g = kv.second;
    if (g.phase == "loading") {
      bool all = true;
      for (Sock f : g.members) all = all && clients[f].gstate == "loaded";
      if (all || t > g.deadline) {
        g.phase = "racing"; g.startedAt = t;
        for (Sock f : g.members) clients[f].gstate = "racing";
        groupSend(g, NF_GEVENT, "GO");
        groupState(kv.first);
      }
    } else if (g.phase == "racing") {
      bool all = true;
      for (Sock f : g.members) all = all && (clients[f].gstate.rfind("finished", 0) == 0 || clients[f].gstate == "out");
      // fin : tous arrivés ou abandonnés, ou 2 minutes après le premier, ou 30 minutes de course
      if (all || (g.firstFinish && t - g.firstFinish > 120) || t - g.startedAt > 1800) {
        g.phase = g.duel ? "vote" : "lobby";
        if (g.duel) g.track = duelDraw(g, g.track);   // revanche : autre circuit proposé
        groupSend(g, NF_GEVENT, "END");
        groupState(kv.first, true);
        for (Sock f : g.members) if (clients[f].gstate.rfind("finished", 0) != 0 && clients[f].gstate != "out") clients[f].gstate = g.duel ? "vote" : "lobby";
        logf("%s : course terminée", kv.first.c_str());
        if (g.duel && g.members.size() < 2) { duelDoomed.push_back(kv.first); }
      }
    }
  }
  for (auto &key : duelDoomed) duelDissolve(key);
}

void pair(Client &master, Client &other) {
  master.peer = other.fd; other.peer = master.fd;
  master.pairedAt = other.pairedAt = now();
  frame(master, NF_INFO, "PAIRED 1"); frame(other, NF_INFO, "PAIRED 2");   // 1 : maître dans le jeu
  totalGames++;
  logf("partie : %s contre %s", who(master).c_str(), who(other).c_str());
}

// salle privée : liste des joueurs libres de la salle (même version), envoyée à chacun si elle change
void sendLists() {
  for (auto &kv : clients) {
    Client &c = kv.second;
    if (c.mode != Mode::Room || !free_(c)) continue;
    std::string l;
    for (auto &kv2 : clients) {
      const Client &o = kv2.second;
      if (&o == &c || o.mode != Mode::Room || !free_(o) || o.room != c.room || o.version != c.version) continue;
      l += std::to_string(o.id) + "\t" + (o.name.empty() ? "Joueur " + std::to_string(o.id) : o.name) + "\n";
    }
    if (l != c.lastList) { c.lastList = l; frame(c, NF_LIST, l); }
  }
}

void drop(Sock fd, const char *why) {
  auto it = clients.find(fd);
  if (it == clients.end()) return;
  Client &c = it->second;
  if (c.peer != NONE) {
    auto p = clients.find(c.peer);
    if (p != clients.end()) { frame(p->second, NF_INFO, "GONE"); p->second.peer = NONE; p->second.closing = true; }
  }
  if (!c.probe) logf("%s déconnecté (%s)", who(c).c_str(), why);
  c.closing = true;   // plus disponible : jamais réapparié pendant son départ
  groupLeave(c);
  closeSock(fd);
  clients.erase(it);
  dirty = true;
}

bool validRoom(const std::string &r) {
  if (r.empty() || r.size() > 24) return false;
  for (char ch : r) if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_')) return false;
  return true;
}

// texte venu du réseau : UTF-8 valide seulement, sans caractères de contrôle (C0, DEL, C1 :
// pas de séquences d'échappement dans la console), au plus maxw caractères
std::string cleanName(const std::string &s, size_t maxw = 24) {
  std::string r;
  size_t count = 0;
  for (size_t i = 0; i < s.size() && count < maxw;) {
    unsigned char c0 = static_cast<unsigned char>(s[i]);
    size_t len = c0 < 0x80 ? 1 : (c0 & 0xe0) == 0xc0 ? 2 : (c0 & 0xf0) == 0xe0 ? 3 : (c0 & 0xf8) == 0xf0 ? 4 : 0;
    if (len == 0 || i + len > s.size()) { i++; continue; }
    uint32_t cp = len == 1 ? c0 : c0 & (0xff >> (len + 1));
    bool ok = true;
    for (size_t k = 1; k < len; k++) {
      unsigned char ck = static_cast<unsigned char>(s[i + k]);
      if ((ck & 0xc0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | (ck & 0x3f);
    }
    static const uint32_t minCp[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (!ok) { i++; continue; }
    if (cp < minCp[len] || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp < 32 || (cp >= 0x7f && cp < 0xa0)) { i += len; continue; }
    r.append(s, i, len); count++; i += len;
  }
  return r;
}

std::vector<std::string> fields(const uint8_t *p, size_t n) {
  std::vector<std::string> f(1);
  for (size_t i = 0; i < n; i++) { if (p[i] == 0) f.emplace_back(); else f.back() += char(p[i]); }
  return f;
}

void onJoin(Client &c, Mode mode, const std::string &room, const std::string &version, const std::string &name) {
  c.mode = mode; c.version = version.substr(0, 32); c.name = cleanName(name);
  c.room = room;
  for (auto &ch : c.room) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  if (mode != Mode::Auto && !validRoom(c.room)) { frame(c, NF_INFO, "ERR code de salle invalide"); c.closing = true; return; }
  c.joined = true; dirty = true;
  frame(c, NF_PARAMS, paramsText());   // réglages imposés, appliqués par le jeu pendant la partie
  frame(c, NF_INFO, "WAIT");
  if (mode == Mode::Room) { logf("%s entre dans la salle %s", who(c).c_str(), c.room.c_str()); return; }
  if (mode == Mode::Auto) { matchAuto(c); return; }
  // ancien appariement par code : le premier joueur libre compatible, arrivé avant (Computer Link)
  Client *best = nullptr;
  for (auto &kv : clients) {
    Client &o = kv.second;
    if (&o == &c || !free_(o) || o.mode != mode || o.version != c.version || o.room != c.room) continue;
    if (!best || o.since < best->since) best = &o;
  }
  if (best) pair(*best, c);
  else logf("%s attend un adversaire (code %s)", who(c).c_str(), c.room.c_str());
}

// premier joueur disponible : course à deux avec le premier joueur libre (même version) arrivé avant ;
// chacun mène son propre jeu (pas de Computer Link)
void matchAuto(Client &c) {
  Client *best = nullptr;
  for (auto &kv : clients) {
    Client &o = kv.second;
    if (&o == &c || !free_(o) || o.mode != Mode::Auto || o.version != c.version) continue;
    if (!best || o.since < best->since) best = &o;
  }
  if (best) duelCreate(*best, c);
  else logf("%s attend un adversaire (automatique)", who(c).c_str());
}

void onPick(Client &c, const std::string &s) {
  uint32_t id = uint32_t(std::strtoul(s.c_str(), nullptr, 10));
  if (c.mode != Mode::Room || !free_(c)) return;
  for (auto &kv : clients) {
    Client &o = kv.second;
    if (o.id == id && &o != &c && o.mode == Mode::Room && free_(o) && o.room == c.room && o.version == c.version) {
      duelCreate(c, o);   // course à deux : chacun mène son jeu
      return;
    }
  }
  frame(c, NF_INFO, "BUSY");   // déjà parti ou en partie : la liste se met à jour
}

// trames complètes reçues : inscription avant l'appariement, retransmission ensuite
void process(Client &c) {
  size_t pos = 0;
  while (c.in.size() - pos >= 3) {
    size_t n = (size_t(c.in[pos + 1]) << 8) | c.in[pos + 2];
    if (c.in.size() - pos < 3 + n) break;
    uint8_t type = c.in[pos];
    const uint8_t *p = c.in.data() + pos + 3;
    if (!c.joined) {
      auto f = fields(p, n);
      if (type == NF_JOIN2 && f.size() >= 3) {
        c.tnt = f.size() > 4 && f[4] == "tnt";   // circuits TNT présents chez ce joueur
        onJoin(c, f[0] == "SALLE" ? Mode::Room : Mode::Auto, f[1], f[2], f.size() > 3 ? f[3] : "");
      }
      else if (type == NF_JOIN) onJoin(c, Mode::Code, f[0], f.size() > 1 ? f[1] : "", "");
      else if (type == NF_PROBE) {   // le jeu vérifie que le serveur répond
        size_t players = 0, games = 0;
        for (auto &kv : clients) { players += kv.second.joined; games += kv.second.peer != NONE; }
        // version du serveur (= version du jeu) : le jeu prévient le joueur quand une nouvelle version est sortie
        frame(c, NF_INFO, "SERVER " + std::to_string(players) + " " + std::to_string(games / 2) + ";" + paramsText() + ";version=" SCR_RELAY_VERSION);
        c.probe = c.closing = true;
      }
      else { frame(c, NF_INFO, "ERR inscription attendue"); c.closing = true; }
    } else if (c.peer != NONE) {
      Client &o = clients[c.peer];
      if (o.out.size() < MAX_BUFFER) o.out.insert(o.out.end(), c.in.begin() + long(pos), c.in.begin() + long(pos + 3 + n));
      else o.overflow = true;
    } else if (type == NF_PICK) onPick(c, std::string(reinterpret_cast<const char *>(p), n));
    else if (type == NF_GCMD) groupCmd(c, std::string(reinterpret_cast<const char *>(p), std::min<size_t>(n, 64)));
    else if (type == NF_GPOS && c.inGroup && n <= 64) {   // position : aux autres membres, avec le numéro
      std::string m(4, '\0');
      for (int k = 0; k < 4; k++) m[size_t(k)] = char((c.id >> (24 - 8 * k)) & 255);
      m.append(reinterpret_cast<const char *>(p), n);
      for (Sock f : groups[c.gkey].members) if (f != c.fd) frame(clients[f], NF_GPOS, m);
    }
    else if (type == NF_HORN && c.inGroup && now() - c.lastHorn > 0.25) {   // klaxon : aux autres membres
      c.lastHorn = now();
      std::string m(4, '\0');
      for (int k = 0; k < 4; k++) m[size_t(k)] = char((c.id >> (24 - 8 * k)) & 255);
      for (Sock f : groups[c.gkey].members) if (f != c.fd) frame(clients[f], NF_HORN, m);
    }
    else if (type == NF_CHAT && c.mode == Mode::Room && now() - c.lastChat > 0.3) {   // discussion de la salle
      c.lastChat = now();
      std::string text = cleanName(std::string(reinterpret_cast<const char *>(p), std::min<size_t>(n, 360)), 120);
      if (text.empty()) { pos += 3 + n; continue; }
      std::string msg = (c.name.empty() ? "Joueur " + std::to_string(c.id) : c.name) + "\t" + text;
      for (auto &kv : clients) {
        Client &o = kv.second;
        if (&o != &c && o.mode == Mode::Room && o.joined && o.peer == NONE && o.room == c.room) frame(o, NF_CHAT, msg);
      }
      logf("salle %s, %s : %s", c.room.c_str(), c.name.c_str(), text.c_str());
    }
    pos += 3 + n;
    if (c.closing) break;
  }
  c.in.erase(c.in.begin(), c.in.begin() + long(pos));
}

// ------------------------------------------------------------------ affichage en direct
std::string stateOf(const Client &c, double t) {
  if (c.probe) return "test du serveur par un jeu";
  if (c.closing) return "fermeture";
  if (!c.joined) return "connexion...";
  if (c.peer != NONE) {
    auto p = clients.find(c.peer);
    std::string o = p == clients.end() ? "?" : (p->second.name.empty() ? p->second.ip : p->second.name);
    return "en partie contre " + o + " (" + duration(t - c.pairedAt) + ")";
  }
  if (c.inGroup) {
    const std::string &g = c.gstate;
    if (g.rfind("finished:", 0) == 0) { long ms = std::atol(g.c_str() + 9); char b[64]; std::snprintf(b, sizeof b, "course à plusieurs : arrivé %ld,%ld s", ms / 1000, ms % 1000 / 100); return b; }
    if (g == "out") return "course à plusieurs : abandon (voiture détruite)";
    if (g == "vote" || g == "accept") return std::string("course à deux : ") + (g == "accept" ? "circuit accepté" : "choix du circuit");
    return "course à plusieurs : " + std::string(g == "lobby" ? "attente du départ" : g == "racing" ? "en course" : "chargement");
  }
  return c.mode == Mode::Room ? "dans la salle, libre" : "attend un adversaire";
}

void draw(double t) {
  std::string o = "\x1b[H\x1b[2J";
  size_t games = 0;
  for (auto &kv : clients) games += kv.second.peer != NONE;
  char b[512];
  std::snprintf(b, sizeof b, "\x1b[1mSERVEUR RELAIS STUNT CAR RACER " SCR_RELAY_VERSION "\x1b[0m   port TCP %d   en service depuis %s   %s\n", listenPort,
                duration(t - startedAt).c_str(), stamp("%H:%M:%S").c_str());
  o += b;
  if (!machineAddresses.empty()) o += "Adresse(s) de cette machine : " + machineAddresses + "\n";
  o += "Réglages imposés aux jeux : " + paramsText() + "\n";
  std::snprintf(b, sizeof b, "Joueurs connectés : %zu    parties en cours : %zu    depuis le lancement : %llu connexions, %llu parties\n\n",
                clients.size(), games / 2, static_cast<unsigned long long>(totalConnections), static_cast<unsigned long long>(totalGames));
  o += b;
  o += "\x1b[7m" + col(" N°", 5) + col("Joueur", 18) + col("Adresse IP", 16) + col("Mode", 13) + col("Salle", 10) +
       col("État", 36) + col("Connecté", 10) + col("Trafic", 9) + "\x1b[0m\n";
  std::vector<const Client *> list;
  for (auto &kv : clients) list.push_back(&kv.second);
  std::sort(list.begin(), list.end(), [](const Client *a, const Client *b) { return a->id < b->id; });
  size_t shown = 0;
  for (const Client *c : list) {
    if (++shown > 30) { o += "  ... et " + std::to_string(list.size() - 30) + " autres\n"; break; }
    const char *mode = c->mode == Mode::Auto ? "automatique" : c->mode == Mode::Room ? "salle privée" : c->mode == Mode::Code ? "code" : "-";
    std::snprintf(b, sizeof b, "%.0f o/s", c->rate);
    o += " " + col(std::to_string(c->id), 4) + col(c->name.empty() ? "-" : c->name, 18) + col(c->ip, 16) + col(mode, 13) +
         col(c->mode == Mode::Auto || c->room.empty() ? "-" : c->room, 10) + col(stateOf(*c, t), 36) +
         col(duration(t - c->since), 10) + b + "\n";
  }
  if (list.empty()) o += "  (aucun joueur connecté)\n";
  o += "\nDerniers événements :\n";
  for (auto &e : events) o += "  " + e + "\n";
  o += "\nCtrl+C ou fermer la fenêtre : arrêter le serveur\n";
  std::fwrite(o.data(), 1, o.size(), stdout);
  std::fflush(stdout);
}

// adresses IPv4 de cette machine (pour le message de démarrage)
std::string localAddresses() {
  std::string r;
  auto add = [&](const in_addr &ia) {
    std::string ip = inet_ntoa(ia);
    if (ip.rfind("127.", 0) != 0 && r.find(ip) == std::string::npos) r += (r.empty() ? "" : ", ") + ip;
  };
#ifdef _WIN32
  char host[256];
  if (gethostname(host, sizeof host) != 0) return r;
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, nullptr, &hints, &res) != 0) return r;
  for (addrinfo *a = res; a; a = a->ai_next) add(reinterpret_cast<sockaddr_in *>(a->ai_addr)->sin_addr);
  freeaddrinfo(res);
#else
  ifaddrs *list = nullptr;
  if (getifaddrs(&list) != 0) return r;
  for (ifaddrs *f = list; f; f = f->ifa_next)
    if (f->ifa_addr && f->ifa_addr->sa_family == AF_INET) add(reinterpret_cast<sockaddr_in *>(f->ifa_addr)->sin_addr);
  freeifaddrs(list);
#endif
  return r;
}

}  // namespace

int main(int argc, char **argv) {
  int port = 27420;
  bool journal = false;
  std::string config;
  for (int i = 1; i + 1 < argc; i++) if (!std::strcmp(argv[i], "--config")) config = argv[i + 1];
  bool haveConfig = false;
  if (!config.empty()) {
    if (!(haveConfig = readConfig(config, port))) { std::fprintf(stderr, "fichier de réglages introuvable : %s\n", config.c_str()); return 1; }
  } else if ((haveConfig = readConfig(exeDir() + "scr_relay.ini", port))) config = exeDir() + "scr_relay.ini";
  else if ((haveConfig = readConfig("scr_relay.ini", port))) config = "scr_relay.ini";
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--config") && i + 1 < argc) i++;
    else if (!std::strcmp(argv[i], "--journal")) journal = true;
    else if (!std::strcmp(argv[i], "--log") && i + 1 < argc) {
      logFile = std::fopen(argv[++i], "a");
      if (!logFile) { std::fprintf(stderr, "impossible d'ouvrir le journal %s\n", argv[i]); return 1; }
    } else { std::fprintf(stderr, "usage : %s [--port N] [--log fichier] [--journal] [--config scr_relay.ini]\n", argv[0]); return 1; }
  }
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::fprintf(stderr, "Winsock indisponible\n"); return 1; }
  SetConsoleOutputCP(CP_UTF8);   // accents
  SetConsoleTitleA("Serveur relais Stunt Car Racer " SCR_RELAY_VERSION);
#else
  std::signal(SIGPIPE, SIG_IGN);
#endif
  Sock ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int on = 1;
#ifndef _WIN32
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on), sizeof on);
#endif
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(uint16_t(port));
  if (bind(ls, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || listen(ls, 64) != 0) {
    int e = sockErr();
    logf("impossible d'écouter sur le port %d : %s (un autre serveur tourne déjà ?)", port, errText(e));
#ifdef _WIN32
    std::printf("Appuyez sur Entrée pour fermer.\n"); std::getchar();
#endif
    return 1;
  }
  nonBlock(ls);
  listenPort = port;
  startedAt = now();
  machineAddresses = localAddresses();
  live = !journal && isConsole();
  std::srand(unsigned(std::time(nullptr)));
  logf("relais Stunt Car Racer " SCR_RELAY_VERSION " : écoute sur le port TCP %d", port);
  if (!machineAddresses.empty()) logf("adresse(s) de cette machine : %s", machineAddresses.c_str());
  logf("réglages imposés aux jeux%s : %s", haveConfig ? (" (" + config + ")").c_str() : " (par défaut, pas de scr_relay.ini)",
       paramsText().c_str());
  logf("scr.ini des joueurs : relais = ADRESSE:%d (en réseau local : adresse ci-dessus ; par Internet : adresse", port);
  logf("publique de la box ou nom de domaine, avec le port TCP %d redirigé vers cette machine)", port);
  double lastDraw = 0, lastRate = now(), lastSummary = now();
  for (;;) {
    std::vector<PollFd> pf;
    PollFd l0{}; l0.fd = ls; l0.events = POLLIN;
    pf.push_back(l0);
    for (auto &kv : clients) {
      PollFd p{}; p.fd = kv.first; p.events = short(POLLIN | (kv.second.out.empty() ? 0 : POLLOUT));
      pf.push_back(p);
    }
    pollSocks(pf.data(), pf.size(), 250);
    double t = now();
    if (pf[0].revents & POLLIN) {
      for (;;) {
        sockaddr_in ca{}; socklen_t l = sizeof ca;
        Sock fd = accept(ls, reinterpret_cast<sockaddr *>(&ca), &l);
        if (fd == NONE) break;
        std::string ip = inet_ntoa(ca.sin_addr);
        size_t same = 0;
        for (auto &kv : clients) same += kv.second.ip == ip;
        if (clients.size() >= MAX_CLIENTS || same >= MAX_PER_IP) { closeSock(fd); logf("%s refusé (trop de connexions)", ip.c_str()); continue; }
        nonBlock(fd);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof on);
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char *>(&on), sizeof on);
        Client c; c.fd = fd; c.id = nextId++; c.ip = ip; c.since = c.lastRx = t;
        clients[fd] = c;
        totalConnections++;
        dirty = true;
      }
    }
    std::vector<std::pair<Sock, const char *>> dead;
    for (size_t i = 1; i < pf.size(); i++) {
      Sock fd = pf[i].fd;
      auto it = clients.find(fd);
      if (it == clients.end()) continue;
      Client &c = it->second;
      if (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) {
        char buf[8192];
        for (;;) {
          int r = int(recv(fd, buf, sizeof buf, 0));
          if (r > 0) {
            c.in.insert(c.in.end(), buf, buf + r); c.lastRx = t; c.rxBytes += uint64_t(r);
            if (c.in.size() > MAX_BUFFER) { dead.push_back({fd, "trop de données"}); break; }
            continue;
          }
          if (r == 0) { dead.push_back({fd, "fermé"}); break; }
          if (!wouldBlock(sockErr())) dead.push_back({fd, "erreur"});
          break;
        }
        process(c);
      }
    }
    for (auto &kv : clients) {
      Sock fd = kv.first; Client &c = kv.second;
      while (!c.out.empty()) {
        int r = int(send(fd, reinterpret_cast<const char *>(c.out.data()), int(c.out.size()), SEND_FLAGS));
        if (r > 0) c.out.erase(c.out.begin(), c.out.begin() + r);
        else { if (r < 0 && !wouldBlock(sockErr())) dead.push_back({fd, "envoi impossible"}); break; }
      }
      if (c.closing && !c.closingSince) c.closingSince = t;
      if (c.overflow) dead.push_back({fd, "trop de données en attente"});
      else if (c.closing && (c.out.empty() || t - c.closingSince > 10)) dead.push_back({fd, "terminé"});
      else if (!c.joined && t - c.since > JOIN_TIMEOUT) dead.push_back({fd, "pas d'inscription"});
      else if (c.joined && c.peer == NONE && t - c.lastRx > PAIR_TIMEOUT) dead.push_back({fd, "inactif trop longtemps"});
      else if (c.peer != NONE && t - c.lastRx > IDLE_TIMEOUT) dead.push_back({fd, "silence"});
    }
    for (auto &d : dead) drop(d.first, d.second);
    groupTick();
    sendLists();
    if (t - lastRate >= 1.0) {   // trafic de chaque joueur
      for (auto &kv : clients) { kv.second.rate = double(kv.second.rxBytes - kv.second.rxMark) / (t - lastRate); kv.second.rxMark = kv.second.rxBytes; }
      lastRate = t;
    }
    if (live && (t - lastDraw >= 1.0 || (dirty && t - lastDraw >= 0.2))) { draw(t); lastDraw = t; dirty = false; }
    if (!live && t - lastSummary >= 300) {   // journal : point régulier
      lastSummary = t;
      size_t games = 0;
      for (auto &kv : clients) games += kv.second.peer != NONE;
      logf("%zu joueur(s) connecté(s), %zu partie(s) en cours", clients.size(), games / 2);
    }
  }
}
