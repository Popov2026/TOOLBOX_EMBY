(English version below / version anglaise plus bas)

Serveur relais Stunt Car Racer (multijoueur par Internet)
=========================================================

Rôle : les jeux se connectent au serveur, qui met les joueurs en relation puis transmet leurs
données. Façons de se trouver (menu « 4. Online Multiplayer » du jeu) :
  - « premier joueur disponible » : le serveur met le joueur en course à deux avec le premier
    qui attend ; chacun mène son jeu, le circuit est tiré au sort et accepté par les deux
    (« Autre circuit » en fait tirer un autre) ;
  - « salle privée (code) » : les joueurs qui tapent le même code voient qui est présent,
    choisissent leur adversaire (même course à deux) ou font une course à plusieurs (2 à 6).
  Les joueurs n'ont aucun port à ouvrir : seul le serveur
écoute (port TCP 27420 par défaut). Trafic : environ 300 octets/s par joueur pendant une course.


0. Réglages imposés aux joueurs : scr_relay.ini
-----------------------------------------------

  Le serveur impose ses réglages à tous les jeux qui s'y connectent : physique (gravity, thrust,
  brake, timestep, damping, boostuse, shock) et cadence réseau (reseau_cadence). Les scr.ini des
  joueurs n'ont plus d'importance en ligne : tout le monde joue avec la même physique.
  Modifiez scr_relay.ini (à côté de scr_relay.exe / scr_relay) puis relancez le serveur. Le port
  d'écoute s'y règle aussi (port = 27420). Les réglages en cours sont affichés dans la fenêtre du
  serveur, et dans le menu « 4. Online Multiplayer » des joueurs (« Réglages du serveur : ... »).
  Autre fichier : scr_relay --config mes_reglages.ini


1. Serveur sous Windows (le plus simple pour commencer)
--------------------------------------------------------

  scr_relay.exe est un programme autonome (aucune installation, aucune DLL).

  - Double-cliquez sur scr_relay.exe (il garde aussi un journal dans scr_relay.log, à côté du
    programme). Une fenêtre s'ouvre et affiche :
        écoute sur le port TCP 27420
        adresse(s) de cette machine : 192.168.1.20
    Laissez-la ouverte tant qu'on joue ; fermer la fenêtre arrête le serveur.
  - Au premier lancement, Windows demande d'autoriser scr_relay.exe sur le réseau : acceptez
    (réseaux privés, et publics si le PC est sur un réseau marqué « public »).
  - Joueurs sur le même réseau que ce PC : relais = 192.168.1.20 (l'adresse affichée).
  - Joueurs par Internet : sur la box, redirigez le port TCP 27420 vers l'adresse du PC serveur
    (menu « NAT / redirection de ports » de la box), puis donnez aux joueurs l'adresse publique de
    la box (ou un nom de domaine, par exemple une adresse DynDNS / No-IP) : relais = ADRESSE:27420.
  - Options : scr_relay.exe --port 30000 --log journal.txt
             --journal : simple journal au lieu de l'affichage en direct (service, redirection)

  La fenêtre affiche en direct (mise à jour chaque seconde) :
    - le port, les adresses de la machine, le nombre de joueurs et de parties en cours ;
    - un tableau des joueurs connectés : numéro, nom, adresse IP, mode (automatique ou salle
      privée), salle, état (attend un adversaire / dans la salle, libre / en partie contre X avec
      la durée), temps de connexion et trafic (octets reçus par seconde : ~300 en course) ;
    - les derniers événements (arrivées, parties, départs, messages de discussion des salles).
  Robots d'Internet : un port ouvert reçoit sans arrêt des visites de robots (scanners de ports,
  requêtes web, etc.). Ce ne sont pas des joueurs : le serveur ferme aussitôt toute connexion qui
  ne commence pas par une inscription du jeu, sans l'afficher dans le tableau ni dans le journal ;
  seul le compteur « robots refusés » augmente. Une adresse qui revient 3 fois est signalée une
  fois (« robot (pas un jeu), adresse surveillée pendant 1 h ») et n'a plus que 3 secondes pour
  s'inscrire ; un vrai jeu venant de cette adresse peut toujours jouer.
  Quand un joueur ouvre « 4. Online Multiplayer », son jeu teste le serveur (ligne « test du
  serveur par un jeu », une fraction de seconde) et affiche « En ligne - ping ... » ou « Hors ligne ».

  Démarrage automatique avec Windows (en attendant une version « service Windows ») :
  Planificateur de tâches > Créer une tâche > Déclencheur « Au démarrage », Action « Démarrer un
  programme » = scr_relay.exe, avec « Exécuter même si l'utilisateur n'est pas connecté ».


