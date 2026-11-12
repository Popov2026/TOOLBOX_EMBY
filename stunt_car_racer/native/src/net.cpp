// net.cpp — voir net.hpp
#include "net.hpp"
#include "lang.hpp"
#include "wstunnel.hpp"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS   // inet_ntoa
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
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
#include <ifaddrs.h>
#include <net/if.h>
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

#ifdef __EMSCRIPTEN__
// navigateur : Emscripten fait de chaque connexion TCP une WebSocket vers hôte:port ; wss:// pour une
// adresse wss://hôte[:port] (port 443 par défaut), ws:// sinon ; aucun sous-protocole demandé (le
// serveur relais n'en choisit pas)
static void webScheme(bool secure) {
  EM_ASM({
    Module['websocket'] = Module['websocket'] || {};
    Module['websocket']['url'] = $0 ? 'wss://' : 'ws://';
    Module['websocket']['subprotocol'] = 'null';
  }, secure ? 1 : 0);
}
static bool webHostPort(const std::string &a, std::string &host, int &port, bool &secure) {
  std::string s = a;
  secure = s.compare(0, 6, "wss://") == 0;
  if (secure) s = s.substr(6); else if (s.compare(0, 5, "ws://") == 0) s = s.substr(5);
  s = s.substr(0, s.find('/'));
  port = secure ? 443 : isWsUrl(a) ? 80 : NET_PORT;
  size_t c = s.rfind(':');
  if (c != std::string::npos) { port = std::atoi(s.c_str() + c + 1); s.resize(c); }
  host = s;
  return !host.empty() && port > 0 && port < 65536;
}
#endif

uint32_t fnv1a(const void *data, size_t n, uint32_t h) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

static const char ANNOUNCE[] = "SCRLINK1 ";   // annonce UDP (--host) : "SCRLINK1 <port tcp> <nom>"
static const char ANNOUNCE2[] = "SCRLINK2 ";  // annonce UDP du salon : "SCRLINK2 <id> <port tcp> <nom>"

double NetLink::now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

NetLink::NetLink() = default;
NetLink::~NetLink() { close(); }

void NetLink::setIdentity(const std::string &version, uint32_t gameHash, const std::string &name, const std::string &tuning) {
  version_ = version; hash_ = gameHash; name_ = name.substr(0, 40); tuning_ = tuning;
}

void NetLink::closeSock(Sock &s) {
  if (s != BAD) { closeRaw(s); s = BAD; }
}

void NetLink::close(const std::string &why) {
  if (tcp_ != BAD && helloOk_) { frame(NF_BYE, why); flush(); }
  closeSock(tcp_); closeSock(listen_); closeSock(udp_);
  mode_ = Mode::Off; state_ = State::Off; connecting_ = false; shown_.clear();
  in_.clear(); out_.clear(); rx_.clear(); dataOut_.clear(); peers_.clear();
  helloSent_ = helloOk_ = relayPaired_ = master_ = waitedFirst_ = false;
  ping_ = -1; peerName_.clear(); peerTuning_.clear(); serverParams_.clear(); chat_.clear();
  groupIn_ = false; groupMembers_.clear(); groupEvents_.clear(); ghosts_.clear(); groupPhase_.clear(); info_.clear(); error_.clear();
}

void NetLink::fail(const std::string &why) {
  closeSock(tcp_); closeSock(listen_); closeSock(udp_);
  connecting_ = false; helloOk_ = false;
  state_ = State::Failed; error_ = why;
}

bool NetLink::openListener(int port) {
  int on = 1;
  for (int k = 0; k < 10; k++) {   // port occupé (autre jeu sur ce PC) : port suivant
    listen_ = Sock(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listen_ == BAD) return false;
#ifndef _WIN32
    setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on), sizeof on);
#endif
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(uint16_t(port + k));
    if (bind(listen_, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0 && ::listen(listen_, 2) == 0) {
      nonBlock(listen_); port_ = port + k; return true;
    }
    closeSock(listen_);
  }
  return false;
}

bool NetLink::openDiscovery(bool broadcast) {
  udp_ = Sock(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (udp_ == BAD) return false;
  int on = 1;
  setsockopt(udp_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on), sizeof on);
#ifdef SO_REUSEPORT
  setsockopt(udp_, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char *>(&on), sizeof on);
#endif
  if (broadcast) setsockopt(udp_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&on), sizeof on);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(NET_DISCOVERY_PORT);
  if (bind(udp_, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) { closeSock(udp_); return false; }
  nonBlock(udp_);
  return true;
}

bool NetLink::lobby() {
  close();
  mode_ = Mode::Lobby; started_ = now();
  id_ = uint32_t(fnv1a(&started_, sizeof started_, uint32_t(reinterpret_cast<uintptr_t>(this)))) | 1u;
  if (!openListener(NET_PORT)) { fail(L("aucun port libre pour recevoir un adversaire", "no free port to receive an opponent")); return false; }
  if (!openDiscovery(true)) { fail(L("port de détection ", "discovery port ") + std::to_string(NET_DISCOVERY_PORT) + L(" occupé", " in use")); return false; }
  state_ = State::Waiting;
  return true;
}

