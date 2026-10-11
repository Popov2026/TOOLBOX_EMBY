// scr_relay.cpp — serveur relais pour le mode « Computer Link » en ligne de Stunt Car Racer.
//
// Deux joueurs se connectent avec le même code de salle ; le serveur les apparie puis transmet
// leurs trames de l'un à l'autre. Aucun port à ouvrir chez les joueurs : seul ce serveur écoute.
// Trafic : environ 300 octets/s par joueur pendant une course.
//
// Compilation  Linux   : g++ -O2 -std=c++17 scr_relay.cpp -o scr_relay
//              Windows : g++ (MinGW) -O2 -std=c++17 -static scr_relay.cpp -o scr_relay.exe -lws2_32
//                        ou cl /O2 /EHsc /std:c++17 scr_relay.cpp ws2_32.lib
// Lancement           : scr_relay [--port 27420] [--log fichier.txt]
//
// Protocole (trames : type 1 octet, longueur 2 octets gros-boutiste, données) :
//   client -> serveur : JOIN (10) "code\0version"
//   serveur -> client : INFO (11) "WAIT" | "PAIRED" | "FULL" | "GONE" | "ERR message"
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

constexpr uint8_t NF_JOIN = 10, NF_INFO = 11;
constexpr size_t MAX_CLIENTS = 512, MAX_PER_IP = 16, MAX_BUFFER = 1 << 20;
const Sock NONE = Sock(-1);
constexpr double JOIN_TIMEOUT = 20, PAIR_TIMEOUT = 3600, IDLE_TIMEOUT = 90;

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

FILE *logFile = nullptr;   // --log : copie du journal dans un fichier (service Windows / Linux)

void logf(const char *fmt, ...) {
  char ts[32]; time_t t = time(nullptr); strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
  char msg[512];
  va_list ap; va_start(ap, fmt); std::vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
  std::printf("%s  %s\n", ts, msg); std::fflush(stdout);
  if (logFile) { std::fprintf(logFile, "%s  %s\n", ts, msg); std::fflush(logFile); }
}

struct Client {
  Sock fd = Sock(-1);
  std::string ip, room, version;
  std::vector<uint8_t> in, out;
  Sock peer = NONE;              // descripteur du partenaire une fois apparié
  bool joined = false, closing = false;
  double since = 0, lastRx = 0;
};

std::map<Sock, Client> clients;
std::map<std::string, Sock> waiting;   // salle -> joueur en attente

void frame(Client &c, uint8_t type, const std::string &s) {
  size_t n = std::min<size_t>(s.size(), 0xffff);
  c.out.push_back(type); c.out.push_back(uint8_t(n >> 8)); c.out.push_back(uint8_t(n));
  c.out.insert(c.out.end(), s.begin(), s.begin() + long(n));
}

void drop(Sock fd, const char *why) {
  auto it = clients.find(fd);
  if (it == clients.end()) return;
  Client &c = it->second;
  if (c.peer != NONE) {
    auto p = clients.find(c.peer);
    if (p != clients.end()) { frame(p->second, NF_INFO, "GONE"); p->second.peer = NONE; p->second.closing = true; }
  }
  if (c.joined && c.peer == NONE) {
    auto w = waiting.find(c.room);
    if (w != waiting.end() && w->second == fd) waiting.erase(w);
  }
  logf("%-15s salle %-12s déconnecté (%s)", c.ip.c_str(), c.room.c_str(), why);
  closeSock(fd);
  clients.erase(it);
}

bool validRoom(const std::string &r) {
  if (r.empty() || r.size() > 24) return false;
  for (char ch : r) if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_')) return false;
  return true;
}

void onJoin(Client &c, const uint8_t *p, size_t n) {
  std::string s(reinterpret_cast<const char *>(p), n);
  size_t z = s.find('\0');
  c.room = s.substr(0, z);
  c.version = z == std::string::npos ? "" : s.substr(z + 1);
  for (auto &ch : c.room) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  if (!validRoom(c.room)) { frame(c, NF_INFO, "ERR code de salle invalide"); c.closing = true; return; }
  c.joined = true;
  auto w = waiting.find(c.room);
  if (w == waiting.end()) {
    waiting[c.room] = c.fd;
    frame(c, NF_INFO, "WAIT");
    logf("%-15s salle %-12s en attente (version %s)", c.ip.c_str(), c.room.c_str(), c.version.c_str());
    return;
  }
  Client &o = clients[w->second];
  if (o.version != c.version) {
    frame(c, NF_INFO, "ERR versions différentes (" + c.version + " / " + o.version + ")");
    c.closing = true; c.joined = false;
    return;
  }
  waiting.erase(w);
  c.peer = o.fd; o.peer = c.fd;
  frame(o, NF_INFO, "PAIRED 1"); frame(c, NF_INFO, "PAIRED 2");   // 1 : premier arrivé (maître dans le jeu)
  logf("%-15s salle %-12s apparié avec %s", c.ip.c_str(), c.room.c_str(), o.ip.c_str());
}

