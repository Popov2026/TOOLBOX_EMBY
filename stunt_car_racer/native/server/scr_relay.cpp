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
// Robots d'internet (scanners de ports, requêtes web...) : toute connexion dont la première trame
// n'est pas une inscription du jeu est fermée sans être affichée ni journalisée (simple compteur) ;
// une adresse qui insiste (3 essais) n'a plus que 3 secondes pour s'inscrire pendant une heure
// (un vrai jeu s'inscrit tout de suite, même depuis cette adresse).
// Reverse proxy web (HTTPS, par exemple celui d'un NAS) : le jeu peut aussi arriver en WebSocket
// (adresse wss://... côté jeu) ; le serveur accepte la mise à niveau HTTP sur le même port, puis le
// même protocole circule dans des messages binaires. L'adresse du joueur est prise dans les en-têtes
// X-Real-IP / X-Forwarded-For quand la connexion vient du réseau local (le proxy).
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
//                       STATUS (23) ""      (état du serveur pour le menu du jeu, sans inscription : réponse STATUS,
//                                            lignes S/P/G/R séparées par des tabulations, voir statusText)
//                       JOIN (10) "code\0version"  (ancienne version : appariement par code)
//   serveur -> client : INFO (11) "WAIT" | "PAIRED 1" (maître) | "PAIRED 2" | "GONE" | "BUSY" | "ERR message"
//                       LIST (13) "numéro\tnom\n..." (salle privée : joueurs présents et libres)
//                       GSTATE (18) état du groupe : "me N\nhost N\nphase lobby|vote|loading|racing|gone\n
//                                   track T laps L\nm N état nom\n..." (état : lobby, loading, loaded,
//                                   racing, finished:ms, out = abandon)
//                       GEVENT (20) "START circuit tours n1 n2 ..." (ordre sur la grille) | "GO" | "END"
//                                   | "VICTORY n" (seul encore en course, les autres éliminés)
//                                   | "WATCH circuit tours n1 n2 ..." / "WATCHEND" (spectateur : joueur de la
//                                     salle hors de la course ; il reçoit aussi GSTATE et les positions GPOS)
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
#include <iterator>
#include <map>
#include <random>
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
#define SCR_RELAY_VERSION "v97"

namespace {

constexpr uint8_t NF_JOIN = 10, NF_INFO = 11, NF_JOIN2 = 12, NF_LIST = 13, NF_PICK = 14, NF_PROBE = 15, NF_PARAMS = 16, NF_CHAT = 17,
                  NF_GSTATE = 18, NF_GCMD = 19, NF_GEVENT = 20, NF_GPOS = 21, NF_HORN = 22, NF_STATUS = 23;
constexpr size_t GROUP_MAX = 6;
constexpr size_t MAX_CLIENTS = 512, MAX_PER_IP = 16, MAX_BUFFER = 1 << 20;
const Sock NONE = Sock(-1);
constexpr double JOIN_TIMEOUT = 8, PAIR_TIMEOUT = 3600, IDLE_TIMEOUT = 90;
constexpr int BOT_STRIKES = 3;            // connexions de robot avant blocage de l'adresse
constexpr double BOT_BAN = 3600;          // durée de la surveillance (s)
constexpr double SUSPECT_JOIN_TIMEOUT = 3;   // adresse surveillée : délai d'inscription
constexpr size_t MAX_JOIN_FRAME = 512;    // une inscription du jeu fait moins de 100 octets

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ------------------------------------------------------------------ journal et console
FILE *logFile = nullptr;      // --log : copie du journal dans un fichier (Windows : scr_relay.log par défaut)
bool live = false;            // affichage en direct des joueurs (console)
bool dirty = true;            // à redessiner
std::vector<std::string> events;   // derniers événements (affichage en direct)
std::vector<std::string> journal;  // derniers messages du journal (page d'administration)

std::string stamp(const char *fmt) {
  char ts[32]; time_t t = time(nullptr); strftime(ts, sizeof ts, fmt, localtime(&t));
  return ts;
}

void logf(const char *fmt, ...) {
  char msg[512];
  va_list ap; va_start(ap, fmt); std::vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
  std::string ts = stamp("%Y-%m-%d %H:%M:%S");
  if (logFile) { std::fprintf(logFile, "%s  %s\n", ts.c_str(), msg); std::fflush(logFile); }
  journal.push_back(ts + "  " + msg);
  if (journal.size() > 400) journal.erase(journal.begin(), journal.begin() + 100);
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
  bool bot = false;              // ne parle pas le protocole du jeu (robot, scanner)
  bool suspect = false;          // adresse d'un robot récent : délai d'inscription court
  bool ws = false;               // arrivé en WebSocket (par un reverse proxy web)
  bool web = false;              // navigateur : page d'état servie (pas un robot)
  std::string watchKey;          // spectateur de la course de ce groupe (joueur de la salle hors de la course)
  std::vector<uint8_t> raw, wsOut;   // WebSocket : octets reçus non décodés, trames à envoyer
  double lastWsPing = 0;
  bool wsCloseSent = false, wsCloseGot = false;   // fermeture WebSocket propre (les deux sens)
  double closingSince = 0;
  double since = 0, lastRx = 0, pairedAt = 0, lastChat = 0;
  bool inGroup = false;          // course à plusieurs
  bool tnt = false;              // le jeu a les circuits TNT (tirage des courses à deux parmi 16)
  double lastHorn = 0;
  double gposTokens = 100, gposAt = 0;   // positions de course : 100 par seconde au plus (le jeu en envoie 50)
  std::string gkey;              // groupe : code de la salle, ou « DUEL n » (course à deux par appariement)
  std::string gstate = "lobby";  // lobby, loading, loaded, racing, finished:ms, out (voiture détruite)
  uint64_t rxBytes = 0, rxMark = 0;
  double rate = 0;               // octets reçus par seconde
  std::string lastList;          // dernière liste envoyée (salle privée)
};

std::map<Sock, Client> clients;
uint32_t nextId = 1;
uint64_t totalConnections = 0, totalGames = 0, totalBots = 0, totalWeb = 0;
struct Strike { int n = 0; double until = 0, last = 0; };
std::map<std::string, Strike> strikes;   // adresses des robots
struct BotSeen { std::string when, ip, why; };
std::vector<BotSeen> recentBots;          // derniers robots (page d'administration)
void noteBot(const std::string &ip, const std::string &why) {
  recentBots.push_back({stamp("%Y-%m-%d %H:%M:%S"), ip, why});
  if (recentBots.size() > 200) recentBots.erase(recentBots.begin(), recentBots.begin() + 50);
}
// page d'administration (/admin) : mot de passe de scr_relay.ini (« admin = ... »), vide = page désactivée
std::string adminPassword;
struct LicUse { int count = 0; std::string first, last, ip; };
std::map<std::string, LicUse> licenceUses;   // nom du code -> utilisations
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

// codes de licence de la version navigateur (scr_relay.ini : « licence = NOM EMPREINTE ») : quand un
// code sert, la page envoie son empreinte et le relais note le nom dans son journal
std::map<std::string, std::string> licences;   // empreinte -> nom

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
    else if (k == "admin" || k == "admin_password") adminPassword = v;
    else if (k == "licence" || k == "license") {
      size_t sp = v.find_last_of(" \t");
      if (sp != std::string::npos) licences[trim(v.substr(sp + 1))] = trim(v.substr(0, sp));
    }
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

// ------------------------------------------------------------------ WebSocket (reverse proxy web)
std::string sha1(const std::string &msg) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::string m = msg;
  uint64_t bits = uint64_t(msg.size()) * 8;
  m += char(0x80);
  while (m.size() % 64 != 56) m += char(0);
  for (int k = 7; k >= 0; k--) m += char(bits >> (8 * k));
  auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
  for (size_t off = 0; off < m.size(); off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
      w[i] = uint32_t(uint8_t(m[off + 4 * i])) << 24 | uint32_t(uint8_t(m[off + 4 * i + 1])) << 16 |
             uint32_t(uint8_t(m[off + 4 * i + 2])) << 8 | uint8_t(m[off + 4 * i + 3]);
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6; }
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  std::string r;
  for (uint32_t x : h) for (int k = 3; k >= 0; k--) r += char(x >> (8 * k));
  return r;
}

