// wstunnel.cpp — voir wstunnel.hpp
#include "wstunnel.hpp"
#include "lang.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX   // windows.h : pas de macros min / max
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
typedef SOCKET TSock;
static const TSock TBAD = INVALID_SOCKET;
static void tclose(TSock s) { closesocket(s); }
static void tshutdown(TSock s) { shutdown(s, SD_BOTH); }
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int TSock;
static const TSock TBAD = -1;
static void tclose(TSock s) { ::close(s); }
static void tshutdown(TSock s) { shutdown(s, SHUT_RDWR); }
#endif

namespace scr {

bool isWsUrl(const std::string &a) { return a.compare(0, 6, "wss://") == 0 || a.compare(0, 5, "ws://") == 0; }

namespace {

std::mutex errMu;
std::string lastErr;   // raison du dernier échec d'ouverture (affichée par le jeu)
void tlog(const std::string &m);
void setErr(const std::string &e) { { std::lock_guard<std::mutex> l(errMu); lastErr = e; } if (!e.empty()) tlog("échec : " + e); }

// trace du tunnel dans scr_reseau.log (dossier du jeu) : étapes, octets, erreurs ; jamais l'adresse
void tlog(const std::string &m) {
  static std::mutex mu;
  std::lock_guard<std::mutex> l(mu);
  FILE *f = std::fopen("scr_reseau.log", "a");
  if (!f) return;
  char ts[32]; time_t t = time(nullptr); strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
  std::fprintf(f, "%s  %s\n", ts, m.c_str());
  std::fclose(f);
}

// réponse HTTP autre que 101 (mise à niveau WebSocket refusée)
std::string httpHint(int st) {
  std::string r = L("réponse HTTP ", "HTTP answer ") + std::to_string(st) + L(" au lieu de 101", " instead of 101");
  if (st == 404) r += L(" : le serveur relais a reçu la requête sans en-tête WebSocket (proxy : ajouter l'en-tête personnalisé WebSocket)", " : the relay server got the request without the WebSocket headers (proxy: add the WebSocket custom header)");
  else if (st == 502 || st == 503 || st == 504) r += L(" : le proxy n'atteint pas le serveur relais (destination, port, serveur arrêté ?)", " : the proxy cannot reach the relay server (destination, port, server stopped?)");
  else if (st == 400 || st == 426) r += L(" : WebSocket refusée par le proxy (en-tête personnalisé WebSocket ?)", " : WebSocket refused by the proxy (WebSocket custom header?)");
  else if (st == 403) r += L(" : accès refusé par le proxy (profil de contrôle d'accès ?)", " : access denied by the proxy (access control profile?)");
  return r;
}

struct Url { bool secure = false; std::string host, path = "/"; int port = 0; };

bool parseUrl(const std::string &u, Url &r) {
  size_t p;
  if (u.compare(0, 6, "wss://") == 0) { r.secure = true; p = 6; }
  else if (u.compare(0, 5, "ws://") == 0) { r.secure = false; p = 5; }
  else return false;
  size_t slash = u.find('/', p);
  std::string hp = u.substr(p, slash == std::string::npos ? std::string::npos : slash - p);
  if (slash != std::string::npos) r.path = u.substr(slash);
  r.port = r.secure ? 443 : 80;
  size_t c = hp.rfind(':');
  if (c != std::string::npos) { r.port = std::atoi(hp.c_str() + c + 1); hp.resize(c); }
  r.host = hp;
  return !r.host.empty() && r.port > 0 && r.port < 65536;
}

bool sendAll(TSock s, const uint8_t *p, size_t n) {
  while (n) {
    int r = int(::send(s, reinterpret_cast<const char *>(p), int(n), 0));
    if (r <= 0) return false;
    p += r; n -= size_t(r);
  }
  return true;
}

// liaison avec le serveur : messages binaires d'une WebSocket
class Transport {
 public:
  virtual ~Transport() = default;
  virtual bool open(const Url &u) = 0;
  virtual int recv(uint8_t *buf, size_t n) = 0;          // > 0 : octets reçus ; 0 : fermée
  virtual bool send(const uint8_t *p, size_t n) = 0;
  virtual void shutdown() = 0;                           // fin d'envoi (trame de fermeture)
};

// ws:// sans chiffrement, sur un simple socket (toutes plateformes)
class PlainWs : public Transport {
 public:
  ~PlainWs() override { if (s_ != TBAD) tclose(s_); }
  bool open(const Url &u) override {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0 || !res) return false;
    s_ = TSock(socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    bool ok = s_ != TBAD && ::connect(s_, res->ai_addr, int(res->ai_addrlen)) == 0;
    freeaddrinfo(res);
    if (!ok) { setErr(L("connexion impossible", "cannot connect")); return false; }
    int on = 1;
    setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof on);
    uint8_t key[16];
    for (auto &k : key) k = uint8_t(rng_());
    std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + b64(key, 16) +
                      "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if (!sendAll(s_, reinterpret_cast<const uint8_t *>(req.data()), req.size())) return false;
    std::string head;
    char b[1024];
    while (head.find("\r\n\r\n") == std::string::npos) {
      if (head.size() > 8192) return false;
      int r = int(::recv(s_, b, sizeof b, 0));
      if (r <= 0) return false;
      head.append(b, size_t(r));
    }
    size_t end = head.find("\r\n\r\n") + 4;
    if (head.compare(0, 12, "HTTP/1.1 101") != 0) { setErr(httpHint(std::atoi(head.c_str() + 9))); return false; }
    in_.assign(head.begin() + long(end), head.end());
    return true;
  }
  int recv(uint8_t *buf, size_t n) override {
    for (;;) {
      if (!pending_.empty()) {   // données d'une trame déjà décodée
        size_t k = std::min(n, pending_.size());
        std::memcpy(buf, pending_.data(), k);
        pending_.erase(pending_.begin(), pending_.begin() + long(k));
        return int(k);
      }
      size_t h = 0, len = 0;
      if (in_.size() >= 2) {   // en-tête de trame complet ?
        len = in_[1] & 0x7f; h = 2;
        if (len == 126) { if (in_.size() >= 4) { len = (size_t(in_[2]) << 8) | in_[3]; h = 4; } else h = 0; }
        else if (len == 127) {
          if (in_.size() >= 10) { len = 0; for (int k = 2; k < 10; k++) len = (len << 8) | in_[size_t(k)]; h = 10; }
          else h = 0;
        }
        if (h && (in_[1] & 0x80)) h += 4;
        if (len > (1u << 20)) return 0;
      }
      if (h && in_.size() >= h + len) {   // trame complète
        uint8_t op = in_[0] & 0x0f;
        std::vector<uint8_t> pay(in_.begin() + long(h), in_.begin() + long(h + len));
        if (in_[1] & 0x80) for (size_t i = 0; i < len; i++) pay[i] ^= in_[h - 4 + (i & 3)];
        in_.erase(in_.begin(), in_.begin() + long(h + len));
        if (op == 0x8) return 0;                                            // fermeture
        if (op == 0x9) { frame(0xA, pay.data(), pay.size()); continue; }   // ping -> pong
        if (op != 0xA) pending_.insert(pending_.end(), pay.begin(), pay.end());
        continue;
      }
      uint8_t b[8192];
      int r = int(::recv(s_, reinterpret_cast<char *>(b), sizeof b, 0));
      if (r <= 0) return 0;
      in_.insert(in_.end(), b, b + r);
    }
  }
  bool send(const uint8_t *p, size_t n) override { return frame(0x2, p, n); }
  void shutdown() override { frame(0x8, nullptr, 0); }