bool NetLink::connectPeer(const Peer &p) {
  if (relayRoom() && state_ == State::Waiting) {   // salle privée : le serveur relie les deux joueurs
    peerName_ = p.name;
    frame(NF_PICK, std::to_string(p.id));
    flush();
    return true;
  }
  if (mode_ != Mode::Lobby || tcp_ != BAD) return false;
  peerName_ = p.name;
  master_ = true;                       // celui qui choisit l'adversaire mène la partie
  return connectTo(p.ip, p.port);
}

// adresses et diffusions dirigées de chaque carte réseau active : la diffusion générale
// 255.255.255.255 ne part que sur une seule carte sous Windows (souvent une carte virtuelle :
// Hyper-V, VirtualBox, VPN...), elle n'atteint alors pas l'autre PC
void NetLink::refreshNets(double t) {
  if (t - netsAt_ < 5.0) return;
  netsAt_ = t;
  nets_.clear();
#ifdef _WIN32
  SOCKET s = udp_ != BAD ? SOCKET(udp_) : socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return;
  INTERFACE_INFO info[32]; DWORD got = 0;
  if (WSAIoctl(s, SIO_GET_INTERFACE_LIST, nullptr, 0, info, sizeof info, &got, nullptr, nullptr) == 0) {
    for (DWORD k = 0; k < got / sizeof(INTERFACE_INFO); k++) {
      const INTERFACE_INFO &f = info[k];
      if (!(f.iiFlags & IFF_UP) || (f.iiFlags & IFF_LOOPBACK) || f.iiAddress.Address.sa_family != AF_INET) continue;
      uint32_t a = ntohl(f.iiAddress.AddressIn.sin_addr.s_addr), m = ntohl(f.iiNetmask.AddressIn.sin_addr.s_addr);
      if (a) nets_.push_back({a, (a & m) | ~m});
    }
  }
  if (udp_ == BAD) closesocket(s);
#else
  ifaddrs *list = nullptr;
  if (getifaddrs(&list) != 0) return;
  for (ifaddrs *f = list; f; f = f->ifa_next) {
    if (!f->ifa_addr || f->ifa_addr->sa_family != AF_INET || !(f->ifa_flags & IFF_UP) || (f->ifa_flags & IFF_LOOPBACK)) continue;
    uint32_t a = ntohl(reinterpret_cast<sockaddr_in *>(f->ifa_addr)->sin_addr.s_addr);
    uint32_t m = f->ifa_netmask ? ntohl(reinterpret_cast<sockaddr_in *>(f->ifa_netmask)->sin_addr.s_addr) : 0xffffff00u;
    if (a) nets_.push_back({a, (a & m) | ~m});
  }
  freeifaddrs(list);
#endif
}

std::vector<std::string> NetLink::localAddresses() {
  refreshNets(now());
  std::vector<std::string> r;
  for (auto &n : nets_) {
    in_addr ia; ia.s_addr = htonl(n.addr);
    r.push_back(inet_ntoa(ia));
  }
  return r;
}

void NetLink::sendDiscovery(const std::string &msg) {
  refreshNets(now());
  sockaddr_in b{}; b.sin_family = AF_INET; b.sin_port = htons(NET_DISCOVERY_PORT);
  auto to = [&](uint32_t a) {
    b.sin_addr.s_addr = htonl(a);
    sendto(udp_, msg.data(), int(msg.size()), 0, reinterpret_cast<sockaddr *>(&b), sizeof b);
  };
  to(INADDR_BROADCAST);
  for (auto &n : nets_) if (n.bcast != INADDR_BROADCAST) to(n.bcast);
  to(INADDR_LOOPBACK);   // deux jeux sur le même PC
}

void NetLink::announce(double t) {
  if (udp_ == BAD || t - lastAnnounce_ < 1.0) return;
  lastAnnounce_ = t;
  std::string msg = mode_ == Mode::Lobby ? ANNOUNCE2 + std::to_string(id_) + " " + std::to_string(port_) + " " + name_
                                         : ANNOUNCE + std::to_string(port_) + " " + name_;
  sendDiscovery(msg);
}

void NetLink::readAnnounces(double t) {
  for (int k = 0; k < 32 && udp_ != BAD; k++) {
    char buf[256]; sockaddr_in a{}; socklen_t l = sizeof a;
    int r = int(recvfrom(udp_, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr *>(&a), &l));
    if (r <= 0) break;
    buf[r] = 0;
    if (std::strncmp(buf, ANNOUNCE2, sizeof ANNOUNCE2 - 1) != 0) continue;
    char *q = buf + sizeof ANNOUNCE2 - 1, *e = nullptr;
    uint32_t id = uint32_t(std::strtoul(q, &e, 10));
    if (!e || id == id_) continue;      // notre propre annonce
    int port = int(std::strtol(e, &e, 10));
    std::string name = (e && *e == ' ') ? e + 1 : "";
    std::string ip = inet_ntoa(a.sin_addr);
    Peer *pp = nullptr;
    for (auto &p : peers_)
      if (p.id == id) { p.name = name; p.port = port; p.seen = t; if (p.ip == "127.0.0.1" || ip != "127.0.0.1") p.ip = ip; pp = &p; }
    if (!pp && port > 0) { peers_.push_back({id, name, ip, port, t}); pp = &peers_.back(); }
    // réponse directe à l'expéditeur : si ses annonces nous parviennent mais pas les nôtres (pare-feu,
    // carte réseau), il nous voit quand même (le pare-feu de Windows laisse passer la réponse à une diffusion)
    if (pp && t - pp->replied > 1.0) {
      pp->replied = t;
      std::string msg = ANNOUNCE2 + std::to_string(id_) + " " + std::to_string(port_) + " " + name_;
      sendto(udp_, msg.data(), int(msg.size()), 0, reinterpret_cast<sockaddr *>(&a), sizeof a);
    }
  }
  // une partie qui ne s'annonce plus depuis 3,5 s a disparu
  for (size_t i = 0; i < peers_.size();)
    if (t - peers_[i].seen > 3.5) peers_.erase(peers_.begin() + long(i)); else i++;
}