std::string base64(const std::string &s) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string r;
  for (size_t i = 0; i < s.size(); i += 3) {
    uint32_t v = uint32_t(uint8_t(s[i])) << 16 | (i + 1 < s.size() ? uint32_t(uint8_t(s[i + 1])) << 8 : 0) |
                 (i + 2 < s.size() ? uint8_t(s[i + 2]) : 0);
    r += T[v >> 18 & 63]; r += T[v >> 12 & 63];
    r += i + 1 < s.size() ? T[v >> 6 & 63] : '='; r += i + 2 < s.size() ? T[v & 63] : '=';
  }
  return r;
}

// adresse du réseau local (proxy, joueurs de la maison) : pas de limite par adresse ni de surveillance
bool isPrivate(const std::string &ip) {
  unsigned a = 0, b = 0;
  if (std::sscanf(ip.c_str(), "%u.%u", &a, &b) != 2) return false;
  return a == 10 || a == 127 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31) || (a == 169 && b == 254);
}

void wsFrame(Client &c, uint8_t op, const uint8_t *p, size_t n) {   // serveur -> jeu : trame non masquée
  c.wsOut.push_back(uint8_t(0x80 | op));
  if (n < 126) c.wsOut.push_back(uint8_t(n));
  else if (n < 65536) { c.wsOut.push_back(126); c.wsOut.push_back(uint8_t(n >> 8)); c.wsOut.push_back(uint8_t(n)); }
  else { c.wsOut.push_back(127); for (int k = 7; k >= 0; k--) c.wsOut.push_back(uint8_t(uint64_t(n) >> (8 * k))); }
  c.wsOut.insert(c.wsOut.end(), p, p + n);
}

// trames WebSocket reçues -> octets du protocole du jeu (c.in)
void wsDecode(Client &c) {
  for (;;) {
    if (c.raw.size() < 2) return;
    size_t len = c.raw[1] & 0x7f, h = 2;
    if (len == 126) { if (c.raw.size() < 4) return; len = (size_t(c.raw[2]) << 8) | c.raw[3]; h = 4; }
    else if (len == 127) {
      if (c.raw.size() < 10) return;
      len = 0;
      for (int k = 2; k < 10; k++) len = (len << 8) | c.raw[size_t(k)];
      h = 10;
    }
    bool masked = c.raw[1] & 0x80;
    if (masked) h += 4;
    if (len > MAX_BUFFER) { c.closing = true; c.raw.clear(); return; }
    if (c.raw.size() < h + len) return;
    uint8_t op = c.raw[0] & 0x0f;
    std::vector<uint8_t> pay(c.raw.begin() + long(h), c.raw.begin() + long(h + len));
    if (masked) for (size_t i = 0; i < len; i++) pay[i] ^= c.raw[h - 4 + (i & 3)];
    c.raw.erase(c.raw.begin(), c.raw.begin() + long(h + len));
    if (op == 0x8) {   // fermeture demandée par le jeu : réponse, puis fin
      if (!c.wsCloseSent) { wsFrame(c, 0x8, nullptr, 0); c.wsCloseSent = true; }
      c.wsCloseGot = c.closing = true; c.raw.clear(); return;
    }
    if (op == 0x9) { wsFrame(c, 0xA, pay.data(), pay.size()); continue; }                     // ping -> pong
    if (op == 0xA) continue;                                                                    // pong
    c.in.insert(c.in.end(), pay.begin(), pay.end());
  }
}

std::string webPage(const std::string &path, std::string &type, int &status);
std::string adminPage(const std::string &ip);
std::string b64decode(const std::string &s) {
  std::string r; uint32_t v = 0; int n = 0;
  for (char ch : s) {
    int d = ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 : ch >= '0' && ch <= '9' ? ch - '0' + 52 : ch == '+' ? 62 : ch == '/' ? 63 : -1;
    if (d < 0) continue;
    v = (v << 6) | uint32_t(d); n += 6;
    if (n >= 8) { n -= 8; r += char((v >> n) & 255); }
  }
  return r;
}
std::string statusText();