 private:
  bool frame(uint8_t op, const uint8_t *p, size_t n) {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<uint8_t> f;
    f.push_back(uint8_t(0x80 | op));
    if (n < 126) f.push_back(uint8_t(0x80 | n));
    else if (n < 65536) { f.push_back(0x80 | 126); f.push_back(uint8_t(n >> 8)); f.push_back(uint8_t(n)); }
    else { f.push_back(0x80 | 127); for (int k = 7; k >= 0; k--) f.push_back(uint8_t(uint64_t(n) >> (8 * k))); }
    uint8_t m[4];
    for (auto &x : m) x = uint8_t(rng_());
    f.insert(f.end(), m, m + 4);
    for (size_t i = 0; i < n; i++) f.push_back(p[i] ^ m[i & 3]);
    return sendAll(s_, f.data(), f.size());
  }
  static std::string b64(const uint8_t *p, size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string r;
    for (size_t i = 0; i < n; i += 3) {
      uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
      r += T[v >> 18 & 63]; r += T[v >> 12 & 63];
      r += i + 1 < n ? T[v >> 6 & 63] : '='; r += i + 2 < n ? T[v & 63] : '=';
    }
    return r;
  }
  TSock s_ = TBAD;
  std::vector<uint8_t> in_, pending_;
  std::mutex mu_;
  std::mt19937 rng_{std::random_device{}()};
};

#ifdef _WIN32
// wss:// (et ws://) par WinHTTP : TLS, certificats et proxy du système
class WinWs : public Transport {
 public:
  ~WinWs() override {
    if (ws_) WinHttpCloseHandle(ws_);
    if (con_) WinHttpCloseHandle(con_);
    if (ses_) WinHttpCloseHandle(ses_);
  }
  static std::string winErr(const char *step) {
    DWORD e = GetLastError();
    std::string w;
    switch (e) {
      case 12007: w = L("nom du serveur introuvable (DNS)", "server name not found (DNS)"); break;
      case 12029: w = L("connexion impossible (box, redirection du port 443, NAT en boucle ?)", "cannot connect (router, port 443 forwarding, NAT loopback?)"); break;
      case 12002: w = L("délai dépassé", "timed out"); break;
      case 12030: case 12152: w = L("connexion coupée par le proxy ou le serveur (serveur relais à jour lancé ?)", "connection cut by the proxy or the server (up-to-date relay server running?)"); break;
      case 12175: case 12045: case 12057: case 12037: case 12038: case 12044: w = L("certificat refusé", "certificate refused"); break;
      default: w = L("erreur", "error"); break;
    }
    return std::string("WinHTTP ") + step + " : " + w + " (" + std::to_string(e) + ")";
  }
  bool open(const Url &u) override {
    std::wstring host = wide(u.host), path = wide(u.path);
    ses_ = WinHttpOpen(L"StuntCarRacer", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses_) { setErr(winErr(L("ouverture", "opening"))); return false; }
    DWORD keep = 20000;   // trame de maintien toutes les 20 s : les proxys ferment les liaisons muettes
    WinHttpSetOption(ses_, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &keep, sizeof keep);
    con_ = WinHttpConnect(ses_, host.c_str(), INTERNET_PORT(u.port), 0);
    if (!con_) { setErr(winErr(L("connexion", "connection"))); return false; }
    HINTERNET req = WinHttpOpenRequest(con_, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       u.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { setErr(winErr(L("requête", "request"))); return false; }
    bool ok = true;
    if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) { setErr(winErr("WebSocket")); ok = false; }
    else if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)) { setErr(winErr(L("envoi", "sending"))); ok = false; }
    else if (!WinHttpReceiveResponse(req, nullptr)) { setErr(winErr(L("réponse", "answer"))); ok = false; }
    DWORD status = 0, sz = sizeof status;
    if (ok && !WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                   &status, &sz, WINHTTP_NO_HEADER_INDEX)) { setErr(winErr(L("statut", "status"))); ok = false; }
    if (ok && status != 101) { setErr(httpHint(int(status))); ok = false; }
    if (ok && !(ws_ = WinHttpWebSocketCompleteUpgrade(req, 0))) setErr(winErr(L("mise à niveau", "upgrade")));
    if (ws_) tlog("WebSocket ouverte (HTTP 101)");
    WinHttpCloseHandle(req);
    return ws_ != nullptr;
  }
  int recv(uint8_t *buf, size_t n) override {
    for (;;) {
      DWORD got = 0;
      WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
      DWORD e = WinHttpWebSocketReceive(ws_, buf, DWORD(n), &got, &type);
      if (e != NO_ERROR) { tlog("réception : erreur WinHTTP " + std::to_string(e)); return 0; }
      if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) { tlog("fermeture par le serveur"); return 0; }
      if (got) return int(got);
    }
  }
  bool send(const uint8_t *p, size_t n) override {
    DWORD e = WinHttpWebSocketSend(ws_, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, const_cast<uint8_t *>(p), DWORD(n));
    if (e != NO_ERROR) tlog("envoi : erreur WinHTTP " + std::to_string(e));
    return e == NO_ERROR;
  }
  void shutdown() override { WinHttpWebSocketShutdown(ws_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0); }

 private:
  static std::wstring wide(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 1), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize(w.size() - 1);
    return w;
  }
  HINTERNET ses_ = nullptr, con_ = nullptr, ws_ = nullptr;
};
#endif

