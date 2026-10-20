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
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace scr {

constexpr int NET_PORT = 27420;           // TCP : hébergement en réseau local et serveur relais
constexpr int NET_DISCOVERY_PORT = 27421; // UDP : annonces de parties en réseau local

enum NetFrame : uint8_t { NF_HELLO = 1, NF_DATA = 2, NF_PING = 3, NF_PONG = 4, NF_BYE = 5, NF_JOIN = 10, NF_INFO = 11,
                          NF_JOIN2 = 12, NF_LIST = 13, NF_PICK = 14, NF_PROBE = 15, NF_PARAMS = 16, NF_CHAT = 17,
                          NF_GSTATE = 18, NF_GCMD = 19, NF_GEVENT = 20, NF_GPOS = 21 };

class NetLink {
 public:
  enum class Mode { Off, Lobby, LanHost, LanJoin, Direct, Relay };
  struct Peer { uint32_t id; std::string name, ip; int port; double seen; double replied = -1e9; };   // partie vue en réseau local
  enum class State { Off, Waiting, Connecting, Handshake, Connected, Failed };

  NetLink();
  ~NetLink();
  NetLink(const NetLink &) = delete;
  NetLink &operator=(const NetLink &) = delete;

  // identité de ce jeu : version du programme, empreinte du jeu et des réglages, nom affiché
  // gameHash : empreinte du programme du jeu (image disque) ; tuning : réglages de physique en texte
  // (le jeu esclave adopte ceux du maître pendant la partie)
  void setIdentity(const std::string &version, uint32_t gameHash, const std::string &name, const std::string &tuning = "");
  const std::string &peerTuning() const { return peerTuning_; }
  void setName(const std::string &name) { name_ = name.substr(0, 40); }   // pseudo (prochaines annonces / connexions)
  const std::string &name() const { return name_; }
  // réglages imposés par le serveur relais (« gravity=1 thrust=1 ... cadence=400 »), vide sinon
  const std::string &serverParams() const { return serverParams_; }

  // salon du réseau local (menu « Computer Link ») : ce jeu s'annonce et liste les autres jeux qui
  // attendent ; on en choisit un (connectPeer) ou un autre joueur nous choisit (connexion entrante).
  // Même liste et même choix pour une salle privée du serveur relais.
  bool lobby();
  const std::vector<Peer> &peers() const { return peers_; }
  bool connectPeer(const Peer &p);
  bool hostLan(int port = NET_PORT);                       // héberger en réseau local
  bool joinLan();                                          // rejoindre la première partie annoncée
  bool joinDirect(const std::string &hostPort);            // adresse[:port]
  // serveur relais : code vide = automatique (premier joueur disponible) ; sinon salle privée,
  // dont les joueurs présents arrivent dans peers() (choix par connectPeer)
  // display : nom montré à la place de l'adresse dans les messages (vide : l'adresse)
  bool joinRelay(const std::string &server, const std::string &room, const std::string &display = "");
  bool relayRoom() const { return mode_ == Mode::Relay && !room_.empty(); }
  const std::string &room() const { return room_; }

  // discussion : avec les joueurs de la salle privée (par le serveur), puis avec l'adversaire
  struct ChatLine { std::string from, text; double t; bool mine; };
  bool canChat() const { return (relayRoom() && state_ == State::Waiting) || state_ == State::Connected; }
  void sendChat(const std::string &text);
  void localNote(const std::string &text);   // message du jeu lui-même dans la discussion
  const std::deque<ChatLine> &chat() const { return chat_; }
  static double clock() { return now(); }

  // course à plusieurs (2 à 6 joueurs d'une salle privée, voitures fantômes) : le serveur relaie les
  // positions, donne le départ en même temps à tous et collecte les temps d'arrivée
  struct GroupMember { uint32_t id; std::string name, state; };   // état : lobby, loading, loaded, racing, finished:ms
  struct GhostSample { double t; float v[7]; uint8_t lap, flags; };   // x y z lacet tangage roulis progression
  bool inGroup() const { return groupIn_; }
  void groupCommand(const std::string &cmd);   // ENTER, LEAVE, START circuit tours, LOADED, FINISH ms
  const std::vector<GroupMember> &groupMembers() const { return groupMembers_; }
  uint32_t groupMe() const { return groupMe_; }
  uint32_t groupHost() const { return groupHost_; }
  const std::string &groupPhase() const { return groupPhase_; }
  int groupTrack() const { return groupTrack_; }
  int groupLaps() const { return groupLaps_; }
  bool groupDuel() const { return groupDuel_; }   // course à deux (circuit proposé, accepté par les deux)
  bool takeGroupEvent(std::string &ev);        // START / GO / END reçus (dans l'ordre)
  void sendGhost(const float v[7], uint8_t lap, uint8_t flags);
  const std::map<uint32_t, std::deque<GhostSample>> &ghosts() const { return ghosts_; }
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
  // adresses IPv4 de ce PC sur le réseau local (affichées dans le menu, pour « Par adresse IP »)
  std::vector<std::string> localAddresses();
  double sinceStart() const { return now() - started_; }
  uint64_t bytesSent() const { return sent_; }
  uint64_t bytesReceived() const { return received_; }

 private:
  using Sock = intptr_t;
  static constexpr Sock BAD = -1;
  Mode mode_ = Mode::Off;
  State state_ = State::Off;
  Sock listen_ = BAD, tcp_ = BAD, udp_ = BAD;
  bool connecting_ = false;
  std::string version_, name_, peerName_, room_, info_, error_, tuning_, peerTuning_, serverParams_;
  std::string shown_;   // nom montré à la place de l'adresse du serveur
  uint32_t hash_ = 0;
  int port_ = NET_PORT;
  std::vector<uint8_t> in_, out_;    // tampons TCP (trames)
  std::deque<uint8_t> rx_;           // octets du câble reçus
  std::vector<uint8_t> dataOut_;     // octets du câble à envoyer au prochain poll()
  bool helloSent_ = false, helloOk_ = false, relayPaired_ = false, master_ = false, waitedFirst_ = false;
  uint32_t id_ = 0;                  // identifiant de ce jeu dans les annonces
  std::vector<Peer> peers_;
  std::deque<ChatLine> chat_;
  bool groupIn_ = false;
  std::vector<GroupMember> groupMembers_;
  uint32_t groupMe_ = 0, groupHost_ = 0;
  std::string groupPhase_;
  int groupTrack_ = 0, groupLaps_ = 3;
  bool groupDuel_ = false;
  std::deque<std::string> groupEvents_;
  std::map<uint32_t, std::deque<GhostSample>> ghosts_;
  bool openListener(int port);
  bool openDiscovery(bool broadcast);
  void announce(double t);
  void sendDiscovery(const std::string &msg);   // diffusion sur chaque réseau local + boucle locale
  struct LocalNet { uint32_t addr, bcast; };    // ordre hôte
  std::vector<LocalNet> nets_;
  double netsAt_ = -1e9;
  void refreshNets(double t);
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

// test du serveur relais (menu « Online Multiplayer ») : connexion, question PROBE, réponse
// « SERVER joueurs parties ». Dans un fil à part (la résolution du nom peut prendre du temps).
class RelayProbe {
 public:
  enum class State { Off, Probing, Ok, Failed };
  ~RelayProbe();
  void start(const std::string &server);   // serveur[:port]
  State state() const;
  std::string text() const;                // texte pour le menu
  bool ok() const { return state() == State::Ok; }
  std::string params() const;              // réglages imposés par le serveur (vide si inconnus)
  std::string version() const;             // version du serveur (= version du jeu à jour), vide si ancien serveur

 private:
  struct Shared {
    std::mutex mu;
    State state = State::Off;
    std::string server, error, params, version;
    int players = -1, games = -1, pingMs = -1;
  };
  std::shared_ptr<Shared> sh_;
  std::thread th_;
};

// empreinte FNV-1a (vérification que les deux joueurs ont le même jeu et les mêmes réglages)
uint32_t fnv1a(const void *data, size_t n, uint32_t h = 2166136261u);

}  // namespace scr