bool NetLink::hostLan(int port) {
  close();
  mode_ = Mode::LanHost; port_ = port; started_ = now();
  listen_ = Sock(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (listen_ == BAD) { fail(L("création du point d'écoute impossible", "cannot create the listening socket")); return false; }
  int on = 1;
  setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on), sizeof on);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(uint16_t(port));
  if (bind(listen_, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || ::listen(listen_, 1) != 0) {
    fail("port " + std::to_string(port) + L(" déjà utilisé", " already in use")); return false;
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
  if (udp_ == BAD) { fail(L("création de la socket UDP impossible", "cannot create the UDP socket")); return false; }
  int on = 1;
  setsockopt(udp_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on), sizeof on);
#ifdef SO_REUSEPORT
  setsockopt(udp_, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char *>(&on), sizeof on);
#endif
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(NET_DISCOVERY_PORT);
  if (bind(udp_, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) { fail(L("port de détection ", "discovery port ") + std::to_string(NET_DISCOVERY_PORT) + L(" occupé", " in use")); return false; }
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
  if (!splitHostPort(hostPort, host, port)) { fail(L("adresse invalide : ", "invalid address: ") + hostPort); return false; }
  master_ = true;
  return connectTo(host, port);
}

bool NetLink::joinRelay(const std::string &server, const std::string &room, const std::string &display) {
  close();
  shown_ = display;
  mode_ = Mode::Relay; room_ = room; started_ = now();
  for (auto &ch : room_) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  std::string host; int port = NET_PORT;
  if (server.empty()) { fail(L("aucun serveur relais configuré (scr.ini : relais = adresse:port)", "no relay server set (scr.ini: relais = address:port)")); return false; }
#ifdef __EMSCRIPTEN__
  {   // navigateur : la connexion est une WebSocket (wss:// pour une adresse wss://)
    bool secure = false;
    if (!webHostPort(server, host, port, secure)) { fail(L("adresse du relais invalide : ", "invalid relay address: ") + (shown_.empty() ? server : shown_)); return false; }
    if (shown_.empty() && isWsUrl(server)) shown_ = server;
    webScheme(secure);
    return connectTo(host, port);
  }
#endif
  if (isWsUrl(server)) {   // par un reverse proxy web : tunnel local vers la WebSocket
    std::string err, local = openWsTunnel(server, err);
    if (local.empty()) { fail(err); return false; }
    if (shown_.empty()) shown_ = server;
    splitHostPort(local, host, port);
    return connectTo(host, port);
  }
  if (!splitHostPort(server, host, port)) { fail(L("adresse du relais invalide : ", "invalid relay address: ") + (shown_.empty() ? server : shown_)); return false; }
  return connectTo(host, port);
}

bool NetLink::connectTo(const std::string &host, int port) {
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  const std::string nm = shown_.empty() ? host : shown_;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) { fail(L("adresse introuvable : ", "address not found: ") + nm); return false; }
  tcp_ = Sock(socket(res->ai_family, res->ai_socktype, res->ai_protocol));
  if (tcp_ == BAD) { freeaddrinfo(res); fail(L("création de la connexion impossible", "cannot create the connection")); return false; }
  nonBlock(tcp_);
  int r = ::connect(tcp_, res->ai_addr, socklen_t(res->ai_addrlen));
  freeaddrinfo(res);
  if (r != 0 && !SCR_WOULDBLOCK(sockErr())) { fail(L("connexion refusée par ", "connection refused by ") + nm); return false; }
  connecting_ = true; state_ = State::Connecting; started_ = now();
  info_ = shown_.empty() ? host + ":" + std::to_string(port) : shown_;
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
  h.push_back('\0'); h += tuning_;   // réglages de physique (scr.ini)
  frame(NF_HELLO, h);
  helloSent_ = true;
  lastRx_ = now();
}

