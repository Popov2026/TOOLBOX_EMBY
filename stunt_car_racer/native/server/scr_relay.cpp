// scr_relay.cpp — serveur relais pour le mode « Computer Link » en ligne de Stunt Car Racer.
//
// Deux joueurs se connectent avec le même code de salle ; le serveur les apparie puis transmet
// leurs trames de l'un à l'autre. Aucun port à ouvrir chez les joueurs : seul ce serveur écoute.
// Trafic : environ 300 octets/s par joueur pendant une course.
//
// Compilation (Linux) : g++ -O2 -std=c++17 scr_relay.cpp -o scr_relay
// Lancement           : ./scr_relay [--port 27420]
//
// Protocole (trames : type 1 octet, longueur 2 octets gros-boutiste, données) :
//   client -> serveur : JOIN (10) "code\0version"
//   serveur -> client : INFO (11) "WAIT" | "PAIRED" | "FULL" | "GONE" | "ERR message"
//   ensuite, les trames de chaque joueur sont retransmises telles quelles à l'autre.
#include <arpa/inet.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint8_t NF_JOIN = 10, NF_INFO = 11;
constexpr size_t MAX_CLIENTS = 512, MAX_PER_IP = 16, MAX_BUFFER = 1 << 20;
constexpr double JOIN_TIMEOUT = 20, WAIT_TIMEOUT = 3600, IDLE_TIMEOUT = 90;

double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char *fmt, ...) {
  char ts[32]; time_t t = time(nullptr); strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
  std::printf("%s  ", ts);
  va_list ap; va_start(ap, fmt); std::vprintf(fmt, ap); va_end(ap);
  std::printf("\n"); std::fflush(stdout);
}

struct Client {
  int fd = -1;
  std::string ip, room, version;
  std::vector<uint8_t> in, out;
  int peer = -1;                 // descripteur du partenaire une fois apparié
  bool joined = false, closing = false;
  double since = 0, lastRx = 0;
};

std::map<int, Client> clients;
std::map<std::string, int> waiting;   // salle -> joueur en attente

void frame(Client &c, uint8_t type, const std::string &s) {
  size_t n = std::min<size_t>(s.size(), 0xffff);
  c.out.push_back(type); c.out.push_back(uint8_t(n >> 8)); c.out.push_back(uint8_t(n));
  c.out.insert(c.out.end(), s.begin(), s.begin() + long(n));
}

void drop(int fd, const char *why) {
  auto it = clients.find(fd);
  if (it == clients.end()) return;
  Client &c = it->second;
  if (c.peer >= 0) {
    auto p = clients.find(c.peer);
    if (p != clients.end()) { frame(p->second, NF_INFO, "GONE"); p->second.peer = -1; p->second.closing = true; }
  }
  if (c.joined && c.peer < 0) {
    auto w = waiting.find(c.room);
    if (w != waiting.end() && w->second == fd) waiting.erase(w);
  }
  logf("%-15s salle %-12s déconnecté (%s)", c.ip.c_str(), c.room.c_str(), why);
  close(fd);
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
    } else if (c.peer >= 0) {
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

}  // namespace

int main(int argc, char **argv) {
  int port = 27420;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = std::atoi(argv[++i]);
    else { std::fprintf(stderr, "usage : %s [--port N]\n", argv[0]); return 1; }
  }
  std::signal(SIGPIPE, SIG_IGN);
  int ls = socket(AF_INET, SOCK_STREAM, 0);
  int on = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(uint16_t(port));
  if (bind(ls, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || listen(ls, 64) != 0) {
    std::fprintf(stderr, "impossible d'écouter sur le port %d : %s\n", port, std::strerror(errno));
    return 1;
  }
  fcntl(ls, F_SETFL, fcntl(ls, F_GETFL, 0) | O_NONBLOCK);
  logf("relais Stunt Car Racer : écoute sur le port TCP %d", port);
  for (;;) {
    std::vector<pollfd> pf;
    pf.push_back({ls, POLLIN, 0});
    for (auto &[fd, c] : clients) pf.push_back({fd, short(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
    poll(pf.data(), nfds_t(pf.size()), 500);
    double t = now();
    if (pf[0].revents & POLLIN) {
      for (;;) {
        sockaddr_in ca{}; socklen_t l = sizeof ca;
        int fd = accept(ls, reinterpret_cast<sockaddr *>(&ca), &l);
        if (fd < 0) break;
        std::string ip = inet_ntoa(ca.sin_addr);
        size_t same = 0;
        for (auto &[k, c] : clients) same += c.ip == ip;
        if (clients.size() >= MAX_CLIENTS || same >= MAX_PER_IP) { close(fd); logf("%-15s refusé (trop de connexions)", ip.c_str()); continue; }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
        Client c; c.fd = fd; c.ip = ip; c.since = c.lastRx = t;
        clients[fd] = c;
      }
    }
    std::vector<std::pair<int, const char *>> dead;
    for (size_t i = 1; i < pf.size(); i++) {
      int fd = pf[i].fd;
      auto it = clients.find(fd);
      if (it == clients.end()) continue;
      Client &c = it->second;
      if (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) {
        uint8_t buf[8192];
        for (;;) {
          ssize_t r = recv(fd, buf, sizeof buf, 0);
          if (r > 0) { c.in.insert(c.in.end(), buf, buf + r); c.lastRx = t; if (c.in.size() > MAX_BUFFER) { dead.push_back({fd, "trop de données"}); break; } continue; }
          if (r == 0) { dead.push_back({fd, "fermé"}); break; }
          if (errno != EAGAIN && errno != EWOULDBLOCK) dead.push_back({fd, "erreur"});
          break;
        }
        process(c);
      }
    }
    for (auto &[fd, c] : clients) {
      while (!c.out.empty()) {
        ssize_t r = send(fd, c.out.data(), c.out.size(), 0);
        if (r > 0) c.out.erase(c.out.begin(), c.out.begin() + r);
        else { if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) dead.push_back({fd, "envoi impossible"}); break; }
      }
      if (c.closing && c.out.empty()) dead.push_back({fd, "terminé"});
      else if (!c.joined && t - c.since > JOIN_TIMEOUT) dead.push_back({fd, "pas de code de salle"});
      else if (c.joined && c.peer < 0 && t - c.since > WAIT_TIMEOUT) dead.push_back({fd, "attente trop longue"});
      else if (c.peer >= 0 && t - c.lastRx > IDLE_TIMEOUT) dead.push_back({fd, "silence"});
    }
    for (auto &[fd, why] : dead) drop(fd, why);
  }
}