// une connexion locale du jeu <-> la WebSocket, jusqu'à ce que l'une des deux se ferme
void runTunnel(TSock ls, Url u) {
  fd_set f; FD_ZERO(&f); FD_SET(ls, &f);
  timeval tv{15, 0};
  TSock local = TBAD;
  if (select(int(ls + 1), &f, nullptr, nullptr, &tv) > 0) local = TSock(accept(ls, nullptr, nullptr));
  tclose(ls);
  if (local == TBAD) { tlog("tunnel : le jeu ne s'est pas connecté"); return; }
  tlog(std::string("tunnel : ouverture ") + (u.secure ? "wss" : "ws"));
  int on = 1;
  setsockopt(local, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof on);
  std::unique_ptr<Transport> t;
#ifdef _WIN32
  t.reset(new WinWs);
#else
  if (!u.secure) t.reset(new PlainWs);
#endif
  if (!t || !t->open(u)) { tclose(local); return; }   // le jeu voit la connexion se fermer : « pas de réponse »
  Transport *tp = t.get();
  std::thread up([local, tp] {   // jeu -> serveur
    uint8_t b[8192];
    size_t total = 0;
    for (;;) {
      int r = int(::recv(local, reinterpret_cast<char *>(b), sizeof b, 0));
      if (r <= 0 || !tp->send(b, size_t(r))) break;
      if (total < 64) tlog("jeu -> serveur : " + std::to_string(r) + " octets");
      total += size_t(r);
    }
    tp->shutdown();
  });
  uint8_t b[16384];
  size_t total = 0;
  for (;;) {   // serveur -> jeu
    int r = t->recv(b, sizeof b);
    if (r <= 0 || !sendAll(local, b, size_t(r))) break;
    if (total < 512) tlog("serveur -> jeu : " + std::to_string(r) + " octets");
    total += size_t(r);
  }
  tlog("tunnel : fin (" + std::to_string(total) + " octets reçus du serveur)");
  tshutdown(local);
  up.join();
  tclose(local);
}

}  // namespace

std::string tunnelError() { std::lock_guard<std::mutex> l(errMu); return lastErr; }

std::string openWsTunnel(const std::string &url, std::string &err) {
  setErr("");
  Url u;
  if (!parseUrl(url, u)) { err = L("adresse wss:// invalide", "invalid wss:// address"); return ""; }
#ifndef _WIN32
  if (u.secure) { err = L("connexion wss:// disponible seulement sous Windows (ws:// ou adresse:port ici)", "wss:// connections only on Windows (use ws:// or address:port here)"); return ""; }
#endif
  TSock ls = TSock(socket(AF_INET, SOCK_STREAM, 0));
  if (ls == TBAD) { err = L("réseau indisponible", "network unavailable"); return ""; }
  sockaddr_in a{};
  a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
  socklen_t l = sizeof a;
  if (bind(ls, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || listen(ls, 1) != 0 ||
      getsockname(ls, reinterpret_cast<sockaddr *>(&a), &l) != 0) {
    tclose(ls); err = L("tunnel local impossible", "cannot open the local tunnel"); return "";
  }
  std::thread(runTunnel, ls, u).detach();
  return "127.0.0.1:" + std::to_string(ntohs(a.sin_port));
}

}  // namespace scr