// texte venu du réseau : sans caractères de contrôle, au plus maxw caractères (UTF-8)
static std::string cleanText(const std::string &s, size_t maxw) {
  std::string r;
  size_t count = 0;
  for (unsigned char ch : s) {
    if (ch < 32 || ch == 127) continue;
    if ((ch & 0xc0) != 0x80 && count++ >= maxw) break;   // début de caractère au-delà de la limite
    r += char(ch);
  }
  return r;
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
      peerTuning_.clear();
      size_t z3 = peerName_.find('\0');
      if (z3 != std::string::npos) { peerTuning_ = peerName_.substr(z3 + 1, 200); peerName_ = peerName_.substr(0, z3); }
      peerName_ = cleanText(peerName_, 24);
      if (ver != version_) { fail(L("versions différentes (ici ", "different versions (here ") + version_ + L(", en face ", ", other side ") + ver + ")"); return; }
      if (h != hash_) { fail(L("image disque du jeu différente de celle de l'autre joueur (même fichier .st requis)", "game disk image differs from the other player's (same .st file required)")); return; }
      helloOk_ = true; state_ = State::Connected;
      break;
    }
    case NF_DATA:
      if (rx_.size() > (1u << 20)) { fail(L("trop de données reçues de l'autre joueur", "too much data received from the other player")); return; }
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
    case NF_BYE: fail(L("l'autre joueur a quitté la partie", "the other player left the game")); break;
    case NF_INFO: {   // messages du serveur relais
      std::string s(reinterpret_cast<const char *>(p), n);
      if (s.compare(0, 6, "PAIRED") == 0) {   // « PAIRED 1 » : premier arrivé = maître
        relayPaired_ = true;
        master_ = s.size() > 7 ? s[7] == '1' : waitedFirst_;
        startSession();
      }
      else if (s == "WAIT") { info_ = "WAIT"; waitedFirst_ = true; }
      else if (s.compare(0, 9, "MISMATCH ") == 0)
        localNote(L("Un joueur attend avec la version ", "A player is waiting with version ") + cleanText(s.substr(9), 12) + L(" (vous : ", " (you: ") + version_ +
                  L(") : les deux jeux et le serveur doivent avoir la même version", "): both games and the server must have the same version"));
      else if (s == "BUSY") peerName_.clear();   // joueur choisi déjà parti : la liste se met à jour
      else if (s == "FULL") fail(L("salle ", "room ") + room_ + L(" déjà complète (deux joueurs)", " already full (two players)"));
      else if (s == "GONE") fail(L("l'autre joueur s'est déconnecté", "the other player disconnected"));
      else if (s.compare(0, 4, "ERR ") == 0) fail(L("relais : ", "relay: ") + (s.substr(4) == "code de salle invalide" ? std::string(L("code de salle invalide", "invalid room code")) : s.substr(4)));
      break;
    }
    case NF_PARAMS: serverParams_.assign(reinterpret_cast<const char *>(p), n); break;
    case NF_GSTATE: {   // état du groupe (course à plusieurs)
      std::string s(reinterpret_cast<const char *>(p), n);
      std::stringstream ss(s); std::string line;
      groupMembers_.clear(); groupDuel_ = false;
      while (std::getline(ss, line)) {
        std::stringstream ls(line); std::string k; ls >> k;
        if (k == "me") ls >> groupMe_;
        else if (k == "host") ls >> groupHost_;
        else if (k == "phase") ls >> groupPhase_;
        else if (k == "track") { std::string l; ls >> groupTrack_ >> l >> groupLaps_; }
        else if (k == "duel") groupDuel_ = true;
        else if (k == "m") {
          GroupMember gm; ls >> gm.id >> gm.state; std::getline(ls, gm.name);
          if (!gm.name.empty() && gm.name[0] == ' ') gm.name.erase(0, 1);
          gm.name = cleanText(gm.name, 24); gm.state = gm.state.substr(0, 24);
          if (groupMembers_.size() < 16) groupMembers_.push_back(gm);
        }
      }
      groupIn_ = false;
      for (auto &gm : groupMembers_) groupIn_ = groupIn_ || gm.id == groupMe_;
      // fantômes des joueurs partis : oubliés
      for (auto it = ghosts_.begin(); it != ghosts_.end();) {
        bool still = false;
        for (auto &gm : groupMembers_) still = still || gm.id == it->first;
        it = still ? std::next(it) : ghosts_.erase(it);
      }
      break;
    }
    case NF_HORN: {   // klaxon d'un autre joueur
      uint32_t id = n >= 4 ? (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3] : 0;
      if (horns_.size() < 8) horns_.push_back(id);
      break;
    }
    case NF_GEVENT:
      groupEvents_.emplace_back(reinterpret_cast<const char *>(p), n);
      if (groupEvents_.back().rfind("START", 0) == 0 || groupEvents_.back().rfind("WATCH ", 0) == 0) ghosts_.clear();
      if (groupEvents_.back() == "WATCHEND" && !groupIn_) { groupMembers_.clear(); ghosts_.clear(); groupPhase_.clear(); }   // fin du spectacle
      break;
    case NF_GPOS:
      if (n >= 4 + 30) {
        uint32_t id = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        GhostSample g; g.t = now();
        for (int k = 0; k < 7; k++) {
          uint32_t u = (uint32_t(p[4 + 4 * k]) << 24) | (uint32_t(p[5 + 4 * k]) << 16) | (uint32_t(p[6 + 4 * k]) << 8) | p[7 + 4 * k];
          std::memcpy(&g.v[k], &u, 4);
        }
        g.lap = p[32]; g.flags = p[33];
        bool sane = true;   // position venue d'un autre joueur : valeurs finies et raisonnables seulement
        for (int k = 0; k < 7; k++) sane = sane && std::isfinite(g.v[k]) && std::fabs(g.v[k]) < 1e7f;
        if (!sane || (ghosts_.size() >= 16 && !ghosts_.count(id))) break;
        auto &q = ghosts_[id];
        q.push_back(g);
        while (q.size() > 40) q.pop_front();
      }
      break;
    case NF_CHAT: {   // « nom \t texte » (salle, par le serveur) ou « texte » (adversaire)
      std::string s(reinterpret_cast<const char *>(p), n), from = peerName_;
      size_t tab = s.find('\t');
      if (tab != std::string::npos) { from = s.substr(0, tab); s = s.substr(tab + 1); }
      from = cleanText(from, 24);
      if (from.empty()) from = L("Adversaire", "Opponent");
      chat_.push_back({from, cleanText(s, 120), now(), false});
      if (std::getenv("SCR_HORNLOG")) std::fprintf(stderr, "message reçu de %s : %s\n", from.c_str(), s.c_str());
      while (chat_.size() > 30) chat_.pop_front();
      break;
    }
    case NF_LIST: {   // salle privée : joueurs présents et libres, « numéro \t nom \n »
      std::string s(reinterpret_cast<const char *>(p), n);
      peers_.clear();
      size_t a = 0;
      while (a < s.size()) {
        size_t e = s.find('\n', a); if (e == std::string::npos) e = s.size();
        std::string line = s.substr(a, e - a);
        size_t tab = line.find('\t');
        if (tab != std::string::npos && peers_.size() < 64) peers_.push_back({uint32_t(std::strtoul(line.c_str(), nullptr, 10)), cleanText(line.substr(tab + 1), 24), "", 0, now()});
        a = e + 1;
      }
      break;
    }
    default: break;
  }
}