// trames complètes reçues : JOIN avant l'appariement, retransmission ensuite
void process(Client &c) {
  size_t pos = 0;
  while (c.in.size() - pos >= 3) {
    size_t n = (size_t(c.in[pos + 1]) << 8) | c.in[pos + 2];
    if (c.in.size() - pos < 3 + n) break;
    uint8_t type = c.in[pos];
    if (!c.joined) {
      if (type == NF_JOIN) onJoin(c, c.in.data() + pos + 3, n);
      else { frame(c, NF_INFO, "ERR JOIN attendu"); c.closing = true; }
    } else if (c.peer != NONE) {
      Client &o = clients[c.peer];
      if (o.out.size() < MAX_BUFFER) o.out.insert(o.out.end(), c.in.begin() + long(pos), c.in.begin() + long(pos + 3 + n));
    } else if (type == NF_JOIN) {
      // déjà en attente : ignoré
    }
    pos += 3 + n;
    if (c.closing) break;
  }
  c.in.erase(c.in.begin(), c.in.begin() + long(pos));
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
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--log") && i + 1 < argc) {
      logFile = std::fopen(argv[++i], "a");
      if (!logFile) { std::fprintf(stderr, "impossible d'ouvrir le journal %s\n", argv[i]); return 1; }
    } else { std::fprintf(stderr, "usage : %s [--port N] [--log fichier]\n", argv[0]); return 1; }
  }
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::fprintf(stderr, "Winsock indisponible\n"); return 1; }
  SetConsoleOutputCP(CP_UTF8);   // accents du journal
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
  logf("relais Stunt Car Racer : écoute sur le port TCP %d", port);
  std::string ips = localAddresses();
  if (!ips.empty()) logf("adresse(s) de cette machine : %s", ips.c_str());
  logf("dans scr.ini des joueurs : relais = ADRESSE:%d  (adresse ci-dessus en réseau local, adresse publique", port);
  logf("de la box ou nom de domaine par Internet, avec le port TCP %d redirigé vers cette machine)", port);
  for (;;) {
    std::vector<PollFd> pf;
    PollFd l0{}; l0.fd = ls; l0.events = POLLIN;
    pf.push_back(l0);
    for (auto &kv : clients) {
      PollFd p{}; p.fd = kv.first; p.events = short(POLLIN | (kv.second.out.empty() ? 0 : POLLOUT));
      pf.push_back(p);
    }
    pollSocks(pf.data(), pf.size(), 500);
    double t = now();
    if (pf[0].revents & POLLIN) {
      for (;;) {
        sockaddr_in ca{}; socklen_t l = sizeof ca;
        Sock fd = accept(ls, reinterpret_cast<sockaddr *>(&ca), &l);
        if (fd == NONE) break;
        std::string ip = inet_ntoa(ca.sin_addr);
        size_t same = 0;
        for (auto &kv : clients) same += kv.second.ip == ip;
        if (clients.size() >= MAX_CLIENTS || same >= MAX_PER_IP) { closeSock(fd); logf("%-15s refusé (trop de connexions)", ip.c_str()); continue; }
        nonBlock(fd);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof on);
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char *>(&on), sizeof on);
        Client c; c.fd = fd; c.ip = ip; c.since = c.lastRx = t;
        clients[fd] = c;
        logf("%-15s connecté", ip.c_str());
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
          if (r > 0) { c.in.insert(c.in.end(), buf, buf + r); c.lastRx = t; if (c.in.size() > MAX_BUFFER) { dead.push_back({fd, "trop de données"}); break; } continue; }
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
      else if (!c.joined && t - c.since > JOIN_TIMEOUT) dead.push_back({fd, "pas de code de salle"});
      else if (c.joined && c.peer == NONE && t - c.since > PAIR_TIMEOUT) dead.push_back({fd, "attente trop longue"});
      else if (c.peer != NONE && t - c.lastRx > IDLE_TIMEOUT) dead.push_back({fd, "silence"});
    }
    for (auto &d : dead) drop(d.first, d.second);
  }
}