2. Serveur Linux (VPS ou machine à la maison)
---------------------------------------------

  N'importe quel petit serveur Linux joignable depuis Internet suffit : un serveur virtuel
  (VPS) d'entrée de gamme à quelques euros par mois (OVH, Scaleway, Hetzner, Ionos...), un
  Raspberry Pi, ou une offre gratuite. Pour des joueurs en France, un serveur en France donne le
  meilleur ping.

  Le dossier contient :
    scr_relay            le programme, déjà compilé pour Linux x86-64 (sans dépendance)
    scr_relay.exe        le même pour Windows
    scr_relay.cpp        son code source (le même pour Windows et Linux)
    scr_relay.ini        réglages imposés aux joueurs (physique, cadence réseau) et port
    scr-relay.service    lancement automatique au démarrage (systemd)

  Sur le serveur (en tant que root) :

    # copier les fichiers (depuis votre PC) :
    #   scp scr_relay scr-relay.service root@ADRESSE_DU_SERVEUR:/root/

    install -m 755 scr_relay /usr/local/bin/scr_relay
    install -m 644 scr_relay.ini /usr/local/bin/scr_relay.ini   # réglages (lus à côté du programme)
    useradd --system --no-create-home scrrelay
    install -m 644 scr-relay.service /etc/systemd/system/scr-relay.service
    systemctl daemon-reload
    systemctl enable --now scr-relay

    # pare-feu du serveur, s'il y en a un (exemple avec ufw) :
    ufw allow 27420/tcp

  Vérifier :  systemctl status scr-relay      journal :  journalctl -u scr-relay -f
  Affichage en direct : lancez simplement ./scr_relay dans un terminal (ssh) ; en service, le
  programme écrit un journal (les arrivées, les parties, et toutes les 5 min le nombre de joueurs).

  Autre processeur (ARM, Raspberry Pi...) ou recompilation :
    g++ -O2 -std=c++17 scr_relay.cpp -o scr_relay

  Port différent :  scr_relay --port 30000   (à reporter dans scr.ini des joueurs)


3. Côté joueurs
---------------

  Dans scr.ini, à côté de scr.exe :

    relais = ADRESSE_DU_SERVEUR:27420

  (adresse IP ou nom de domaine). Puis en jeu : « 4. Online Multiplayer », « Premier joueur
  disponible » ou « Salle privée (code) », même code pour tous les joueurs, Entrée.


4. Fonctionnement et limites
----------------------------

  - Automatique : le premier joueur attend (une heure au plus sans activité), le suivant est relié à lui.
  - Salle privée : autant de joueurs que l'on veut ; chacun voit les joueurs libres de la salle
    et en choisit un. Une fois en partie, les deux joueurs disparaissent de la liste.
  - Course à plusieurs : dans une salle privée, de 2 à 6 joueurs ; le serveur donne le départ
    quand tous sont prêts (30 s au plus), relaie les positions et collecte les temps ; la course
    se termine quand tous sont arrivés ou ont abandonné (voiture détruite), ou deux minutes
    après le premier arrivé. (Serveur v34 ou plus récent.)
    Circuits TNT (8 à 15) proposés par l'organisateur : serveur v46 ou plus récent.
  - Les deux joueurs doivent avoir la même version de scr.exe (le serveur refuse sinon).
  - Mises à jour : le serveur porte le même numéro de version que le jeu et l'annonce aux jeux
    qui le testent (au démarrage et dans le menu « 4. Online Multiplayer », jeu v49 ou plus
    récent) ; un jeu plus ancien affiche « Nouvelle version vXX disponible ». Mettre le serveur à
    jour en même temps que le jeu distribué aux joueurs.
  - Une connexion silencieuse plus de 90 s est fermée ; inscription du jeu sous 8 s ; 16
    connexions au plus par adresse IP, 512 en tout.
  - Le serveur ne voit que des octets du jeu : aucun compte, aucune donnée personnelle.