void NetLink::flush() {
  while (tcp_ != BAD && !out_.empty()) {
    int r = int(::send(tcp_, reinterpret_cast<const char *>(out_.data()), int(out_.size()), 0));
    if (r > 0) out_.erase(out_.begin(), out_.begin() + r);
    else { if (r < 0 && SCR_WOULDBLOCK(sockErr())) break; fail(L("connexion perdue", "connection lost")); break; }
  }
}

void NetLink::groupCommand(const std::string &cmd) {
  if (mode_ != Mode::Relay || tcp_ == BAD) return;
  frame(NF_GCMD, cmd);
  flush();
  if (cmd == "LEAVE") { groupIn_ = false; groupMembers_.clear(); ghosts_.clear(); groupEvents_.clear(); groupPhase_.clear(); }
}

bool NetLink::takeGroupEvent(std::string &ev) {
  if (groupEvents_.empty()) return false;
  ev = groupEvents_.front(); groupEvents_.pop_front();
  return true;
}

void NetLink::sendGhost(const float v[7], uint8_t lap, uint8_t flags) {
  if (!groupIn_ || tcp_ == BAD) return;
  uint8_t b[30];
  for (int k = 0; k < 7; k++) {
    uint32_t u; std::memcpy(&u, &v[k], 4);
    b[4 * k] = uint8_t(u >> 24); b[4 * k + 1] = uint8_t(u >> 16); b[4 * k + 2] = uint8_t(u >> 8); b[4 * k + 3] = uint8_t(u);
  }
  b[28] = lap; b[29] = flags;
  frame(NF_GPOS, b, 30);
}

void NetLink::localNote(const std::string &text) {
  chat_.push_back({L("Jeu", "Game"), text.substr(0, 120), now(), false});
  while (chat_.size() > 30) chat_.pop_front();
}

void NetLink::sendHorn() {
  if (!canHorn()) return;
  frame(NF_HORN, std::string());
  flush();
}

bool NetLink::takeHorn(uint32_t &from) {
  if (horns_.empty()) return false;
  from = horns_.front(); horns_.pop_front();
  return true;
}

void NetLink::sendChat(const std::string &text) {
  if (!canChat() || text.empty()) return;
  std::string t = text.substr(0, 120);
  frame(NF_CHAT, t);
  flush();
  chat_.push_back({name_, t, now(), true});
  while (chat_.size() > 30) chat_.pop_front();
}

void NetLink::send(uint8_t b) {
  // câble « débranché » tant que la connexion n'est pas prête : l'octet est perdu, comme sur l'Atari
  if (state_ == State::Connected) { dataOut_.push_back(b); sent_++; }   // regroupés en une trame par poll()
}

bool NetLink::recv(uint8_t &b) {
  if (rx_.empty()) return false;
  b = rx_.front(); rx_.pop_front();
  return true;
}

