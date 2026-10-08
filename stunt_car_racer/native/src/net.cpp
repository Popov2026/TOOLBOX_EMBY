// net.cpp — voir net.hpp
#include "net.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS   // inet_ntoa
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
typedef SOCKET SockRaw;
#define SCR_WOULDBLOCK(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS || (e) == WSAEALREADY)
static int sockErr() { return WSAGetLastError(); }
static void closeRaw(intptr_t s) { closesocket(SOCKET(s)); }
static bool nonBlock(intptr_t s) { u_long on = 1; return ioctlsocket(SOCKET(s), FIONBIO, &on) == 0; }
namespace {
struct WsaInit {
  WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
  ~WsaInit() { WSACleanup(); }
} wsaInit;
}  // namespace
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SockRaw;
#define SCR_WOULDBLOCK(e) ((e) == EWOULDBLOCK || (e) == EAGAIN || (e) == EINPROGRESS || (e) == EALREADY)
static int sockErr() { return errno; }
static void closeRaw(intptr_t s) { ::close(int(s)); }
static bool nonBlock(intptr_t s) { int f = fcntl(int(s), F_GETFL, 0); return fcntl(int(s), F_SETFL, f | O_NONBLOCK) == 0; }
#endif

namespace scr {

uint32_t fnv1a(const void *data, size_t n, uint32_t h) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

static const char ANNOUNCE[] = "SCRLINK1 ";   // annonce UDP : "SCRLINK1 <port tcp> <nom>"

double NetLink::now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

NetLink::NetLink() = default;
NetLink::~NetLink() { close(); }

void NetLink::setIdentity(const std::string &version, uint32_t gameHash, const std::string &name) {
  version_ = version; hash_ = gameHash; name_ = name.substr(0, 40);
}

void NetLink::closeSock(Sock &s) {
  if (s != BAD) { closeRaw(s); s = BAD; }
}

void NetLink::close(const std::string &why) {
  if (tcp_ != BAD && helloOk_) { frame(NF_BYE, why); flush(); }
  closeSock(tcp_); closeSock(listen_); closeSock(udp_);
  mode_ = Mode::Off; state_ = State::Off; connecting_ = false;
  in_.clear(); out_.clear(); rx_.clear(); pendingData_.clear(); dataOut_.clear();
  helloSent_ = helloOk_ = relayPaired_ = false;
  ping_ = -1; peerName_.clear(); info_.clear(); error_.clear();
}

void NetLink::fail(const std::string &why) {
  closeSock(tcp_); closeSock(listen_); closeSock(udp_);
  connecting_ = false; helloOk_ = false;
  state_ = State::Failed; error_ = why;
}

bool NetLink::hostLan(int port) {
  close();
  mode_ = Mode::LanHost; port_ = port; started_ = now();
  listen_ = Sock(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (listen_ == BAD) { fail("création du point d'écoute impossible"); return false; }
  int on = 1;
  setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on), sizeof on);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(uint16_t(port));
  if (bind(listen_, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || ::listen(listen_, 1) != 0) {
    fail("port " + std::to_string(port) + " déjà utilisé"); return false;
  }
  nonBlock(listen_);
  udp_ = Sock(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (udp_ != BAD) {
    setsockopt(udp_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&on), sizeof on);
    nonBlock(udp_);
  }
  state_ = State::Waiting;
  return true;
}

bool NetLink::joinLan() {
  close();
  mode_ = Mode::LanJoin; started_ = now();
  udp_ = Sock(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (udp_ == BAD) { fail("création de la socket UDP impossible"); return false; }
  int on = 1;
  setsockopt(udp_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on), sizeof on);
#ifdef SO_REUSEPORT
  setsockopt(udp_, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char *>(&on), sizeof on);
#endif
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(NET_DISCOVERY_PORT);
  if (bind(udp_, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) { fail("port de détection " + std::to_string(NET_DISCOVERY_PORT) + " occupé"); return false; }
  nonBlock(udp_);
  state_ = State::Waiting;
  return true;
}

static bool splitHostPort(const std::string &s, std::string &host, int &port) {
  size_t c = s.rfind(':');
  if (c != std::string::npos && s.find(':') == c) {
    host = s.substr(0, c);
    port = std::atoi(s.c_str() + c + 1);
    return port > 0 && port < 65536 && !host.empty();
  }
  host = s;
  return !host.empty();
}

bool NetLink::joinDirect(const std::string &hostPort) {
  close();
  mode_ = Mode::Direct; started_ = now();
  std::string host; int port = NET_PORT;
  if (!splitHostPort(hostPort, host, port)) { fail("adresse invalide : " + hostPort); return false; }
  return connectTo(host, port);
}

bool NetLink::joinRelay(const std::string &server, const std::string &room) {
  close();
  mode_ = Mode::Relay; room_ = room; started_ = now();
  for (auto &ch : room_) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  std::string host; int port = NET_PORT;
  if (server.empty()) { fail("aucun serveur relais configuré (scr.ini : relais = adresse:port)"); return false; }
  if (!splitHostPort(server, host, port)) { fail("adresse du relais invalide : " + server); return false; }
  if (room_.empty()) { fail("code de salle vide"); return false; }
  return connectTo(host, port);
}

bool NetLink::connectTo(const std::string &host, int port) {
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) { fail("adresse introuvable : " + host); return false; }
  tcp_ = Sock(socket(res->ai_family, res->ai_socktype, res->ai_protocol));
  if (tcp_ == BAD) { freeaddrinfo(res); fail("création de la connexion impossible"); return false; }
  nonBlock(tcp_);
  int r = ::connect(tcp_, res->ai_addr, socklen_t(res->ai_addrlen));
  freeaddrinfo(res);
  if (r != 0 && !SCR_WOULDBLOCK(sockErr())) { fail("connexion refusée par " + host); return false; }
  connecting_ = true; state_ = State::Connecting; started_ = now();
  info_ = host + ":" + std::to_string(port);
  return true;
}

void NetLink::frame(uint8_t type, const uint8_t *p, size_t n) {
  if (n > 0xffff) n = 0xffff;
  out_.push_back(type); out_.push_back(uint8_t(n >> 8)); out_.push_back(uint8_t(n));
  out_.insert(out_.end(), p, p + n);
}

void NetLink::startSession() {
  state_ = State::Handshake;
  closeSock(listen_); closeSock(udp_);
  int on = 1;
  setsockopt(tcp_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof on);
  // HELLO : "SCR1" \0 version \0 empreinte (4 octets) nom
  std::string h = "SCR1";
  h.push_back('\0'); h += version_; h.push_back('\0');
  for (int k = 3; k >= 0; k--) h.push_back(char((hash_ >> (8 * k)) & 255));
  h += name_;
  frame(NF_HELLO, h);
  helloSent_ = true;
  lastRx_ = now();
}

void NetLink::handle(uint8_t type, const uint8_t *p, size_t n) {
  lastRx_ = now();
  switch (type) {
    case NF_HELLO: {
      std::string s(reinterpret_cast<const char *>(p), n);
      size_t z1 = s.find('\0'), z2 = z1 == std::string::npos ? z1 : s.find('\0', z1 + 1);
      if (s.compare(0, 4, "SCR1") != 0 || z2 == std::string::npos || z2 + 5 > s.size()) { fail("l'autre programme n'est pas Stunt Car Racer"); return; }
      std::string ver = s.substr(z1 + 1, z2 - z1 - 1);
      uint32_t h = 0;
      for (int k = 0; k < 4; k++) h = (h << 8) | uint8_t(s[z2 + 1 + k]);
      peerName_ = s.substr(z2 + 5);
      if (ver != version_) { fail("versions différentes (ici " + version_ + ", en face " + ver + ")"); return; }
      if (h != hash_) { fail("jeu ou réglages différents de l'autre joueur (image disque, scr.ini)"); return; }
      helloOk_ = true; state_ = State::Connected;
      if (!pendingData_.empty()) { frame(NF_DATA, pendingData_.data(), pendingData_.size()); sent_ += pendingData_.size(); pendingData_.clear(); }
      break;
    }
    case NF_DATA:
      rx_.insert(rx_.end(), p, p + n);
      received_ += n;
      break;
    case NF_PING: frame(NF_PONG, p, n); break;
    case NF_PONG:
      if (n == 8) {
        double t; std::memcpy(&t, p, 8);
        ping_ = int((now() - t) * 1000 + 0.5);
      }
      break;
    case NF_BYE: fail("l'autre joueur a quitté la partie"); break;
    case NF_INFO: {   // messages du serveur relais
      std::string s(reinterpret_cast<const char *>(p), n);
      if (s == "PAIRED") { relayPaired_ = true; startSession(); }
      else if (s == "WAIT") info_ = "WAIT";
      else if (s == "FULL") fail("salle " + room_ + " déjà complète (deux joueurs)");
      else if (s == "GONE") fail("l'autre joueur s'est déconnecté");
      else if (s.compare(0, 4, "ERR ") == 0) fail("relais : " + s.substr(4));
      break;
    }
    default: break;
  }
}

void NetLink::flush() {
  while (tcp_ != BAD && !out_.empty()) {
    int r = int(::send(tcp_, reinterpret_cast<const char *>(out_.data()), int(out_.size()), 0));
    if (r > 0) out_.erase(out_.begin(), out_.begin() + r);
    else { if (r < 0 && SCR_WOULDBLOCK(sockErr())) break; fail("connexion perdue"); break; }
  }
}

void NetLink::send(uint8_t b) {
  if (state_ == State::Connected) { dataOut_.push_back(b); sent_++; }   // regroupés en une trame par poll()
  else if (pendingData_.size() < 65536) pendingData_.push_back(b);
}

bool NetLink::recv(uint8_t &b) {
  if (rx_.empty()) return false;
  b = rx_.front(); rx_.pop_front();
  return true;
}

void NetLink::poll() {
  if (mode_ == Mode::Off || state_ == State::Failed) return;
  const double t = now();
  // réseau local, hôte : accepter le joueur, annoncer la partie
  if (mode_ == Mode::LanHost && tcp_ == BAD && listen_ != BAD) {
    sockaddr_in a{}; socklen_t l = sizeof a;
    Sock s = Sock(accept(listen_, reinterpret_cast<sockaddr *>(&a), &l));
    if (s != BAD) { tcp_ = s; nonBlock(tcp_); info_ = inet_ntoa(a.sin_addr); startSession(); }
    else if (udp_ != BAD && t - lastAnnounce_ > 1.0) {
      lastAnnounce_ = t;
      std::string msg = ANNOUNCE + std::to_string(port_) + " " + name_;
      sockaddr_in b{}; b.sin_family = AF_INET; b.sin_port = htons(NET_DISCOVERY_PORT);
      b.sin_addr.s_addr = htonl(INADDR_BROADCAST);
      sendto(udp_, msg.data(), int(msg.size()), 0, reinterpret_cast<sockaddr *>(&b), sizeof b);
      b.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // deux jeux sur le même PC
      sendto(udp_, msg.data(), int(msg.size()), 0, reinterpret_cast<sockaddr *>(&b), sizeof b);
    }
  }
  // réseau local, invité : écouter les annonces
  if (mode_ == Mode::LanJoin && tcp_ == BAD && udp_ != BAD) {
    char buf[256]; sockaddr_in a{}; socklen_t l = sizeof a;
    int r = int(recvfrom(udp_, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr *>(&a), &l));
    if (r > int(sizeof ANNOUNCE - 1) && std::memcmp(buf, ANNOUNCE, sizeof ANNOUNCE - 1) == 0) {
      buf[r] = 0;
      int port = std::atoi(buf + sizeof ANNOUNCE - 1);
      const char *nm = std::strchr(buf + sizeof ANNOUNCE - 1, ' ');
      peerName_ = nm ? nm + 1 : "";
      std::string host = inet_ntoa(a.sin_addr);
      closeSock(udp_);
      if (port > 0) connectTo(host, port);
    }
  }
  // connexion TCP en cours
  if (connecting_ && tcp_ != BAD) {
    fd_set w, e; FD_ZERO(&w); FD_ZERO(&e);
    FD_SET(SockRaw(tcp_), &w); FD_SET(SockRaw(tcp_), &e);
    timeval tv{0, 0};
    int r = select(int(tcp_ + 1), nullptr, &w, &e, &tv);
    if (r > 0) {
      int err = 0; socklen_t l = sizeof err;
      getsockopt(tcp_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err), &l);
      connecting_ = false;
      if (err != 0 || FD_ISSET(SockRaw(tcp_), &e)) { fail("connexion impossible à " + info_); return; }
      if (mode_ == Mode::Relay) {   // demander la salle au relais : JOIN code \0 version
        int on = 1;
        setsockopt(tcp_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof on);
        std::string j = room_; j.push_back('\0'); j += version_;
        frame(NF_JOIN, j);
        state_ = State::Waiting; info_ = "WAIT"; lastRx_ = t;
      } else startSession();
    } else if (t - started_ > 10) { fail("pas de réponse de " + info_); return; }
  }
  if (tcp_ == BAD || connecting_) return;
  // lecture
  for (;;) {
    uint8_t buf[4096];
    int r = int(::recv(tcp_, reinterpret_cast<char *>(buf), sizeof buf, 0));
    if (r > 0) { in_.insert(in_.end(), buf, buf + r); continue; }
    if (r == 0) { fail(helloOk_ ? "l'autre joueur s'est déconnecté" : "connexion fermée par " + info_); return; }
    if (!SCR_WOULDBLOCK(sockErr())) { fail("connexion perdue"); return; }
    break;
  }
  size_t pos = 0;
  while (in_.size() - pos >= 3) {
    size_t n = (size_t(in_[pos + 1]) << 8) | in_[pos + 2];
    if (in_.size() - pos < 3 + n) break;
    handle(in_[pos], in_.data() + pos + 3, n);
    if (state_ == State::Failed) return;
    pos += 3 + n;
  }
  in_.erase(in_.begin(), in_.begin() + long(pos));
  // ping chaque seconde, connexion perdue après 15 s de silence
  if (helloOk_ && t - lastPing_ > 1.0) {
    lastPing_ = t;
    uint8_t p[8]; std::memcpy(p, &t, 8);
    frame(NF_PING, p, 8);
  }
  if (helloOk_ && t - lastRx_ > 15) { fail("plus de nouvelles de l'autre joueur"); return; }
  if (!dataOut_.empty()) { frame(NF_DATA, dataOut_.data(), dataOut_.size()); dataOut_.clear(); }
  flush();
}

std::string NetLink::status() const {
  switch (state_) {
    case State::Off: return "";
    case State::Failed: return "Réseau : " + error_;
    case State::Connected:
      return "Relié à " + (peerName_.empty() ? std::string("l'autre joueur") : peerName_) +
             (ping_ >= 0 ? " - ping " + std::to_string(ping_) + " ms" : "") + " - choisissez « 3. Computer Link »";
    case State::Handshake: return "Réseau : vérification des versions...";
    case State::Connecting: return "Réseau : connexion à " + info_ + "...";
    case State::Waiting:
      if (mode_ == Mode::LanHost) return "Réseau local : partie hébergée, en attente d'un joueur (port " + std::to_string(port_) + ")";
      if (mode_ == Mode::LanJoin) return "Réseau local : recherche d'une partie...";
      if (mode_ == Mode::Relay) return "En ligne : salle " + room_ + ", en attente de l'autre joueur";
      return "Réseau : en attente";
  }
  return "";
}

}  // namespace scr