5. Sécurité (port ouvert sur la box)
------------------------------------

  Ouvrir un port n'est pas une faille en soi : le risque est dans le programme qui écoute. Ce
  serveur ne fait que relayer des trames de jeu ; il n'exécute rien, n'ouvre aucun fichier à la
  demande d'un joueur et ne donne aucun accès à la machine.
  Protections : 16 connexions par adresse IP, 512 en tout ; 1 Mo au plus en attente par joueur
  dans chaque sens (au-delà, le joueur est déconnecté : ni inondation, ni client qui ne lit plus
  ne peuvent remplir la mémoire) ; inscription sous 8 s (robots écartés sans bruit, voir plus haut) ; noms et messages réduits à de l'UTF-8
  valide sans caractères de contrôle (pas de séquences d'échappement dans la console) ; valeurs
  bornées (circuit, tours, temps, 6 joueurs par course). Testé avec des trames aléatoires, des
  noms piégés et des inondations, sous AddressSanitizer : aucune erreur.
  Ce qui reste : quelqu'un de malveillant peut saturer le serveur (gêne pour jouer, sans accès à
  la machine), et les joueurs connaissent votre adresse IP publique.
  Conseils :
  - box : rediriger uniquement ce port, en TCP, vers la machine du serveur (jamais de « DMZ ») ;
  - lancer le serveur sans droits d'administrateur (sous Linux, scr-relay.service le fait avec
    un compte dédié et des protections systemd) ; pare-feu Windows : « réseaux privés » suffit ;
  - fermer la redirection quand vous ne jouez pas, ou héberger le serveur sur un petit VPS.

6. Derrière un reverse proxy web (HTTPS, par exemple un NAS Synology)
--------------------------------------------------------------------

  Un reverse proxy web ne transmet que du HTTP/HTTPS : le jeu y passe en WebSocket sécurisée
  (adresse wss://... côté jeu ; serveur v73 et jeu v73 ou plus récents). Le serveur relais
  accepte ces connexions sur son port habituel (27420), en plus des connexions directes.
  Exemple avec le reverse proxy d'un NAS Synology (Panneau de configuration > Portail de
  connexion > Avancé > Proxy inversé > Créer) :
    - Source : protocole HTTPS, nom d'hôte = votre nom de domaine, port 443 ;
      un certificat valide pour ce nom (Panneau de configuration > Sécurité > Certificat) ;
    - Destination : protocole HTTP, nom d'hôte = adresse du PC du serveur relais (par exemple
      192.168.1.110), port 27420 ;
    - onglet « En-tête personnalisé » : bouton « Créer » > « WebSocket » (ajoute les en-têtes
      Upgrade et Connection) : INDISPENSABLE, sans lui la connexion est refusée ;
    - Enregistrer. Sur la box, le port 443 doit arriver sur le NAS.
  Côté joueurs : relais = wss://votre.domaine (port 443 par défaut ; wss://domaine:port/chemin
  possible). Connexion chiffrée par Windows (WinHTTP) avec ses certificats.
  Le serveur voit les joueurs avec leur vraie adresse IP (en-têtes X-Real-IP / X-Forwarded-For
  ajoutés par le proxy, lus seulement quand la connexion vient du réseau local) ; il envoie une
  trame de maintien toutes les 20 s, et le jeu aussi, pour que le proxy ne coupe pas une liaison
  muette (délai de 60 s par défaut chez Synology). Les requêtes web ordinaires (robots) reçoivent
  une réponse « 404 » et sont traitées comme les autres robots.
  En cas de souci : le message « Hors ligne : ... » du jeu donne la raison (réponse HTTP du proxy,
  certificat...) ; le jeu note aussi chaque étape dans scr_reseau.log (dossier du jeu), et la
  fenêtre du serveur affiche « connexion WebSocket » et « test du serveur par WebSocket ».
  Page web d'état : https://votre.domaine/ dans un navigateur (par le proxy, ou http://adresse:27420/
  directement) affiche en direct (rafraîchie toutes les 5 s, en anglais) les parties en cours
  (circuit, tours, état de chaque joueur), les joueurs connectés (mode, salle, état, durée) et les
  salles privées ouvertes avec leurs joueurs ; mêmes informations en JSON : /status.json. Aucune
  adresse IP n'y figure (attention : les codes des salles privées y sont visibles).