void NetLink::poll() {
  if (mode_ == Mode::Off || state_ == State::Failed) return;
  const double t = now();
  // salon du réseau local : s'annoncer, lister les autres, accepter le joueur qui nous choisit
  if (mode_ == Mode::Lobby && tcp_ == BAD) {
    announce(t);
    readAnnounces(t);
    if (listen_ != BAD) {
      sockaddr_in a{}; socklen_t l = sizeof a;
      Sock s = Sock(accept(listen_, reinterpret_cast<sockaddr *>(&a), &l));
      if (s != BAD) { tcp_ = s; nonBlock(tcp_); info_ = inet_ntoa(a.sin_addr); master_ = false; startSession(); }
    }
  }
  // réseau local, hôte : accepter le joueur, annoncer la partie
  if (mode_ == Mode::LanHost && tcp_ == BAD && listen_ != BAD) {
    sockaddr_in a{}; socklen_t l = sizeof a;
    Sock s = Sock(accept(listen_, reinterpret_cast<sockaddr *>(&a), &l));
    if (s != BAD) { tcp_ = s; nonBlock(tcp_); info_ = inet_ntoa(a.sin_addr); startSession(); }
    else if (udp_ != BAD && t - lastAnnounce_ > 1.0) {
      lastAnnounce_ = t;
      sendDiscovery(ANNOUNCE + std::to_string(port_) + " " + name_);
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
      master_ = true;
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
      if (err != 0 || FD_ISSET(SockRaw(tcp_), &e)) { fail(L("connexion impossible à ", "cannot connect to ") + info_); return; }
      if (mode_ == Mode::Relay) {   // inscription au relais : JOIN2 AUTO|SALLE \0 code \0 version \0 nom
        int on = 1;
        setsockopt(tcp_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof on);
        std::string j = room_.empty() ? "AUTO" : "SALLE";
        j.push_back('\0'); j += room_; j.push_back('\0'); j += version_; j.push_back('\0'); j += name_;
        j.push_back('\0'); j += tnt_ ? "tnt" : "";   // circuits TNT présents
        frame(NF_JOIN2, j);
        state_ = State::Waiting; info_ = "WAIT"; lastRx_ = t;
      } else startSession();
    } else if (t - started_ > 10) { fail(L("pas de réponse de ", "no answer from ") + info_); return; }
  }
  if (tcp_ == BAD || connecting_) return;
  // lecture
  for (;;) {
    uint8_t buf[4096];
    int r = int(::recv(tcp_, reinterpret_cast<char *>(buf), sizeof buf, 0));
    if (r > 0) { in_.insert(in_.end(), buf, buf + r); continue; }
    if (r == 0) { fail(helloOk_ ? L("l'autre joueur s'est déconnecté", "the other player disconnected") : L("connexion fermée par ", "connection closed by ") + info_); return; }
    if (!SCR_WOULDBLOCK(sockErr())) { fail(L("connexion perdue", "connection lost")); return; }
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
  if (helloOk_ && t - lastRx_ > 15) { fail(L("plus de nouvelles de l'autre joueur", "no more news from the other player")); return; }
  if (!dataOut_.empty()) { frame(NF_DATA, dataOut_.data(), dataOut_.size()); dataOut_.clear(); }
  flush();
}

// ------------------------------------------------------------------ test du serveur relais
RelayProbe::~RelayProbe() {
  if (th_.joinable()) th_.detach();   // le fil ne touche qu'à sh_ (partagé) : il peut finir seul
}

void RelayProbe::start(const std::string &server, bool status) {
  if (th_.joinable()) th_.detach();
  auto sh = std::make_shared<Shared>();
  sh->server = server; sh->wantStatus = status;
  sh_ = sh;
  if (server.empty()) { sh->state = State::Failed; sh->error = L("aucun serveur dans scr.ini", "no server in scr.ini"); return; }
  sh->state = State::Probing;
#ifdef __EMSCRIPTEN__
  {   // navigateur : WebSocket ouverte maintenant, réponse lue par step()
    std::string host; int port = NET_PORT; bool secure = false;
    if (!webHostPort(server, host, port, secure)) { sh->state = State::Failed; sh->error = L("adresse du serveur introuvable", "server address not found"); return; }
    webScheme(secure);
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
      sh->state = State::Failed; sh->error = L("adresse du serveur introuvable", "server address not found"); return;
    }
    sh->fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sh->fd < 0) { freeaddrinfo(res); sh->state = State::Failed; sh->error = L("réseau indisponible", "network unavailable"); return; }
    nonBlock(sh->fd);
    ::connect(sh->fd, res->ai_addr, socklen_t(res->ai_addrlen));
    freeaddrinfo(res);
    sh->t0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    sh->phase = 1;
  }