// requête HTTP en tête de connexion : mise à niveau WebSocket (le jeu, par le reverse proxy), sinon
// un robot (réponse 404). Rend vrai quand la connexion est passée en WebSocket.
bool httpUpgrade(Client &c) {
  std::string req(c.in.begin(), c.in.end());
  size_t end = req.find("\r\n\r\n");
  if (end == std::string::npos) {
    if (req.size() > 8192) { c.bot = c.closing = true; c.in.clear(); }
    return false;
  }
  std::map<std::string, std::string> hd;
  size_t pos = req.find("\r\n");
  std::string first = req.substr(0, pos);
  while (pos < end) {
    size_t nl = req.find("\r\n", pos + 2);
    std::string line = req.substr(pos + 2, nl - pos - 2);
    pos = nl;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string k = line.substr(0, colon), v = line.substr(colon + 1);
    for (auto &ch : k) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    hd[k] = v;
  }
  std::vector<uint8_t> rest(c.in.begin() + long(end + 4), c.in.end());
  c.in.clear();
  if (isPrivate(c.ip)) {   // derrière le proxy : adresse réelle du joueur
    std::string real = hd.count("x-real-ip") ? hd["x-real-ip"] : hd.count("x-forwarded-for") ? hd["x-forwarded-for"] : "";
    real = real.substr(0, real.find(','));
    while (!real.empty() && real.back() == ' ') real.pop_back();
    bool okIp = !real.empty() && real.size() <= 45;
    for (char ch : real) okIp = okIp && (std::isxdigit(static_cast<unsigned char>(ch)) || ch == '.' || ch == ':');
    if (okIp) c.ip = real;
  }
  std::string up = hd.count("upgrade") ? hd["upgrade"] : "";
  for (auto &ch : up) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  if (first.compare(0, 4, "GET ") == 0 && up != "websocket") {   // navigateur : page d'état (parties, joueurs, salles)
    std::string path = first.substr(4, first.find(' ', 4) - 4), type;
    int status = 200;
    std::string body, extra;
    if ((path == "/admin" || path.rfind("/admin?", 0) == 0) && !adminPassword.empty()) {
      // page d'administration : mot de passe (authentification HTTP Basic, par le HTTPS du reverse proxy)
      auto st = strikes.find(c.ip);
      std::string auth = hd.count("authorization") ? hd["authorization"] : "", given;
      if (auth.size() > 6 && auth.compare(0, 6, "Basic ") == 0) { given = b64decode(auth.substr(6)); given = given.substr(given.find(':') + 1); }
      if (st != strikes.end() && st->second.until > now()) { status = 403; type = "text/plain; charset=utf-8"; body = "Adresse bloquée pendant 1 h.\n"; }
      else if (given != adminPassword) {
        status = 401; type = "text/plain; charset=utf-8"; body = "Mot de passe requis.\n";
        extra = "WWW-Authenticate: Basic realm=\"Stunt Car Racer relay admin\", charset=\"UTF-8\"\r\n";
        if (!auth.empty()) {   // mauvais mot de passe : compté comme un robot (3 essais, puis adresse bloquée 1 h)
          Strike &s2 = strikes[c.ip];
          if (now() - s2.last > BOT_BAN) s2.n = 0;
          s2.last = now();
          if (++s2.n >= BOT_STRIKES) { s2.until = now() + BOT_BAN; logf("%s : mots de passe d'administration faux, adresse bloquée 1 h", c.ip.c_str()); }
          noteBot(c.ip, "mot de passe d'administration faux");
        }
      } else { type = "text/html; charset=utf-8"; body = adminPage(c.ip); }
    } else body = webPage(path, type, status);
    if (status != 404) {
      const char *line = status == 200 ? "200 OK" : status == 401 ? "401 Unauthorized" : status == 403 ? "403 Forbidden" : "204 No Content";
      std::string resp = "HTTP/1.1 " + std::string(line) + "\r\n" + extra + "Content-Type: " + type +
                         "\r\nContent-Length: " + std::to_string(body.size()) +
                         "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
      c.out.insert(c.out.end(), resp.begin(), resp.end());
      c.web = c.closing = true;
      return false;
    }
  }
  if (first.compare(0, 4, "GET ") != 0 || up != "websocket" || !hd.count("sec-websocket-key")) {
    static const std::string nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    c.out.insert(c.out.end(), nf.begin(), nf.end());
    c.bot = c.closing = true;
    return false;
  }
  size_t same = 0;
  for (auto &kv : clients) same += &kv.second != &c && kv.second.ip == c.ip;
  if (!isPrivate(c.ip) && same >= MAX_PER_IP) { c.bot = c.closing = true; return false; }
  auto st = strikes.find(c.ip);
  c.suspect = st != strikes.end() && st->second.until > now();
  std::string acc = base64(sha1(hd["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
  std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: " + acc + "\r\n\r\n";
  c.wsOut.insert(c.wsOut.end(), resp.begin(), resp.end());
  c.ws = true; c.lastWsPing = now();
  logf("%s : connexion WebSocket (reverse proxy)", c.ip.c_str());
  c.raw = rest;
  wsDecode(c);
  dirty = true;
  return true;
}

std::string who(const Client &c) { return c.name.empty() ? c.ip : c.name + " (" + c.ip + ")"; }

size_t playerCount() {   // jeux inscrits (sans les robots ni les connexions en cours)
  size_t n = 0;
  for (auto &kv : clients) n += kv.second.joined;
  return n;
}

bool free_(const Client &c) { return c.joined && !c.closing && c.peer == NONE && !c.inGroup; }

// ------------------------------------------------------------------ course à plusieurs (groupe d'une salle)
struct Group {
  std::vector<Sock> members;     // ordre d'arrivée ; le premier est l'organisateur
  std::string phase = "lobby";   // lobby, loading, racing
  int track = 0, laps = 3;
  size_t starters = 0;           // joueurs au départ de la course en cours
  std::vector<int> bag; int bagSize = 0;   // course à deux : circuits pas encore proposés
  bool duel = false;             // course à deux (premier joueur disponible, ou « Affronter » dans une salle) :
                                 // chacun mène son jeu, circuit proposé et accepté par les deux (phase vote)
  double deadline = 0, firstFinish = 0, startedAt = 0;
  std::string lastState;
};
std::map<std::string, Group> groups;   // salle -> groupe

void groupSend(Group &g, uint8_t type, const std::string &msg) {
  for (Sock f : g.members) { auto it = clients.find(f); if (it != clients.end()) frame(it->second, type, msg); }
}

// spectateurs : joueurs de la salle de la course (même version), ni dans la course ni en partie, pendant
// le chargement et la course ; ils reçoivent l'état du groupe et les positions, et regardent la course
std::string groupRoom(const Group &g) {
  for (Sock f : g.members) { auto it = clients.find(f); if (it != clients.end() && it->second.mode == Mode::Room) return it->second.room; }
  return "";
}
std::vector<Sock> watchersOf(const std::string &key, const Group &g) {
  std::vector<Sock> v;
  const std::string room = groupRoom(g);
  if (room.empty() || (g.phase != "loading" && g.phase != "racing") || g.members.empty()) return v;
  const std::string &ver = clients[g.members[0]].version;
  for (auto &kv : clients) {
    const Client &c = kv.second;
    if (c.joined && !c.closing && c.mode == Mode::Room && c.room == room && !c.inGroup && c.peer == NONE && c.version == ver &&
        (c.watchKey.empty() || c.watchKey == key))
      v.push_back(kv.first);
  }
  return v;
}
void groupState(const std::string &room, bool force = false);
// nouveaux spectateurs : départ de la course à regarder ; anciens (course finie, partis) : fin
void watchSync(const std::string &key) {
  auto git = groups.find(key);
  std::vector<Sock> now;
  bool added = false;
  if (git != groups.end()) now = watchersOf(key, git->second);
  for (auto &kv : clients) {
    Client &c = kv.second;
    bool want = std::find(now.begin(), now.end(), kv.first) != now.end();
    if (want && c.watchKey.empty()) {
      const Group &g = git->second;
      c.watchKey = key;
      std::string msg = "WATCH " + std::to_string(g.track) + " " + std::to_string(g.laps);
      for (Sock f : g.members) msg += " " + std::to_string(clients[f].id);
      frame(c, NF_GEVENT, msg);
      logf("%s regarde la course %s (spectateur)", who(c).c_str(), key.c_str());
      dirty = added = true;
    } else if (!want && c.watchKey == key) {
      c.watchKey.clear();
      frame(c, NF_GEVENT, "WATCHEND");
      dirty = true;
    }
  }
  if (git != groups.end() && added) groupState(key, true);   // état complet pour les nouveaux spectateurs
}

void groupState(const std::string &room, bool force) {
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
  for (auto &kv : clients)   // spectateurs : même état (ils ne sont pas dans la liste des membres)
    if (kv.second.watchKey == room) frame(kv.second, NF_GSTATE, "me " + std::to_string(kv.second.id) + "\n" + body);
  dirty = true;
}

void matchAuto(Client &c);
void duelDissolve(const std::string &key);

// circuit tiré au sort pour une course à deux : 8 d'origine, ou 16 si tous ont les circuits TNT ;
// tirage « sans remise » (sac mélangé) : chaque circuit est proposé une fois avant qu'un circuit revienne
std::mt19937 &rng() {
  static std::mt19937 g(unsigned(std::random_device{}()) ^ unsigned(std::time(nullptr)) ^ unsigned(std::clock()));
  return g;
}
int duelDraw(Group &g, int avoid) {
  bool tnt = !g.members.empty();
  for (Sock f : g.members) { auto it = clients.find(f); tnt = tnt && it != clients.end() && it->second.tnt; }
  const int n = tnt ? 16 : 8;
  if (g.bagSize != n) { g.bag.clear(); g.bagSize = n; }
  if (g.bag.empty()) {
    for (int k = 0; k < n; k++) if (k != avoid) g.bag.push_back(k);
    std::shuffle(g.bag.begin(), g.bag.end(), rng());
  }
  const int t = g.bag.back();
  g.bag.pop_back();
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
  g.phase = "loading"; g.deadline = now() + 30; g.firstFinish = 0; g.starters = g.members.size();
  for (Sock f : g.members) clients[f].gstate = "loading";
  // grille de départ (3 rangées de 2) : ordre tiré au sort à chaque course
  std::vector<uint32_t> grid;
  for (Sock f : g.members) grid.push_back(clients[f].id);
  for (size_t i = grid.size(); i > 1; i--) std::swap(grid[i - 1], grid[size_t(std::rand()) % i]);
  std::string startMsg = "START " + std::to_string(g.track) + " " + std::to_string(g.laps);
  for (uint32_t id : grid) startMsg += " " + std::to_string(id);
  groupSend(g, NF_GEVENT, startMsg);
  logf("%s : course à %zu joueurs lancée (circuit %d, %d tour(s))", key.c_str(), g.members.size(), g.track + 1, g.laps);
  watchSync(key);   // les autres joueurs de la salle deviennent spectateurs
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
  std::vector<std::string> ended;        // courses finies : fin pour leurs spectateurs
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
      // seul encore en course, tous les autres éliminés (aucun arrivé) : victoire
      {
        int racing = 0, out = 0, fin = 0; Sock last = NONE;
        for (Sock f : g.members) {
          const std::string &st = clients[f].gstate;
          if (st == "racing") { racing++; last = f; } else if (st == "out") out++; else if (st.rfind("finished", 0) == 0) fin++;
        }
        if (racing == 1 && fin == 0 && g.starters >= 2 && out + (g.starters - g.members.size()) >= 1) {   // éliminés ou partis
          Client &w = clients[last];
          long ms = long((t - g.startedAt) * 1000);
          w.gstate = "finished:" + std::to_string(ms);
          groupSend(g, NF_GEVENT, "VICTORY " + std::to_string(w.id));
          logf("%s : %s gagne, les autres joueurs sont éliminés", kv.first.c_str(), w.name.c_str());
          all = true;
        }
      }
      // fin : tous arrivés ou abandonnés, ou 2 minutes après le premier, ou 30 minutes de course
      if (all || (g.firstFinish && t - g.firstFinish > 120) || t - g.startedAt > 1800) {
        g.phase = g.duel ? "vote" : "lobby";
        if (g.duel) g.track = duelDraw(g, g.track);   // revanche : autre circuit proposé
        groupSend(g, NF_GEVENT, "END");
        groupState(kv.first, true);
        for (Sock f : g.members) if (clients[f].gstate.rfind("finished", 0) != 0 && clients[f].gstate != "out") clients[f].gstate = g.duel ? "vote" : "lobby";
        logf("%s : course terminée", kv.first.c_str());
        ended.push_back(kv.first);
        if (g.duel && g.members.size() < 2) { duelDoomed.push_back(kv.first); }
      }
    }
  }
  for (auto &key : duelDoomed) duelDissolve(key);
  for (auto &key : ended) watchSync(key);
  // spectateurs : arrivées dans la salle pendant une course, groupes disparus
  static double lastSync = 0;
  if (t - lastSync > 1) {
    lastSync = t;
    for (auto &kv : groups) if (kv.second.phase == "loading" || kv.second.phase == "racing") watchSync(kv.first);
    for (auto &kv : clients)
      if (!kv.second.watchKey.empty() && !groups.count(kv.second.watchKey)) { kv.second.watchKey.clear(); frame(kv.second, NF_GEVENT, "WATCHEND"); }
  }
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
  if (c.web) totalWeb++;   // page d'état servie à un navigateur
  else if (!c.joined && !c.probe && isPrivate(c.ip)) { totalBots++; noteBot(c.ip, why); }   // réseau local (proxy compris) : jamais surveillé
  else if (!c.joined && !c.probe) {   // jamais inscrit : robot (ou jeu coupé avant l'inscription), pas de journal
    totalBots++;
    noteBot(c.ip, why);
    Strike &s = strikes[c.ip];
    if (now() - s.last > BOT_BAN) s.n = 0;
    s.last = now();
    if (++s.n >= BOT_STRIKES && s.until < now()) { s.until = now() + BOT_BAN; logf("%s : robot (pas un jeu), adresse surveillée pendant 1 h", c.ip.c_str()); }
  } else if (!c.probe) logf("%s déconnecté (%s)", who(c).c_str(), why);
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
  if (mode == Mode::Room) {
    logf("%s entre dans la salle %s", who(c).c_str(), c.room.c_str());
    for (auto &kv : clients) {   // même salle, autre version : invisibles l'un pour l'autre, on prévient
      Client &o = kv.second;
      if (&o == &c || !o.joined || o.closing || o.mode != Mode::Room || o.room != c.room || o.version == c.version) continue;
      frame(c, NF_INFO, "MISMATCH " + o.version);
      frame(o, NF_INFO, "MISMATCH " + c.version);
    }
    return;
  }
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
  if (best) { duelCreate(*best, c); return; }
  logf("%s attend un adversaire (automatique)", who(c).c_str());
  // joueurs libres avec une autre version du jeu : jamais associés, on prévient les deux
  for (auto &kv : clients) {
    Client &o = kv.second;
    if (&o == &c || !free_(o) || o.mode != Mode::Auto || o.version == c.version) continue;
    frame(c, NF_INFO, "MISMATCH " + o.version);
    frame(o, NF_INFO, "MISMATCH " + c.version);
    logf("%s (%s) et %s (%s) : versions différentes, pas de course possible", who(c).c_str(), c.version.c_str(), who(o).c_str(), o.version.c_str());
  }
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

// positions de course : relayées à tous les membres et spectateurs, donc limitées par joueur (un jeu
// trafiqué qui en enverrait des milliers par seconde saturerait la connexion des autres)
bool gposAllowed(Client &c) {
  const double t = now();
  c.gposTokens = std::min(100.0, c.gposTokens + (t - c.gposAt) * 100.0);
  c.gposAt = t;
  if (c.gposTokens < 1) return false;
  c.gposTokens -= 1;
  return true;
}

// trames complètes reçues : inscription avant l'appariement, retransmission ensuite
void process(Client &c) {
  size_t pos = 0;
  if (c.ws) wsDecode(c);
  else if (!c.joined && !c.in.empty() && c.in[0] == 'G' && !httpUpgrade(c)) return;   // HTTP : WebSocket ou robot
  // avant l'inscription, seules JOIN2, JOIN et PROBE (courtes) sont admises : le reste est un robot
  if (!c.joined && !c.in.empty()) {
    uint8_t t0 = c.in[0];
    bool bad = t0 != NF_JOIN2 && t0 != NF_JOIN && t0 != NF_PROBE && t0 != NF_STATUS;
    if (!bad && c.in.size() >= 3) bad = ((size_t(c.in[1]) << 8) | c.in[2]) > MAX_JOIN_FRAME;
    if (bad) { c.bot = c.closing = true; c.in.clear(); return; }
  }
  while (c.in.size() - pos >= 3) {
    size_t n = (size_t(c.in[pos + 1]) << 8) | c.in[pos + 2];
    if (c.in.size() - pos < 3 + n) break;
    uint8_t type = c.in[pos];
    const uint8_t *p = c.in.data() + pos + 3;
    if (!c.joined) {
      auto f = fields(p, n);
      if (type == NF_JOIN2 && f.size() >= 3 && f[0] != "AUTO" && f[0] != "SALLE") { c.bot = c.closing = true; break; }
      if (type == NF_JOIN2 && f.size() >= 3) {
        c.tnt = f.size() > 4 && f[4] == "tnt";   // circuits TNT présents chez ce joueur
        onJoin(c, f[0] == "SALLE" ? Mode::Room : Mode::Auto, f[1], f[2], f.size() > 3 ? f[3] : "");
      }
      else if (type == NF_JOIN) onJoin(c, Mode::Code, f[0], f.size() > 1 ? f[1] : "", "");
      else if (type == NF_PROBE) {   // le jeu vérifie que le serveur répond
        if (n > 4 && n <= 40 && std::memcmp(p, "LIC ", 4) == 0) {   // version navigateur : code de licence utilisé
          auto it = licences.find(std::string(reinterpret_cast<const char *>(p) + 4, n - 4));
          if (it != licences.end()) {
            logf("CODE DE LICENCE UTILISÉ : « %s » (disquette chargée dans la version navigateur, %s)", it->second.c_str(), c.ip.c_str());
            LicUse &u = licenceUses[it->second];
            u.last = stamp("%Y-%m-%d %H:%M:%S");
            if (!u.count++) u.first = u.last;
            u.ip = c.ip;
          }
        }
        size_t players = 0, games = 0;
        for (auto &kv : clients) { players += kv.second.joined; games += kv.second.peer != NONE; }
        // version du serveur (= version du jeu) : le jeu prévient le joueur quand une nouvelle version est sortie
        frame(c, NF_INFO, "SERVER " + std::to_string(players) + " " + std::to_string(games / 2) + ";" + paramsText() + ";version=" SCR_RELAY_VERSION);
        c.probe = c.closing = true;
        if (c.ws) logf("%s : test du serveur par WebSocket, réponse envoyée", c.ip.c_str());
      }
      else if (type == NF_STATUS) {   // état du serveur pour le menu du jeu (parties, joueurs, salles)
        frame(c, NF_STATUS, statusText());
        c.probe = c.closing = true;
      }
      else { c.bot = c.closing = true; break; }
    } else if (c.peer != NONE) {
      Client &o = clients[c.peer];
      if (o.out.size() < MAX_BUFFER) o.out.insert(o.out.end(), c.in.begin() + long(pos), c.in.begin() + long(pos + 3 + n));
      else o.overflow = true;
    } else if (type == NF_PICK) onPick(c, std::string(reinterpret_cast<const char *>(p), n));
    else if (type == NF_GCMD) groupCmd(c, std::string(reinterpret_cast<const char *>(p), std::min<size_t>(n, 64)));
    else if (type == NF_GPOS && c.inGroup && n <= 64 && gposAllowed(c)) {   // position : aux autres membres, avec le numéro
      std::string m(4, '\0');
      for (int k = 0; k < 4; k++) m[size_t(k)] = char((c.id >> (24 - 8 * k)) & 255);
      m.append(reinterpret_cast<const char *>(p), n);
      for (Sock f : groups[c.gkey].members) if (f != c.fd) frame(clients[f], NF_GPOS, m);
      for (auto &kv : clients) if (kv.second.watchKey == c.gkey) frame(kv.second, NF_GPOS, m);   // spectateurs
    }
    else if (type == NF_HORN && c.inGroup && now() - c.lastHorn > 0.25) {   // klaxon : aux autres membres
      c.lastHorn = now();
      std::string m(4, '\0');
      for (int k = 0; k < 4; k++) m[size_t(k)] = char((c.id >> (24 - 8 * k)) & 255);
      for (Sock f : groups[c.gkey].members) if (f != c.fd) frame(clients[f], NF_HORN, m);
    }
    else if (type == NF_CHAT && c.inGroup && now() - c.lastChat > 0.3) {   // discussion de la course (à deux ou à plusieurs)
      c.lastChat = now();
      std::string text = cleanName(std::string(reinterpret_cast<const char *>(p), std::min<size_t>(n, 360)), 120);
      if (text.empty()) { pos += 3 + n; continue; }
      std::string msg = (c.name.empty() ? "Joueur " + std::to_string(c.id) : c.name) + "\t" + text;
      for (Sock f : groups[c.gkey].members) if (f != c.fd) frame(clients[f], NF_CHAT, msg);
      logf("%s, %s : %s", c.gkey.c_str(), c.name.c_str(), text.c_str());
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
  if (!c.watchKey.empty()) return "spectateur de la course";
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
  std::snprintf(b, sizeof b, "Joueurs connectés : %zu    parties en cours : %zu    depuis le lancement : %llu parties, %llu robots refusés, %llu pages web\n\n",
                playerCount(), games / 2, static_cast<unsigned long long>(totalGames), static_cast<unsigned long long>(totalBots),
                static_cast<unsigned long long>(totalWeb));
  o += b;
  o += "\x1b[7m" + col(" N°", 5) + col("Joueur", 18) + col("Adresse IP", 16) + col("Mode", 13) + col("Salle", 10) +
       col("État", 36) + col("Connecté", 10) + col("Trafic", 9) + "\x1b[0m\n";
  std::vector<const Client *> list;
  for (auto &kv : clients) if (kv.second.joined || kv.second.probe) list.push_back(&kv.second);   // pas les robots
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

// ------------------------------------------------------------------ page web d'état (https://... par le proxy)
const char *const TRACK_NAMES[16] = {"Little Ramp", "Stepping Stones", "Hump Back", "Big Ramp", "Ski Jump", "Draw Bridge",
                                     "High Jump", "Roller Coaster", "Dizzy Descent", "Witty Way", "Crazy Caper", "Amazing Adept",
                                     "Jerkily Jump", "Evilly Episode", "Teasing Temper", "Rat Race"};
std::string trackLabel(int t) {
  if (t < 0 || t > 15) return "?";
  return std::string(TRACK_NAMES[t]) + (t >= 8 ? " (TNT)" : "");
}

std::string esc(const std::string &s) {   // HTML et JSON : noms venus des joueurs
  std::string r;
  for (char ch : s) {
    if (ch == '&') r += "&amp;"; else if (ch == '<') r += "&lt;"; else if (ch == '>') r += "&gt;";
    else if (ch == '"') r += "&quot;"; else if (ch == '\\') r += "&#92;"; else r += ch;
  }
  return r;
}

std::string playerName(const Client &c) { return c.name.empty() ? "Player " + std::to_string(c.id) : c.name; }

std::string stateEn(const Client &c, double t) {   // état d'un joueur, en anglais (page web)
  if (c.peer != NONE) {
    auto p = clients.find(c.peer);
    return "racing against " + (p == clients.end() ? std::string("?") : playerName(p->second)) + " (" + duration(t - c.pairedAt) + ")";
  }
  if (c.inGroup) {
    const std::string &g = c.gstate;
    if (g.rfind("finished:", 0) == 0) { long ms = std::atol(g.c_str() + 9); char b[64]; std::snprintf(b, sizeof b, "finished in %ld.%ld s", ms / 1000, ms % 1000 / 100); return b; }
    if (g == "out") return "out (car wrecked)";
    if (g == "vote") return "choosing the track";
    if (g == "accept") return "track accepted";
    if (g == "racing") return "racing";
    if (g == "lobby") return "waiting for the start";
    return "loading the track";
  }
  if (!c.watchKey.empty()) return "watching the race (spectator)";
  return c.mode == Mode::Room ? "in the room, free" : "waiting for an opponent";
}

std::string phaseEn(const std::string &ph) {
  if (ph == "vote") return "choosing the track";
  if (ph == "loading") return "loading";
  if (ph == "racing") return "race in progress";
  if (ph == "lobby") return "waiting for the start";
  return ph;
}

std::string webPage(const std::string &path, std::string &type, int &status) {
  const double t = now();
  if (path == "/favicon.ico" || path == "/robots.txt") {
    type = "text/plain"; status = path == "/robots.txt" ? 200 : 204;
    return path == "/robots.txt" ? "User-agent: *\nDisallow: /\n" : "";
  }
  std::vector<const Client *> players;
  for (auto &kv : clients) if (kv.second.joined && !kv.second.closing) players.push_back(&kv.second);
  std::sort(players.begin(), players.end(), [](const Client *a, const Client *b) { return a->id < b->id; });
  // parties : groupes (course à deux, course à plusieurs) et anciennes parties appariées
  struct Game { std::string kind, where, track, phase; std::vector<std::string> who; };
  std::vector<Game> games;
  for (auto &kv : groups) {
    const Group &g = kv.second;
    if (g.members.empty()) continue;
    Game gm;
    gm.kind = g.duel ? "Two-player race" : "Multi-car race";
    gm.where = kv.first.rfind("DUEL", 0) == 0 ? "first available player" : "room " + kv.first;
    gm.track = trackLabel(g.track) + ", " + std::to_string(g.laps) + " lap" + (g.laps > 1 ? "s" : "");
    gm.phase = phaseEn(g.phase);
    for (Sock f : g.members) {
      auto it = clients.find(f);
      if (it != clients.end()) gm.who.push_back(playerName(it->second) + " - " + stateEn(it->second, t));
    }
    games.push_back(gm);
  }
  for (const Client *c : players) {
    if (c->peer == NONE || c->peer < c->fd) continue;   // une fois par paire
    auto p = clients.find(c->peer);
    if (p == clients.end()) continue;
    games.push_back({"Computer Link", c->room.empty() ? "-" : "code " + c->room, "-", "in progress (" + duration(t - c->pairedAt) + ")",
                     {playerName(*c), playerName(p->second)}});
  }
  std::map<std::string, std::vector<std::string>> rooms;   // salles privées : joueurs présents
  for (const Client *c : players) if (c->mode == Mode::Room) rooms[c->room].push_back(playerName(*c));
  auto modeEn = [](const Client &c) { return c.mode == Mode::Auto ? "first available" : c.mode == Mode::Room ? "private room" : c.mode == Mode::Code ? "code" : "-"; };

  if (path == "/status.json" || path == "/api") {
    type = "application/json; charset=utf-8";
    std::string j = "{\"version\":\"" SCR_RELAY_VERSION "\",\"uptime_s\":" + std::to_string(long(t - startedAt)) +
                    ",\"players\":[";
    for (size_t i = 0; i < players.size(); i++) {
      const Client &c = *players[i];
      j += std::string(i ? "," : "") + "{\"name\":\"" + esc(playerName(c)) + "\",\"mode\":\"" + modeEn(c) + "\",\"room\":\"" +
           esc(c.mode == Mode::Room ? c.room : "") + "\",\"state\":\"" + esc(stateEn(c, t)) + "\",\"connected_s\":" +
           std::to_string(long(t - c.since)) + "}";
    }
    j += "],\"games\":[";
    for (size_t i = 0; i < games.size(); i++) {
      j += std::string(i ? "," : "") + "{\"type\":\"" + games[i].kind + "\",\"where\":\"" + esc(games[i].where) + "\",\"track\":\"" +
           esc(games[i].track) + "\",\"phase\":\"" + esc(games[i].phase) + "\",\"players\":[";
      for (size_t k = 0; k < games[i].who.size(); k++) j += std::string(k ? "," : "") + "\"" + esc(games[i].who[k]) + "\"";
      j += "]}";
    }
    j += "],\"rooms\":[";
    size_t i = 0;
    for (auto &r : rooms) {
      j += std::string(i++ ? "," : "") + "{\"code\":\"" + esc(r.first) + "\",\"players\":[";
      for (size_t k = 0; k < r.second.size(); k++) j += std::string(k ? "," : "") + "\"" + esc(r.second[k]) + "\"";
      j += "]}";
    }
    j += "],\"total_games\":" + std::to_string(totalGames) + "}";
    return j;
  }
  if (path != "/" && path.rfind("/?", 0) != 0 && path != "/index.html") { status = 404; return ""; }
  type = "text/html; charset=utf-8";
  std::string h =
      "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<meta http-equiv=\"refresh\" content=\"5\"><title>Stunt Car Racer - relay server</title><style>"
      "body{margin:0;background:#101418;color:#e8e8e8;font:15px/1.45 system-ui,sans-serif}main{max-width:960px;margin:0 auto;padding:16px}"
      "h1{font-size:22px;margin:4px 0 2px;color:#ffd84a}h2{font-size:17px;margin:22px 0 8px;color:#ffb347}"
      ".sub{color:#9aa4ae;font-size:13px}.stats{display:flex;flex-wrap:wrap;gap:10px;margin:14px 0}"
      ".stat{background:#1c232b;border-radius:8px;padding:10px 14px;min-width:120px}.stat b{display:block;font-size:22px;color:#fff}"
      "table{width:100%;border-collapse:collapse;background:#161c22;border-radius:8px;overflow:hidden}"
      "th,td{text-align:left;padding:7px 10px;border-bottom:1px solid #26303a;vertical-align:top}th{background:#1f2831;color:#b8c2cc;font-weight:600}"
      ".none{color:#7c8792;font-style:italic;padding:10px}.tag{display:inline-block;background:#2a3642;border-radius:4px;padding:1px 6px;font-size:12px}"
      "ul{margin:0;padding-left:18px}@media(max-width:600px){th,td{padding:6px}}</style></head><body><main>"
      "<h1>Stunt Car Racer - relay server</h1><div class=\"sub\">Version " SCR_RELAY_VERSION " &middot; up for " + duration(t - startedAt) +
      " &middot; refreshed every 5 s</div><div class=\"stats\">";
  auto stat = [&](const std::string &n, const std::string &v) { h += "<div class=\"stat\"><b>" + v + "</b>" + n + "</div>"; };
  stat("players online", std::to_string(players.size()));
  stat("games in progress", std::to_string(games.size()));
  stat("private rooms", std::to_string(rooms.size()));
  stat("games since start", std::to_string(totalGames));
  h += "</div><h2>Games in progress</h2>";
  if (games.empty()) h += "<div class=\"none\">No game in progress.</div>";
  else {
    h += "<table><tr><th>Game</th><th>Track</th><th>Status</th><th>Players</th></tr>";
    for (auto &g : games) {
      h += "<tr><td>" + esc(g.kind) + "<br><span class=\"tag\">" + esc(g.where) + "</span></td><td>" + esc(g.track) + "</td><td>" + esc(g.phase) + "</td><td><ul>";
      for (auto &w : g.who) h += "<li>" + esc(w) + "</li>";
      h += "</ul></td></tr>";
    }
    h += "</table>";
  }
  h += "<h2>Players online</h2>";
  if (players.empty()) h += "<div class=\"none\">Nobody is connected.</div>";
  else {
    h += "<table><tr><th>Player</th><th>Mode</th><th>Room</th><th>State</th><th>Connected</th></tr>";
    for (const Client *c : players)
      h += "<tr><td>" + esc(playerName(*c)) + "</td><td>" + modeEn(*c) + "</td><td>" + esc(c->mode == Mode::Room ? c->room : "-") + "</td><td>" +
           esc(stateEn(*c, t)) + "</td><td>" + duration(t - c->since) + "</td></tr>";
    h += "</table>";
  }
  h += "<h2>Private rooms</h2>";
  if (rooms.empty()) h += "<div class=\"none\">No private room open.</div>";
  else {
    h += "<table><tr><th>Room code</th><th>Players</th></tr>";
    for (auto &r : rooms) {
      h += "<tr><td>" + esc(r.first) + "</td><td><ul>";
      for (auto &n : r.second) h += "<li>" + esc(n) + "</li>";
      h += "</ul></td></tr>";
    }
    h += "</table>";
  }
  h += "<p class=\"sub\">Machine-readable: <a style=\"color:#8cc8ff\" href=\"/status.json\">/status.json</a></p></main></body></html>";
  return h;
}

// page d'administration (/admin, mot de passe « admin » de scr_relay.ini) : codes de licence utilisés,
// joueurs avec leur adresse et ce qu'ils font, robots, journal. Jamais visible sans le mot de passe.
std::string modeFr(const Client &c) {
  return c.mode == Mode::Auto ? "premier joueur dispo" : c.mode == Mode::Room ? (c.room == "PUBLIC" ? "salle publique" : "salle privée") : c.mode == Mode::Code ? "code (ancien jeu)" : "-";
}
std::string stateFr(const Client &c, double t) {   // ce que fait un joueur (page d'administration)
  if (c.peer != NONE) {
    auto p = clients.find(c.peer);
    return "course à deux contre " + (p == clients.end() ? std::string("?") : playerName(p->second)) + " (" + duration(t - c.pairedAt) + ")";
  }
  if (c.inGroup) {
    const std::string &g = c.gstate;
    if (g.rfind("finished:", 0) == 0) { long ms = std::atol(g.c_str() + 9); char b[64]; std::snprintf(b, sizeof b, "arrivé en %ld,%ld s", ms / 1000, ms % 1000 / 100); return b; }
    if (g == "out") return "éliminé (voiture détruite)";
    if (g == "vote") return "choisit le circuit";
    if (g == "accept") return "circuit accepté";
    if (g == "racing") return "en course";
    if (g == "lobby") return "attend le départ";
    return "charge le circuit";
  }
  if (!c.watchKey.empty()) return "regarde la course (spectateur)";
  return c.mode == Mode::Room ? "dans la salle, libre" : "attend un adversaire";
}

std::string adminPage(const std::string &viewer) {
  const double t = now();
  std::string h =
      "<!doctype html><html lang=\"fr\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<meta name=\"robots\" content=\"noindex\"><meta http-equiv=\"refresh\" content=\"5\"><title>Relais SCR - administration</title><style>"
      "body{margin:0;background:#0d1115;color:#e8e8e8;font:14px/1.45 system-ui,sans-serif}main{max-width:1200px;margin:0 auto;padding:16px}"
      "h1{font-size:22px;margin:4px 0 2px;color:#ffd84a}h2{font-size:16px;margin:24px 0 8px;color:#ffb347}"
      ".sub{color:#9aa4ae;font-size:13px}.stats{display:flex;flex-wrap:wrap;gap:10px;margin:14px 0}"
      ".stat{background:#1c232b;border-radius:8px;padding:10px 14px;min-width:110px}.stat b{display:block;font-size:22px;color:#fff}"
      ".stat.hot{background:#3a2a10;outline:1px solid #ffb347}"
      "table{width:100%;border-collapse:collapse;background:#151b21;border-radius:8px;overflow:hidden}"
      "th,td{text-align:left;padding:6px 9px;border-bottom:1px solid #25303a;vertical-align:top}th{background:#1e2730;color:#b8c2cc;font-weight:600}"
      ".none{color:#7c8792;font-style:italic;padding:8px}.ok{color:#5fd38a;font-weight:700}.no{color:#7c8792}.bad{color:#ff7b6b}"
      ".mono{font-family:ui-monospace,Consolas,monospace;font-size:12.5px}.log{background:#151b21;border-radius:8px;padding:8px 10px;max-height:420px;overflow:auto}"
      ".log div{white-space:pre-wrap;border-bottom:1px solid #1f2830;padding:2px 0}.lic{color:#ffd84a;font-weight:700}"
      ".wrap{overflow-x:auto}</style></head><body><main>"
      "<h1>Relais Stunt Car Racer &middot; administration</h1><div class=\"sub\">Version " SCR_RELAY_VERSION " &middot; en service depuis " +
      duration(t - startedAt) + " &middot; actualisé toutes les 5 s &middot; vous : " + esc(viewer) + "</div><div class=\"stats\">";
  size_t players = 0, pending = 0, blocked = 0, used = 0;
  for (auto &kv : clients) { if (kv.second.joined && !kv.second.closing) players++; else if (!kv.second.closing) pending++; }
  for (auto &kv : strikes) blocked += kv.second.until > t;
  for (auto &kv : licenceUses) used += kv.second.count > 0;
  auto stat = [&](const std::string &n, const std::string &v, bool hot = false) {
    h += "<div class=\"stat" + std::string(hot ? " hot" : "") + "\"><b>" + v + "</b>" + n + "</div>";
  };
  stat("joueurs en ligne", std::to_string(players));
  stat("connexions en attente", std::to_string(pending));
  stat("parties depuis le lancement", std::to_string(totalGames));
  stat("codes de licence utilisés", std::to_string(used) + " / " + std::to_string(licences.size()), used > 0);
  stat("robots refusés", std::to_string(totalBots));
  stat("adresses bloquées", std::to_string(blocked), blocked > 0);
  stat("pages d'état vues", std::to_string(totalWeb));

  h += "</div><h2>Codes de licence (version navigateur)</h2>";
  if (licences.empty()) h += "<div class=\"none\">Aucun code dans scr_relay.ini (lignes « licence = NOM EMPREINTE »).</div>";
  else {
    h += "<div class=\"wrap\"><table><tr><th>Code</th><th>Utilisé</th><th>Fois</th><th>Première fois</th><th>Dernière fois</th><th>Adresse</th></tr>";
    std::vector<std::string> names;
    for (auto &kv : licences) names.push_back(kv.second);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    for (auto &n : names) {
      auto u = licenceUses.find(n);
      bool yes = u != licenceUses.end() && u->second.count > 0;
      h += "<tr><td class=\"lic\">" + esc(n) + "</td><td>" + std::string(yes ? "<span class=\"ok\">OUI</span>" : "<span class=\"no\">pas encore</span>") +
           "</td><td>" + (yes ? std::to_string(u->second.count) : "-") + "</td><td>" + (yes ? esc(u->second.first) : "-") + "</td><td>" +
           (yes ? esc(u->second.last) : "-") + "</td><td class=\"mono\">" + (yes ? esc(u->second.ip) : "-") + "</td></tr>";
    }
    h += "</table></div><p class=\"sub\">Compté depuis le lancement du serveur ; le journal scr_relay.log garde tout.</p>";
  }

  h += "<h2>Joueurs connectés</h2>";
  std::vector<const Client *> list;
  for (auto &kv : clients) if (kv.second.joined && !kv.second.closing) list.push_back(&kv.second);
  std::sort(list.begin(), list.end(), [](const Client *a, const Client *b) { return a->id < b->id; });
  if (list.empty()) h += "<div class=\"none\">Personne n'est connecté.</div>";
  else {
    h += "<div class=\"wrap\"><table><tr><th>N°</th><th>Pseudo</th><th>Adresse</th><th>Version</th><th>Mode</th><th>Salle / course</th>"
         "<th>Ce qu'il fait</th><th>Connecté depuis</th><th>Débit</th></tr>";
    for (const Client *c : list) {
      std::string where = c->inGroup ? c->gkey : c->mode == Mode::Room ? c->room : "-";
      if (!c->watchKey.empty()) where += " (regarde " + c->watchKey + ")";
      h += "<tr><td>" + std::to_string(c->id) + "</td><td>" + esc(playerName(*c)) + "</td><td class=\"mono\">" + esc(c->ip) +
           (c->ws ? " <span class=\"sub\">WS</span>" : "") + "</td><td>" + esc(c->version) + (c->tnt ? " <span class=\"sub\">TNT</span>" : "") +
           "</td><td>" + modeFr(*c) + "</td><td>" + esc(where) + "</td><td>" + esc(stateFr(*c, t)) +
           (c->inGroup ? " <span class=\"sub\">(" + esc(c->gstate) + ")</span>" : "") + "</td><td>" + duration(t - c->since) +
           "</td><td>" + std::to_string(int(c->rate / 1024)) + " Ko/s</td></tr>";
    }
    h += "</table></div>";
  }

  h += "<h2>Robots et adresses bloquées</h2>";
  std::string bl;
  for (auto &kv : strikes)
    if (kv.second.until > t) bl += "<tr><td class=\"mono\">" + esc(kv.first) + "</td><td>" + std::to_string(kv.second.n) + "</td><td>encore " + duration(kv.second.until - t) + "</td></tr>";
  if (bl.empty()) h += "<div class=\"none\">Aucune adresse bloquée.</div>";
  else h += "<div class=\"wrap\"><table><tr><th>Adresse bloquée</th><th>Tentatives</th><th>Blocage</th></tr>" + bl + "</table></div>";
  h += "<p class=\"sub\">Derniers robots (connexions qui ne sont pas le jeu) :</p>";
  if (recentBots.empty()) h += "<div class=\"none\">Aucun robot depuis le lancement.</div>";
  else {
    h += "<div class=\"wrap\"><table><tr><th>Quand</th><th>Adresse</th><th>Raison</th></tr>";
    for (size_t i = recentBots.size(), k = 0; i-- > 0 && k < 40; k++)
      h += "<tr><td>" + esc(recentBots[i].when) + "</td><td class=\"mono\">" + esc(recentBots[i].ip) + "</td><td>" + esc(recentBots[i].why) + "</td></tr>";
    h += "</table></div>";
  }

  h += "<h2>Journal (le plus récent en haut)</h2><div class=\"log mono\">";
  for (size_t i = journal.size(), k = 0; i-- > 0 && k < 200; k++) {
    bool lic = journal[i].find("CODE DE LICENCE") != std::string::npos;
    h += "<div" + std::string(lic ? " class=\"lic\"" : "") + ">" + esc(journal[i]) + "</div>";
  }
  if (journal.empty()) h += "<div class=\"none\">Journal vide.</div>";
  h += "</div><p class=\"sub\">Page publique (sans mot de passe) : <a style=\"color:#8cc8ff\" href=\"/\">/</a></p></main></body></html>";
  return h;
}

// état pour le menu du jeu (STATUS) : une ligne par élément, champs séparés par des tabulations ;
// le jeu traduit les codes d'état dans sa langue
//   S  joueurs  parties  salles  parties depuis le lancement  secondes de service
//   P  nom  mode (a, r, c)  salle  état  secondes de connexion
//      état : wait, free, vs:nom, vote, accept, lobby, loading, racing, out, fin:ms
//   G  type (d : course à deux, m : à plusieurs, l : Computer Link)  salle  circuit  tours  phase  noms (|)
//   R  code  noms (|)
std::string statusText() {
  const double t = now();
  auto clean = [](std::string n) { for (auto &ch : n) if (ch == '\t' || ch == '|' || ch == '\n') ch = '/'; return n; };
  auto stateCode = [&](const Client &c) -> std::string {
    if (c.peer != NONE) { auto p = clients.find(c.peer); return "vs:" + clean(p == clients.end() ? "?" : playerName(p->second)); }
    if (c.inGroup) {
      const std::string &g = c.gstate;
      if (g.rfind("finished:", 0) == 0) return "fin:" + g.substr(9);
      if (g == "out" || g == "vote" || g == "accept" || g == "racing" || g == "lobby") return g;
      return "loading";
    }
    if (!c.watchKey.empty()) return "watch";
    return c.mode == Mode::Room ? "free" : "wait";
  };
  std::vector<const Client *> players;
  for (auto &kv : clients) if (kv.second.joined && !kv.second.closing) players.push_back(&kv.second);
  std::sort(players.begin(), players.end(), [](const Client *a, const Client *b) { return a->id < b->id; });
  std::string gl;
  size_t ng = 0;
  for (auto &kv : groups) {
    const Group &g = kv.second;
    if (g.members.empty()) continue;
    std::string names;
    for (Sock f : g.members) { auto it = clients.find(f); if (it != clients.end()) names += (names.empty() ? "" : "|") + clean(playerName(it->second)); }
    gl += std::string("G\t") + (g.duel ? "d" : "m") + "\t" + (kv.first.rfind("DUEL", 0) == 0 ? "" : clean(kv.first)) + "\t" + std::to_string(g.track) +
          "\t" + std::to_string(g.laps) + "\t" + g.phase + "\t" + names + "\n";
    ng++;
  }
  for (const Client *c : players) {
    if (c->peer == NONE || c->peer < c->fd) continue;
    auto p = clients.find(c->peer);
    if (p == clients.end()) continue;
    gl += "G\tl\t" + clean(c->room) + "\t-1\t0\tracing\t" + clean(playerName(*c)) + "|" + clean(playerName(p->second)) + "\n";
    ng++;
  }
  std::map<std::string, std::string> rooms;
  for (const Client *c : players)
    if (c->mode == Mode::Room) { auto &r = rooms[c->room]; r += (r.empty() ? "" : "|") + clean(playerName(*c)); }
  std::string r = "S\t" + std::to_string(players.size()) + "\t" + std::to_string(ng) + "\t" + std::to_string(rooms.size()) + "\t" +
                  std::to_string(totalGames) + "\t" + std::to_string(long(t - startedAt)) + "\n" + gl;
  for (const Client *c : players)
    r += std::string("P\t") + clean(playerName(*c)) + "\t" + (c->mode == Mode::Auto ? "a" : c->mode == Mode::Room ? "r" : "c") + "\t" +
         clean(c->mode == Mode::Room ? c->room : "") + "\t" + stateCode(*c) + "\t" + std::to_string(long(t - c->since)) + "\n";
  for (auto &kv : rooms) r += "R\t" + clean(kv.first) + "\t" + kv.second + "\n";
  if (r.size() > 60000) r.resize(r.rfind('\n', 60000) + 1);   // une trame (64 Ko au plus)
  return r;
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
  // Windows (double-clic) : journal gardé par défaut dans scr_relay.log à côté du programme (plus de
  // fichier .bat de lancement, que les antivirus prenaient pour un script suspect)
  if (!logFile) logFile = std::fopen((exeDir() + "scr_relay.log").c_str(), "a");
#endif
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
      PollFd p{}; p.fd = kv.first; p.events = short(POLLIN | (kv.second.out.empty() && kv.second.wsOut.empty() ? 0 : POLLOUT));
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
        auto st = strikes.find(ip);
        if (clients.size() >= MAX_CLIENTS || (same >= MAX_PER_IP && !isPrivate(ip))) { closeSock(fd); totalBots++; noteBot(ip, "trop de connexions"); continue; }
        nonBlock(fd);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof on);
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char *>(&on), sizeof on);
        Client c; c.fd = fd; c.id = nextId++; c.ip = ip; c.since = c.lastRx = t;
        c.suspect = st != strikes.end() && st->second.until > t;
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
            auto &dst = c.ws ? c.raw : c.in;
            dst.insert(dst.end(), buf, buf + r); c.lastRx = t; c.rxBytes += uint64_t(r);
            if (dst.size() > MAX_BUFFER) { dead.push_back({fd, "trop de données"}); break; }
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
      if (c.ws) {   // WebSocket : protocole du jeu dans des messages binaires, maintien toutes les 20 s
        if (!c.out.empty()) { wsFrame(c, 0x2, c.out.data(), c.out.size()); c.out.clear(); }
        if (c.closing && !c.wsCloseSent) { wsFrame(c, 0x8, nullptr, 0); c.wsCloseSent = true; }   // après les dernières données
        if (t - c.lastWsPing > 20) { c.lastWsPing = t; wsFrame(c, 0x9, nullptr, 0); }
        if (c.wsOut.size() > MAX_BUFFER) c.overflow = true;
      }
      auto &ob = c.ws ? c.wsOut : c.out;
      while (!ob.empty()) {
        int r = int(send(fd, reinterpret_cast<const char *>(ob.data()), int(ob.size()), SEND_FLAGS));
        if (r > 0) ob.erase(ob.begin(), ob.begin() + r);
        else { if (r < 0 && !wouldBlock(sockErr())) dead.push_back({fd, "envoi impossible"}); break; }
      }
      if (c.closing && !c.closingSince) c.closingSince = t;
      if (c.overflow) dead.push_back({fd, "trop de données en attente"});
      // WebSocket : fermeture propre (réponse du jeu, 3 s au plus) pour que le proxy livre tout avant la coupure
      else if (c.closing && ((c.out.empty() && c.wsOut.empty() && (!c.ws || c.wsCloseGot || t - c.closingSince > 3)) ||
                             t - c.closingSince > 10)) dead.push_back({fd, "terminé"});
      else if (!c.joined && t - c.since > (c.suspect ? SUSPECT_JOIN_TIMEOUT : JOIN_TIMEOUT)) dead.push_back({fd, "pas d'inscription"});
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
      logf("%zu joueur(s) connecté(s), %zu partie(s) en cours, %llu robot(s) refusé(s) depuis le lancement", playerCount(), games / 2,
           static_cast<unsigned long long>(totalBots));
      for (auto it = strikes.begin(); it != strikes.end();)   // oubli des vieux robots
        it = t - it->second.last > BOT_BAN && it->second.until < t ? strikes.erase(it) : std::next(it);
    }
  }
}
