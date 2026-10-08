// net.hpp — câble « Computer Link » par le réseau : les octets que le jeu émet sur son port série
// partent vers l'autre joueur, ceux de l'autre joueur arrivent comme s'ils venaient du câble.
//
// Trois façons de se relier (même protocole ensuite) :
//   - réseau local : l'un héberge (écoute TCP + annonce UDP par diffusion), l'autre détecte
//     automatiquement l'annonce et se connecte ;
//   - adresse directe : connexion TCP à adresse[:port] ;
//   - serveur relais : les deux joueurs se connectent au serveur avec le même code de salle, le
//     serveur les apparie et transmet leurs données (aucun port à ouvrir chez les joueurs).
//
// Trames : type (1 octet), longueur (2 octets, gros-boutiste), données. Entre les deux jeux :
// HELLO (version, empreinte du jeu, nom), DATA (octets du câble), PING / PONG (mesure du ping), BYE.
// Avec le relais : JOIN (code de salle) vers le serveur, INFO (texte) depuis le serveur.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace scr {

constexpr int NET_PORT = 27420;           // TCP : hébergement en réseau local et serveur relais
constexpr int NET_DISCOVERY_PORT = 27421; // UDP : annonces de parties en réseau local

enum NetFrame : uint8_t { NF_HELLO = 1, NF_DATA = 2, NF_PING = 3, NF_PONG = 4, NF_BYE = 5, NF_JOIN = 10, NF_INFO = 11 };

class NetLink {
 public:
  enum class Mode { Off, Lobby, LanHost, LanJoin, Direct, Relay };
  struct Peer { uint32_t id; std::string name, ip; int port; double seen; };   // partie vue en réseau local
  enum class State { Off, Waiting, Connecting, Handshake, Connected, Failed };

  NetLink();
  ~NetLink();
  NetLink(const NetLink &) = delete;
  NetLink &operator=(const NetLink &) = delete;

  // identité de ce jeu : version du programme, empreinte du jeu et des réglages, nom affiché
  void setIdentity(const std::string &version, uint32_t gameHash, const std::string &name);

  // salon du réseau local (menu « Computer Link ») : ce jeu s'annonce et liste les autres jeux qui
  // attendent ; on en choisit un (connectPeer) ou un autre joueur nous choisit (connexion entrante)
  bool lobby();
  const std::vector<Peer> &peers() const { return peers_; }
  bool connectPeer(const Peer &p);
  bool hostLan(int port = NET_PORT);                       // héberger en réseau local
  bool joinLan();                                          // rejoindre la première partie annoncée
  bool joinDirect(const std::string &hostPort);            // adresse[:port]
  bool joinRelay(const std::string &server, const std::string &room);   // serveur[:port] + code
  void close(const std::string &why = "");

  // à appeler souvent (plusieurs fois par trame) : connexions, lecture, écriture, annonces, ping
  void poll();
  void send(uint8_t b);              // octet émis par le jeu sur le port série
  bool recv(uint8_t &b);             // octet reçu de l'autre jeu

  Mode mode() const { return mode_; }
  State state() const { return state_; }
  bool active() const { return mode_ != Mode::Off; }
  bool connected() const { return state_ == State::Connected; }
  // rôle dans le jeu : le « maître » lance la poignée de main et mène les menus (celui qui a choisi
  // l'adversaire, ou le premier arrivé dans la salle du relais)
  bool master() const { return master_; }
  int pingMs() const { return ping_; }
  const std::string &peerName() const { return peerName_; }
  std::string status() const;        // texte court pour le titre de la fenêtre
  uint64_t bytesSent() const { return sent_; }
  uint64_t bytesReceived() const { return received_; }

 private:
  using Sock = intptr_t;
  static constexpr Sock BAD = -1;
  Mode mode_ = Mode::Off;
  State state_ = State::Off;
  Sock listen_ = BAD, tcp_ = BAD, udp_ = BAD;
  bool connecting_ = false;
  std::string version_, name_, peerName_, room_, info_, error_;
  uint32_t hash_ = 0;
  int port_ = NET_PORT;
  std::vector<uint8_t> in_, out_;    // tampons TCP (trames)
  std::deque<uint8_t> rx_;           // octets du câble reçus
  std::vector<uint8_t> dataOut_;     // octets du câble à envoyer au prochain poll()
  bool helloSent_ = false, helloOk_ = false, relayPaired_ = false, master_ = false, waitedFirst_ = false;
  uint32_t id_ = 0;                  // identifiant de ce jeu dans les annonces
  std::vector<Peer> peers_;
  bool openListener(int port);
  bool openDiscovery(bool broadcast);
  void announce(double t);
  void readAnnounces(double t);
  double lastAnnounce_ = -1e9, lastPing_ = -1e9, lastRx_ = 0, started_ = 0;
  int ping_ = -1;
  uint64_t sent_ = 0, received_ = 0;

  static double now();
  void frame(uint8_t type, const uint8_t *p, size_t n);
  void frame(uint8_t type, const std::string &s) { frame(type, reinterpret_cast<const uint8_t *>(s.data()), s.size()); }
  void startSession();               // connexion TCP établie (ou appariement par le relais)
  void handle(uint8_t type, const uint8_t *p, size_t n);
  void fail(const std::string &why);
  bool connectTo(const std::string &host, int port);
  void flush();
  void closeSock(Sock &s);
};

// empreinte FNV-1a (vérification que les deux joueurs ont le même jeu et les mêmes réglages)
uint32_t fnv1a(const void *data, size_t n, uint32_t h = 2166136261u);

}  // namespace scr