#else
  th_ = std::thread([sh] {
    auto fin = [&](State st, const std::string &err) { std::lock_guard<std::mutex> l(sh->mu); sh->state = st; sh->error = err; };
    std::string host = sh->server; int port = NET_PORT;
    if (isWsUrl(host)) {   // par un reverse proxy web : tunnel local vers la WebSocket
      std::string err;
      host = openWsTunnel(sh->server, err);
      if (host.empty()) { fin(State::Failed, err); return; }
    }
    size_t c = host.rfind(':');
    if (c != std::string::npos && host.find(':') == c) { port = std::atoi(host.c_str() + c + 1); host = host.substr(0, c); }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (port <= 0 || getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
      fin(State::Failed, L("adresse du serveur introuvable", "server address not found")); return;
    }
    SockRaw s = SockRaw(socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    if (intptr_t(s) == -1) { freeaddrinfo(res); fin(State::Failed, L("réseau indisponible", "network unavailable")); return; }
    nonBlock(intptr_t(s));
    double t0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    ::connect(s, res->ai_addr, socklen_t(res->ai_addrlen));
    freeaddrinfo(res);
    auto waitFor = [&](bool write, double secs) {
      fd_set f, e; FD_ZERO(&f); FD_ZERO(&e); FD_SET(s, &f); FD_SET(s, &e);
      timeval tv{long(secs), long((secs - long(secs)) * 1e6)};
      return select(int(s + 1), write ? nullptr : &f, write ? &f : nullptr, &e, &tv) > 0 && FD_ISSET(s, &f);
    };
    if (!waitFor(true, 5)) { closeRaw(intptr_t(s)); fin(State::Failed, L("le serveur ne répond pas", "the server does not answer")); return; }
    int err = 0; socklen_t l = sizeof err;
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err), &l);
    if (err != 0) { closeRaw(intptr_t(s)); fin(State::Failed, L("connexion refusée (serveur arrêté ?)", "connection refused (server stopped?)")); return; }
    const bool tunnel = isWsUrl(sh->server);
    double t1 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(), t2 = 0;
    const char q[3] = {char(sh->wantStatus ? NF_STATUS : NF_PROBE), 0, 0};
    ::send(s, q, 3, 0);
    std::string got;
    while (got.size() < 3 || got.size() < 3 + ((size_t(uint8_t(got[1])) << 8) | uint8_t(got[2]))) {
      if (!waitFor(false, tunnel ? 12 : 5)) break;   // tunnel : connexion chiffrée en plus
      char b[512];
      int r = int(::recv(s, b, sizeof b, 0));
      if (r <= 0) break;
      if (got.empty()) t2 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
      got.append(b, size_t(r));
    }
    closeRaw(intptr_t(s));
    if (got.size() < 3) {
      std::string why = tunnel ? tunnelError() : "";   // par un tunnel : raison précise (proxy, certificat...)
      if (why.empty() && sh->wantStatus) why = L("pas de réponse (serveur v80 ou plus récent ?)", "no answer (server v80 or later?)");
      fin(State::Failed, why.empty() ? std::string(L("pas de réponse (est-ce bien le relais ?)", "no answer (is it really the relay?)")) : why); return;
    }
    std::lock_guard<std::mutex> lk(sh->mu);
    // ping : temps de connexion ; par un tunnel, temps de la réponse (connexion chiffrée comprise)
    sh->pingMs = int(((tunnel ? t2 - t1 : t1 - t0)) * 1000 + 0.5);
    applyReply(*sh, got);
  });
#endif
}

void RelayProbe::applyReply(Shared &sh, const std::string &got) {
  std::string msg = got.substr(3);
  if (uint8_t(got[0]) == NF_STATUS) sh.status = msg;
  if (uint8_t(got[0]) == NF_INFO && msg.compare(0, 7, "SERVER ") == 0) {
    std::sscanf(msg.c_str() + 7, "%d %d", &sh.players, &sh.games);
    size_t sc = msg.find(';');
    if (sc != std::string::npos) sh.params = msg.substr(sc + 1);
    size_t v = sh.params.find(";version=");   // serveur v49 et suivants : sa version
    if (v != std::string::npos) {
      sh.version = cleanText(sh.params.substr(v + 9), 16);
      sh.params.resize(v);
    }
  }
  sh.state = State::Ok;   // un ancien relais répond aussi (message d'erreur) : il est en service
}

#ifdef __EMSCRIPTEN__
// navigateur : connexion WebSocket (Emscripten) ouverte au départ, puis le test avance à chaque
// consultation de son état : connexion établie -> demande envoyée -> réponse complète, ou délai dépassé
void RelayProbe::step() const {
  if (!sh_) return;
  Shared &sh = *sh_;
  std::lock_guard<std::mutex> l(sh.mu);
  if (sh.state != State::Probing || sh.fd < 0) return;
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  auto fail = [&](const char *fr, const char *en) {
    ::close(sh.fd); sh.fd = -1; sh.state = State::Failed;
    sh.error = sh.wantStatus && sh.phase == 2 ? L("pas de réponse (serveur v80 ou plus récent ?)", "no answer (server v80 or later?)") : L(fr, en);
  };
  if (sh.phase == 1) {   // connexion
    fd_set w; FD_ZERO(&w); FD_SET(sh.fd, &w);
    timeval tv{0, 0};
    if (select(sh.fd + 1, nullptr, &w, nullptr, &tv) > 0 && FD_ISSET(sh.fd, &w)) {
      int err = 0; socklen_t len = sizeof err;
      getsockopt(sh.fd, SOL_SOCKET, SO_ERROR, &err, &len);
      if (err) { fail("connexion refusée (serveur arrêté ?)", "connection refused (server stopped?)"); return; }
      const char q[3] = {char(sh.wantStatus ? NF_STATUS : NF_PROBE), 0, 0};
      ::send(sh.fd, q, 3, 0);
      sh.t1 = t; sh.phase = 2;
    } else if (t - sh.t0 > 8) { fail("le serveur ne répond pas", "the server does not answer"); return; }
  }
  if (sh.phase == 2) {   // réponse
    char b[4096];
    for (;;) {
      int r = int(::recv(sh.fd, b, sizeof b, 0));
      if (r > 0) { if (sh.got.empty()) sh.t2 = t; sh.got.append(b, size_t(r)); continue; }
      if (r == 0 && sh.got.size() < 3) { fail("pas de réponse (est-ce bien le relais ?)", "no answer (is it really the relay?)"); return; }
      break;
    }
    const std::string &g = sh.got;
    if (g.size() >= 3 && g.size() >= 3 + ((size_t(uint8_t(g[1])) << 8) | uint8_t(g[2]))) {
      ::close(sh.fd); sh.fd = -1;
      sh.pingMs = int((sh.t2 - sh.t1) * 1000 + 0.5);
      applyReply(sh, g);
    } else if (t - sh.t1 > 12) fail("pas de réponse (est-ce bien le relais ?)", "no answer (is it really the relay?)");
  }
}
#else
void RelayProbe::step() const {}
#endif