7. Page d'administration (mot de passe)
  Ajoutez dans scr_relay.ini une ligne « admin = votre-mot-de-passe », puis relancez le serveur :
  https://votre.domaine/admin (ou http://adresse:27420/admin) demande ce mot de passe (le nom
  d'utilisateur n'a pas d'importance) et montre, actualisé toutes les 5 s :
  - les codes de licence de la version navigateur (lignes « licence = NOM EMPREINTE ») : utilisé
    ou pas encore, combien de fois, quand et depuis quelle adresse ;
  - les joueurs connectés avec leur adresse, leur version, leur salle ou course et ce qu'ils font ;
  - les robots refusés et les adresses bloquées ;
  - les 200 dernières lignes du journal.
  3 mots de passe faux bloquent l'adresse pendant 1 h. Sans ligne « admin », la page n'existe pas.

==============================================================================
ENGLISH
==============================================================================

Stunt Car Racer relay server (multiplayer over the Internet)
============================================================

Role: the games connect to the server, which puts the players in touch and then passes their
data along. Ways to find each other (the game's "4. Online Multiplayer" menu):
  - "premier joueur disponible" (first available player): the server puts the player in a
    two-player race with the first one waiting; each runs their own game, the track is drawn at
    random and accepted by both ("Autre circuit" draws another one);
  - "salle privée (code)" (private room): players who type the same code see who is there,
    choose their opponent (same two-player race) or run a multi-car race (2 to 6).
  Players have no port to open: only the server listens (TCP port 27420 by default).
  Traffic: about 300 bytes/s per player during a race.


0. Settings imposed on players: scr_relay.ini
---------------------------------------------

  The server imposes its settings on every game that connects to it: physics (gravity, thrust,
  brake, timestep, damping, boostuse, shock) and network rate (reseau_cadence). The players'
  scr.ini files no longer matter online: everyone plays with the same physics.
  Edit scr_relay.ini (next to scr_relay.exe / scr_relay) then restart the server. The listening
  port is set there too (port = 27420). The current settings are shown in the server window, and
  in the players' "4. Online Multiplayer" menu ("Réglages du serveur : ..." = server settings).
  Another file: scr_relay --config my_settings.ini


1. Server on Windows (the easiest way to start)
-----------------------------------------------

  scr_relay.exe is a stand-alone program (no installation, no DLL).

  - Double-click scr_relay.exe (it also keeps a log in scr_relay.log, next to the program).
    A window opens and shows:
        écoute sur le port TCP 27420          (listening on TCP port 27420)
        adresse(s) de cette machine : 192.168.1.20   (this machine's address(es))
    Leave it open while people play; closing the window stops the server.
  - On first start, Windows asks whether to allow scr_relay.exe on the network: accept
    (private networks, and public ones if the PC is on a network marked "public").
  - Players on the same network as this PC: relais = 192.168.1.20 (the address shown).
  - Players over the Internet: on your router, forward TCP port 27420 to the server PC's address
    (the router's "NAT / port forwarding" menu), then give the players the router's public
    address (or a domain name, for example a DynDNS / No-IP address): relais = ADDRESS:27420.
  - Options: scr_relay.exe --port 30000 --log journal.txt
             --journal : plain log instead of the live display (service, redirection)

  The window shows live (updated every second):
    - the port, the machine's addresses, the number of players and of races in progress;
    - a table of the connected players: number, name, IP address, mode (automatic or private
      room), room, state (waiting for an opponent / in the room, free / racing against X with
      the duration), connection time and traffic (bytes received per second: ~300 in a race);
    - the latest events (arrivals, races, departures, room chat messages).
  Internet robots: an open port is constantly visited by robots (port scanners, web requests,
  etc.). They are not players: the server immediately closes any connection that does not start
  with a game registration, without showing it in the table or in the log; only the "robots
  refusés" (robots refused) counter goes up. An address that comes back 3 times is reported once
  ("robot (pas un jeu), adresse surveillée pendant 1 h" = robot, not a game, address watched for
  1 hour) and then has only 3 seconds to register; a real game from that address can still play.
  When a player opens "4. Online Multiplayer", their game tests the server (line "test du
  serveur par un jeu" = server test by a game, a fraction of a second) and shows "En ligne -
  ping ..." (online) or "Hors ligne" (offline).

  Starting automatically with Windows (until there is a "Windows service" version):
  Task Scheduler > Create Task > Trigger "At startup", Action "Start a program" =
  scr_relay.exe, with "Run whether user is logged on or not".


2. Linux server (VPS or a machine at home)
------------------------------------------

  Any small Linux server reachable from the Internet will do: an entry-level virtual server
  (VPS) at a few euros a month (OVH, Scaleway, Hetzner, Ionos...), a Raspberry Pi, or a free
  offer. A server in the players' country gives the best ping.

  The folder contains:
    scr_relay            the program, already compiled for Linux x86-64 (no dependencies)
    scr_relay.exe        the same for Windows
    scr_relay.cpp        its source code (the same for Windows and Linux)
    scr_relay.ini        settings imposed on the players (physics, network rate) and port
    scr-relay.service    automatic start at boot (systemd)

  On the server (as root):

    # copy the files (from your PC):
    #   scp scr_relay scr-relay.service root@SERVER_ADDRESS:/root/

    install -m 755 scr_relay /usr/local/bin/scr_relay
    install -m 644 scr_relay.ini /usr/local/bin/scr_relay.ini   # settings (read next to the program)
    useradd --system --no-create-home scrrelay
    install -m 644 scr-relay.service /etc/systemd/system/scr-relay.service
    systemctl daemon-reload
    systemctl enable --now scr-relay

    # the server's firewall, if there is one (example with ufw):
    ufw allow 27420/tcp

  Check:  systemctl status scr-relay      log:  journalctl -u scr-relay -f
  Live display: just run ./scr_relay in a terminal (ssh); as a service, the program writes a
  log (arrivals, races, and the number of players every 5 minutes).

  Other processor (ARM, Raspberry Pi...) or recompiling:
    g++ -O2 -std=c++17 scr_relay.cpp -o scr_relay

  Different port:  scr_relay --port 30000   (to be copied into the players' scr.ini)


3. On the players' side
-----------------------

  In scr.ini, next to scr.exe:

    relais = SERVER_ADDRESS:27420

  (IP address or domain name). Then in the game: "4. Online Multiplayer", "Premier joueur
  disponible" (first available player) or "Salle privée (code)" (private room), the same code
  for all players, Enter.


4. How it works and limits
--------------------------

  - Automatic: the first player waits (one hour at most without activity), the next one is
    linked to them.
  - Private room: as many players as you like; each sees the free players in the room and picks
    one. Once racing, both players disappear from the list.
  - Multi-car race: in a private room, 2 to 6 players; the server gives the start when everyone
    is ready (30 s at most), relays the positions and collects the times; the race ends when
    everyone has finished or is out (car wrecked), or two minutes after the first finisher.
    (Server v34 or later.)
    TNT tracks (8 to 15) offered by the organiser: server v46 or later.
  - Both players must have the same version of scr.exe (otherwise the server refuses).
  - Updates: the server has the same version number as the game and announces it to the games
    that test it (at start-up and in the "4. Online Multiplayer" menu, game v49 or later); an
    older game shows "Nouvelle version vXX disponible" (new version available). Update the server
    at the same time as the game you give to the players.
  - A connection silent for more than 90 s is closed; the game must register within 8 s; at most
    16 connections per IP address, 512 in total.
  - The server only sees game bytes: no account, no personal data.

5. Security (port open on the router)
-------------------------------------

  Opening a port is not a security hole in itself: the risk lies in the program that listens.
  This server only relays game frames; it runs nothing, opens no file at a player's request and
  gives no access to the machine.
  Protections: 16 connections per IP address, 512 in total; at most 1 MB waiting per player in
  each direction (beyond that, the player is disconnected: neither flooding nor a client that no
  longer reads can fill the memory); registration within 8 s (robots dropped silently, see
  above); names and messages reduced to valid UTF-8 without control characters (no escape
  sequences in the console); bounded values (track, laps, times, 6 players per race). Tested with
  random frames, booby-trapped names and floods, under AddressSanitizer: no errors.
  What remains: someone malicious can overload the server (it spoils the game, but gives no
  access to the machine), and the players know your public IP address.
  Advice:
  - router: forward only this port, in TCP, to the server machine (never a "DMZ");
  - run the server without administrator rights (on Linux, scr-relay.service does so with a
    dedicated account and systemd protections); Windows firewall: "private networks" is enough;
  - close the port forwarding when you are not playing, or host the server on a small VPS.

6. Behind a web reverse proxy (HTTPS, for example a Synology NAS)
-----------------------------------------------------------------

  A web reverse proxy only forwards HTTP/HTTPS: the game goes through it as a secure WebSocket
  (address wss://... on the game side; server v73 and game v73 or later). The relay server
  accepts these connections on its usual port (27420), in addition to direct connections.
  Example with a Synology NAS reverse proxy (Control Panel > Login Portal > Advanced > Reverse
  Proxy > Create):
    - Source: protocol HTTPS, hostname = your domain name, port 443;
      a valid certificate for that name (Control Panel > Security > Certificate);
    - Destination: protocol HTTP, hostname = address of the relay server PC (for example
      192.168.1.110), port 27420;
    - "Custom Header" tab: "Create" button > "WebSocket" (adds the Upgrade and Connection
      headers): ESSENTIAL, without it the connection is refused;
    - Save. On your router, port 443 must reach the NAS.
  On the players' side: relais = wss://your.domain (port 443 by default; wss://domain:port/path
  also works). The connection is encrypted by Windows (WinHTTP) with its certificates.
  The server sees the players with their real IP address (X-Real-IP / X-Forwarded-For headers
  added by the proxy, only read when the connection comes from the local network); it sends a
  keep-alive frame every 20 s, and so does the game, so the proxy does not cut an idle link (60 s
  timeout by default on Synology). Ordinary web requests (robots) get a "404" answer and are
  treated like the other robots.
  If something goes wrong: the game's "Hors ligne : ..." (offline) message gives the reason (proxy
  HTTP answer, certificate...); the game also writes each step to scr_reseau.log (game folder), and
  the server window shows "connexion WebSocket" and "test du serveur par WebSocket".
  Status web page: https://your.domain/ in a browser (through the proxy, or http://address:27420/
  directly) shows live (refreshed every 5 s) the games in progress (track, laps, each player's
  state), the connected players (mode, room, state, time) and the open private rooms with their
  players; the same information as JSON: /status.json. No IP address is shown (note: private room
  codes are visible there).

7. Administration page (password)
  Add a line "admin = your-password" to scr_relay.ini and restart the server:
  https://your.domain/admin (or http://address:27420/admin) asks for this password (the user name
  does not matter) and shows, refreshed every 5 s:
  - the browser version's licence codes ("licence = NAME FINGERPRINT" lines): used or not yet,
    how many times, when and from which address;
  - the connected players with their address, version, room or race and what they are doing;
  - the refused bots and the blocked addresses;
  - the last 200 log lines.
  3 wrong passwords block the address for 1 h. Without an "admin" line, the page does not exist.
