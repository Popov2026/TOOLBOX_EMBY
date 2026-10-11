// scr_relay.cpp — serveur relais pour le mode « Computer Link » en ligne de Stunt Car Racer.
//
// Les jeux se connectent au serveur, qui met deux joueurs en relation puis transmet leurs trames de
// l'un à l'autre. Aucun port à ouvrir chez les joueurs : seul ce serveur écoute.
// Deux façons de se trouver :
//   - automatique : le serveur relie le joueur au premier autre joueur en attente ;
//   - salle privée : les joueurs qui tapent le même code voient la liste des présents et
//     choisissent leur adversaire.
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
//   client -> serveur : JOIN2 (12) "AUTO|SALLE \0 code \0 version \0 nom"
//                       PICK (14) "numéro"  (salle privée : choix de l'adversaire)
//                       PROBE (15) ""       (test du serveur par le jeu : réponse INFO "SERVER joueurs parties")
//                       JOIN (10) "code\0version"  (ancienne version : appariement par code)
//   serveur -> client : INFO (11) "WAIT" | "PAIRED 1" (maître) | "PAIRED 2" | "GONE" | "BUSY" | "ERR message"
//                       LIST (13) "numéro\tnom\n..." (salle privée : joueurs présents et libres)
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

namespace {

constexpr uint8_t NF_JOIN = 10, NF_INFO = 11, NF_JOIN2 = 12, NF_LIST = 13, NF_PICK = 14, NF_PROBE = 15, NF_PARAMS = 16;
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
  double since = 0, lastRx = 0, pairedAt = 0;
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
  size_t n = std::min<size_t>(s.size(), 0xffff);
  c.out.push_back(type); c.out.push_back(uint8_t(n >> 8)); c.out.push_back(uint8_t(n));
  c.out.insert(c.out.end(), s.begin(), s.begin() + long(n));
}

std::string who(const Client &c) { return c.name.empty() ? c.ip : c.name + " (" + c.ip + ")"; }

bool free_(const Client &c) { return c.joined && !c.closing && c.peer == NONE; }

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
  closeSock(fd);
  clients.erase(it);
  dirty = true;
}

bool validRoom(const std::string &r) {
  if (r.empty() || r.size() > 24) return false;
  for (char ch : r) if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_')) return false;
  return true;
}

std::string cleanName(const std::string &s) {
  std::string r;
  for (unsigned char ch : s) if (ch >= 32 && ch != 127) r += char(ch);
  while (width(r) > 24) r.pop_back();
  while (!r.empty() && (static_cast<unsigned char>(r.back()) & 0xc0) == 0xc0) r.pop_back();   // UTF-8 coupé
  return r;
}

std::vector<std::string> fields(const uint8_t *p, size_t n) {
  std::vector<std::string> f(1);
  for (size_t i = 0; i < n; i++) { if (p[i] == 0) f.emplace_back(); else f.back() += char(p[i]); }
  return f;
}

void onJoin(Client &c, Mode mode, const std::string &room, const std::string &version, const std::string &name) {
  c.mode = mode; c.version = version; c.name = cleanName(name);
  c.room = room;
  for (auto &ch : c.room) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  if (mode != Mode::Auto && !validRoom(c.room)) { frame(c, NF_INFO, "ERR code de salle invalide"); c.closing = true; return; }
  c.joined = true; dirty = true;
  frame(c, NF_PARAMS, paramsText());   // réglages imposés, appliqués par le jeu pendant la partie
  frame(c, NF_INFO, "WAIT");
  if (mode == Mode::Room) { logf("%s entre dans la salle %s", who(c).c_str(), c.room.c_str()); return; }
  // automatique (ou ancien appariement par code) : le premier joueur libre compatible, arrivé avant
  Client *best = nullptr;
  for (auto &kv : clients) {
    Client &o = kv.second;
    if (&o == &c || !free_(o) || o.mode != mode || o.version != c.version) continue;
    if (mode == Mode::Code && o.room != c.room) continue;
    if (!best || o.since < best->since) best = &o;
  }
  if (best) pair(*best, c);
  else logf("%s attend un adversaire (%s)", who(c).c_str(), mode == Mode::Auto ? "automatique" : ("code " + c.room).c_str());
}

void onPick(Client &c, const std::string &s) {
  uint32_t id = uint32_t(std::strtoul(s.c_str(), nullptr, 10));
  if (c.mode != Mode::Room || !free_(c)) return;
  for (auto &kv : clients) {
    Client &o = kv.second;
    if (o.id == id && &o != &c && o.mode == Mode::Room && free_(o) && o.room == c.room && o.version == c.version) {
      pair(c, o);   // celui qui choisit mène la partie
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
      if (type == NF_JOIN2 && f.size() >= 3)
        onJoin(c, f[0] == "SALLE" ? Mode::Room : Mode::Auto, f[1], f[2], f.size() > 3 ? f[3] : "");
      else if (type == NF_JOIN) onJoin(c, Mode::Code, f[0], f.size() > 1 ? f[1] : "", "");
      else if (type == NF_PROBE) {   // le jeu vérifie que le serveur répond
        size_t players = 0, games = 0;
        for (auto &kv : clients) { players += kv.second.joined; games += kv.second.peer != NONE; }
        frame(c, NF_INFO, "SERVER " + std::to_string(players) + " " + std::to_string(games / 2) + ";" + paramsText());
        c.probe = c.closing = true;
      }
      else { frame(c, NF_INFO, "ERR inscription attendue"); c.closing = true; }
    } else if (c.peer != NONE) {
      Client &o = clients[c.peer];
      if (o.out.size() < MAX_BUFFER) o.out.insert(o.out.end(), c.in.begin() + long(pos), c.in.begin() + long(pos + 3 + n));
    } else if (type == NF_PICK) onPick(c, std::string(reinterpret_cast<const char *>(p), n));
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
  return c.mode == Mode::Room ? "dans la salle, libre" : "attend un adversaire";
}

void draw(double t) {
  std::string o = "\x1b[H\x1b[2J";
  size_t games = 0;
  for (auto &kv : clients) games += kv.second.peer != NONE;
  char b[512];
  std::snprintf(b, sizeof b, "\x1b[1mSERVEUR RELAIS STUNT CAR RACER\x1b[0m   port TCP %d   en service depuis %s   %s\n", listenPort,
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
  SetConsoleTitleA("Serveur relais Stunt Car Racer");
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
  logf("relais Stunt Car Racer : écoute sur le port TCP %d", port);
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
      if (c.closing && c.out.empty()) dead.push_back({fd, "terminé"});
      else if (!c.joined && t - c.since > JOIN_TIMEOUT) dead.push_back({fd, "pas d'inscription"});
      else if (c.joined && c.peer == NONE && t - c.since > PAIR_TIMEOUT) dead.push_back({fd, "attente trop longue"});
      else if (c.peer != NONE && t - c.lastRx > IDLE_TIMEOUT) dead.push_back({fd, "silence"});
    }
    for (auto &d : dead) drop(d.first, d.second);
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