int RelayProbe::players() const {
  if (!sh_) return -1;
  step();
  std::lock_guard<std::mutex> l(sh_->mu);
  return sh_->players;
}

int RelayProbe::games() const {
  if (!sh_) return -1;
  step();
  std::lock_guard<std::mutex> l(sh_->mu);
  return sh_->games;
}

std::string RelayProbe::status() const {
  if (!sh_) return "";
  step();
  std::lock_guard<std::mutex> l(sh_->mu);
  return sh_->status;
}

std::string RelayProbe::version() const {
  if (!sh_) return "";
  step();
  std::lock_guard<std::mutex> l(sh_->mu);
  return sh_->version;
}

std::string RelayProbe::params() const {
  if (!sh_) return "";
  step();
  std::lock_guard<std::mutex> l(sh_->mu);
  return sh_->params;
}

RelayProbe::State RelayProbe::state() const {
  if (!sh_) return State::Off;
  step();
  std::lock_guard<std::mutex> l(sh_->mu);
  return sh_->state;
}

std::string RelayProbe::text() const {
  if (!sh_) return "";
  step();
  std::lock_guard<std::mutex> l(sh_->mu);
  switch (sh_->state) {
    case State::Off: return "";
    case State::Probing: return L("Test du serveur en cours...", "Testing the server...");
    case State::Failed: return L("Hors ligne : ", "Offline: ") + sh_->error;
    case State::Ok: {
      std::string t = L("En ligne - ping ", "Online - ping ") + std::to_string(sh_->pingMs) + " ms";
      if (sh_->players >= 0) t += " - " + std::to_string(sh_->players) + (g_french ? (sh_->players > 1 ? " joueurs" : " joueur") : (sh_->players == 1 ? " player" : " players"));
      return t;
    }
  }
  return "";
}

std::string NetLink::status() const {
  switch (state_) {
    case State::Off: return "";
    case State::Failed: return L("Réseau : ", "Network: ") + error_;
    case State::Connected:
      return L("Relié à ", "Linked to ") + (peerName_.empty() ? std::string(L("l'autre joueur", "the other player")) : peerName_) +
             (ping_ >= 0 ? " - ping " + std::to_string(ping_) + " ms" : "");
    case State::Handshake: return L("Réseau : vérification des versions...", "Network: checking versions...");
    case State::Connecting: return L("Réseau : connexion à ", "Network: connecting to ") + info_ + "...";
    case State::Waiting:
      if (mode_ == Mode::Lobby) return L("Réseau local : visible sous le nom ", "Local network: visible as ") + name_ + (peers_.empty() ? L(", aucun autre joueur pour l'instant", ", no other player yet") : "");
      if (mode_ == Mode::LanHost) return L("Réseau local : partie hébergée, en attente d'un joueur (port ", "Local network: game hosted, waiting for a player (port ") + std::to_string(port_) + ")";
      if (mode_ == Mode::LanJoin) return L("Réseau local : recherche d'une partie...", "Local network: looking for a game...");
      if (mode_ == Mode::Relay && groupIn_ && groupDuel_) {
        std::string other;
        for (auto &gm : groupMembers_) if (gm.id != groupMe_) other = gm.name;
        return L("En ligne : course à deux contre ", "Online: two-player race against ") + other;
      }
      if (mode_ == Mode::Relay && room_.empty()) return L("En ligne : recherche d'un adversaire (premier joueur disponible)", "Online: looking for an opponent (first available player)");
      if (mode_ == Mode::Relay && groupIn_)
        return L("Course à plusieurs : salle ", "Multi-car race: room ") + room_ + ", " + std::to_string(groupMembers_.size()) + L(" joueur(s)", " player(s)");
      if (mode_ == Mode::Relay)
        return L("En ligne : salle ", "Online: room ") + room_ + (peers_.empty() ? L(", personne d'autre pour l'instant", ", nobody else yet") : "");
      return L("Réseau : en attente", "Network: waiting");
  }
  return "";
}

}  // namespace scr
