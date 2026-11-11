(English version below / version anglaise plus bas)

Stunt Car Racer - jouer à plusieurs en réseau (réseau local et en ligne)
=================================================================

Le jeu d'origine permettait de relier deux Atari ST par un câble série pour faire la course l'un
contre l'autre. Ici, ce câble passe par le réseau : chaque joueur lance scr.exe sur son PC et
retrouve l'autre joueur comme adversaire sur la piste.

Il faut que les deux joueurs aient la même version de scr.exe et la même image disque (c'est
vérifié à la connexion). Les réglages de physique sont uniformisés automatiquement : en ligne,
ceux du serveur ; en réseau local, ceux du joueur qui a choisi l'adversaire.
Le serveur online annonce la dernière version du jeu : si la vôtre est plus ancienne, le jeu
l'affiche au démarrage et dans le menu « 4. Online Multiplayer » (« Nouvelle version vXX
disponible »).


1. Se relier et lancer la course
--------------------------------

  Le premier menu du jeu propose :

    1. Single Player League
    2. Multiplayer - Local            plusieurs pilotes à tour de rôle sur ce PC (jeu d'origine)
    3. Computer Link - Local LAN      deux PC sur le même réseau (maison)
    4. Online Multiplayer             deux PC par Internet, par le serveur relais

  Flèches + Entrée (ou la manette) dans les menus, Échap pour annuler.

  3. Computer Link - Local LAN
     Chaque jeu qui attend dans ce menu apparaît chez l'autre : l'un des deux joueurs choisit
     « Affronter ... » ; les deux jeux se relient et la mise en relation du jeu d'origine démarre
     toute seule (« Link complete »). Au premier lancement, Windows demande d'autoriser scr.exe
     sur le réseau : acceptez.
     Si l'autre joueur n'apparaît pas :
       * les deux jeux doivent être dans ce menu en même temps (la recherche ne tourne que là) ;
       * pare-feu Windows : autorisez scr.exe (Sécurité Windows > Pare-feu > Autoriser une
         application) pour les réseaux privés ET publics, ou passez le réseau de la box en
         « Privé » ; faites-le sur les DEUX PC ;
       * les deux PC doivent être sur la même box (pas un réseau « invité » ni un Wi-Fi isolé) ;
       * sinon : la ligne « Ce PC : 192.168.x.y » du menu donne l'adresse de chaque PC ; l'un
         choisit « Par adresse IP » et tape l'adresse affichée chez l'autre.

  4. Online Multiplayer
     Par défaut, le jeu se connecte au serveur officiel (« Serveur : serveur officiel » dans le
     menu) : rien à régler. Pour jouer sur un autre serveur relais, au choix (prioritaire sur le
     serveur officiel) :
       * dans le menu : « Changer l'adresse du serveur », tapez par exemple 192.168.1.110 puis
         Entrée (port 27420 par défaut) : elle est enregistrée dans scr.ini ; une adresse vide
         ramène au serveur officiel ;
       * ou dans scr.ini, à côté de scr.exe :   relais = 192.168.1.110:27420
         ATTENTION : pas de # au début de la ligne (une ligne qui commence par # est ignorée) ;
       * ou au lancement : scr.exe --relais 192.168.1.110:27420 (prioritaire sur scr.ini).
     En ouvrant ce menu, le jeu teste le serveur et affiche :
       « En ligne - ping 25 ms - 3 joueurs »   (vert)  : le serveur répond ;
       « Hors ligne : ... »                    (rouge) : raison (serveur arrêté, adresse
                                                          introuvable, pas de réponse...).
     « Tester à nouveau le serveur » refait le test.
     « État du serveur (parties, joueurs, salles) » affiche, mis à jour toutes les 5 s, les parties
     en cours (circuit, tours, joueurs), les joueurs connectés et leur état, et les salles privées
     ouvertes ; flèches gauche / droite pour faire défiler, « Retour » ou Échap pour revenir
     (serveur v80 ou plus récent ; la même chose en page web : README_SERVER.txt, section 6).
     Pseudo : « Pseudo : ... (changer) » ; chaque joueur saisit le sien (12 caractères, enregistré
     dans scr.ini, ligne nom =) : c'est le nom affiché au-dessus de sa voiture et au classement.
     Quand le serveur répond :
       * « Premier joueur disponible » : le serveur vous met en COURSE À DEUX avec le premier
         joueur qui attend (ou vous fait attendre le suivant). Pas de Computer Link : chacun mène
         son propre jeu, sans attendre que l'autre valide ses menus. Un circuit est tiré au sort
         (parmi les 8 d'origine, ou les 16 avec les circuits TNT si les deux joueurs les ont) et
         proposé aux deux : « Accepter ce circuit » ; la course part dès que les deux ont
         accepté. « Autre circuit » refuse : un autre circuit est proposé, à accepter de nouveau
         par les deux. Grille : un à gauche, un à droite, la grue amène chaque voiture de son côté
         (comme la course à plusieurs ci-dessous : flammes du boost, chocs, classement en direct).
         Circuits tirés sans répétition : chacun est proposé une fois avant qu'un circuit
         revienne. Discussion pendant la course à deux : F9 ou « Écrire un message ».
         Après l'arrivée, un nouveau circuit est proposé pour la revanche ; « Quitter la course »
         (ou Échap) vous déconnecte, et l'autre joueur attend un nouvel adversaire ;
       * « Salle publique » : la salle ouverte à tous les joueurs du serveur (même salle que le code
         PUBLIC), avec les mêmes possibilités qu'une salle privée. Quand une course y est lancée, les
         joueurs de la salle qui n'y participent pas deviennent spectateurs (voir plus bas) ;
       * « Salle privée (code) » : tapez un code (lettres ou chiffres ; vide = le jeu en invente
         un, à donner à vos amis). Le menu affiche les joueurs présents dans la salle :
         « Affronter ... » pour en choisir un : même course à deux (circuit proposé, accepté par
         les deux). Plusieurs joueurs peuvent attendre dans la même salle ; ceux qui sont déjà en
         partie n'y apparaissent plus.
     Course à plusieurs (2 à 6 joueurs) : dans la salle privée, « Course à plusieurs (2 à 6
     joueurs) ». Le premier entré est l'organisateur : il choisit le circuit et le nombre de tours
     (flèches gauche / droite ; avec les circuits TNT, 16 au choix : les 8 d'origine puis les 8 TNT,
     que chaque joueur doit avoir, cf. README.txt), puis « Lancer la course » dès que deux joueurs au moins sont là.
     Chaque jeu se place sur le circuit et attend que tous soient prêts : le départ est donné en
     même temps à tous. Grille de départ : 3 rangées de 2 (gauche / droite), ordre tiré au sort
     à chaque course ; les rangées 2 et 3 partent un peu en arrière (le temps et le classement
     comptent depuis la ligne de départ). La grue amène chaque voiture de son côté : une place à
     gauche arrive par la gauche de la route, une place à droite par la droite (la trajectoire de
     la grue d'origine, en miroir pour la gauche) ; aucune voiture ne traverse les autres. On voit rouler les autres joueurs
     (avec leur nom au-dessus, et les 8 flammes du moteur V8, en V et en dégradé jaune, orange et
     rouge, quand ils utilisent le boost). Les voitures se heurtent : chacun est repoussé hors de
     l'autre voiture, les vitesses se partagent au choc (on peut pousser une voiture ou être
     poussé), et un choc violent abîme la voiture comme une mauvaise réception. Chaque jeu calcule
     le choc pour sa voiture avec la position reçue des autres : avec du lag, le contact peut
     paraître un peu décalé. Le jeu passe en HD (F8)
     au départ, car le rendu d'origine ne sait dessiner qu'une seule voiture adverse.
     En haut à droite : tour, temps et classement en direct. Le nombre de tours est celui de
     l'organisateur (1 à 9 ; l'arrêt de l'entraînement d'origine après 3 tours est désactivé).
     Spectateurs de la salle : quand une course est lancée dans une salle (salle publique ou privée), les
     joueurs de la salle qui n'y participent pas la regardent aussitôt depuis les tribunes, avec le nom
     et le tour de chaque joueur ; flèches gauche / droite : autre voiture ; Entrée : menu de la salle,
     où « Regarder la course (tribunes) » ramène dans les tribunes (serveur v82 ou plus récent).
     Spectateur : un joueur éliminé (voiture détruite), ou déjà arrivé, regarde la suite de la course
     depuis les tribunes du décor (mode HD, avec le décor) : la caméra se place dans la tribune la plus
     proche de la voiture suivie et la suit des yeux, en zoomant quand elle s'éloigne. Flèches gauche /
     droite : autre voiture ; Entrée : menu du groupe (classement en direct), où « Regarder la course
     (tribunes) » ramène dans les tribunes.
     Si tous les autres joueurs sont éliminés (voiture détruite) ou partis, le dernier encore en
     course gagne : « VICTOIRE ! » reste affiché jusqu'à ce qu'il appuie sur Entrée (ou le bouton A
     de la manette), puis le menu de la course s'ouvre.
     Dès qu'un joueur a franchi l'arrivée, le menu du groupe s'ouvre chez lui (classement en
     direct). Voiture détruite (« wrecked ») : le joueur abandonne, le menu du groupe s'ouvre chez
     lui et « abandon » s'affiche à côté de son nom. À la fin (tous arrivés ou abandonnés, ou deux
     minutes après le premier arrivé), le menu affiche les
     temps et l'organisateur peut relancer une course. « Quitter la course à plusieurs » (ou Échap) ramène au titre du jeu.
     Klaxon : touche K (ou bouton Y de la manette) pour narguer les autres : ils l'entendent,
     plus fort quand votre voiture est proche (aussi en Computer Link).
     Discussion : dans la salle privée, « Écrire un message (F9) » (ou la touche F9) : le message va
     à tous les joueurs présents dans la salle ; les derniers s'affichent dans le menu. Une fois la
     partie lancée, F9 écrit à l'adversaire (aussi en réseau local) ; ses messages apparaissent en
     bas de l'image pendant 10 secondes. Entrée envoie, Échap ferme la saisie.
     Aucun port à ouvrir sur les box des joueurs : les PC appellent le serveur relais.
     Réglages : en ligne, c'est le serveur qui impose la physique et la cadence réseau à tous les
     joueurs (fichier scr_relay.ini du serveur) ; le menu les affiche (« Réglages du serveur »).
     En réseau local (menu 3), le jeu qui rejoint prend les réglages de celui qui l'a choisi.
     Par adresse IP (sans serveur) : menu 3, « Par adresse IP ».

  Computer Link en réseau local (menu 3) : celui qui a choisi l'adversaire est le « maître » : il saisit
  les pilotes (au moins deux : les deux joueurs) avec « 1. Enter another driver », puis
  « 2. Continue », et mène les menus (championnat, entraînement, circuit). L'autre affiche
  « Please wait » puis suit les mêmes écrans. En course, chacun pilote sa voiture et voit l'autre
  comme adversaire.

  F10 coupe la connexion. Un bandeau en haut de l'image indique l'état (relié, ping, ou la raison
  d'un échec).

  En ligne de commande (sans passer par le menu) :
    scr.exe --online auto                 serveur relais, premier joueur disponible
    scr.exe --online CODE                 serveur relais, salle privée (relais = dans scr.ini)
    scr.exe --join ADRESSE[:PORT]         adresse d'un autre joueur qui attend (--host)
    scr.exe --host / --joinlan            héberger / rejoindre en réseau local


2. Ping et vitesse du jeu
-------------------------

  Le jeu d'origine attend l'autre machine à chaque étape de la course. Avec un ping (aller-retour)
  jusqu'à environ 50 ms - le cas normal entre joueurs en France - la vitesse est normale. Au-delà,
  le jeu ralentit un peu (environ 87 % à 80 ms, 76 % à 120 ms) mais ne se désynchronise pas.

  Le mode HD (F8) et les détails élevés (F5) fonctionnent aussi en multijoueur.

  Cadence des échanges (scr.ini) :

    reseau_cadence = 400

  C'est le nombre de fois par seconde où le jeu lit et envoie les données de la course (positions,
  joystick...) : chaque trame du jeu est émulée en petites tranches réparties dans le temps, et le
  réseau est traité à chaque tranche. Plus la valeur est haute, plus une donnée reçue est prise en
  compte vite (moins de « lag ») ; 400 = toutes les 2,5 ms. De 50 (une fois par image, comme avant)
  à 2000. En cas de saccades : essayez 800 ou 1000 ; sur un PC lent, 200.
  Le jeu d'origine échange ses données à chaque étape de la course et attend l'autre machine : la
  cadence réduit l'attente ajoutée par le PC, mais pas le ping lui-même (distance, Wi-Fi, box).
  Conseils contre le lag : câble Ethernet plutôt que Wi-Fi, serveur relais proche des joueurs,
  mode d'affichage d'origine ou HD sans détails élevés (F5) sur un PC modeste.


3. Le serveur relais
--------------------

  Il n'est utile que pour jouer par Internet. C'est un petit programme autonome, pour Windows
  (scr_relay.exe : double-clic) ou pour un serveur Linux (voir le dossier serveur et
  serveur/README_SERVER.txt). Sa fenêtre affiche en direct les joueurs connectés, leur mode,
  leur salle et les parties en cours. Un petit serveur virtuel à quelques
  euros par mois suffit très largement : le trafic est d'environ 300 octets par seconde par joueur.

  Serveur derrière un reverse proxy web (HTTPS) : relais = wss://domaine (ou
  wss://domaine:port/chemin) ; le jeu passe alors en WebSocket chiffrée (Windows). Voir
  serveur/README_SERVER.txt, section 6.


==============================================================================
ENGLISH
==============================================================================

Stunt Car Racer - playing together over a network (local network and online)
============================================================================

The original game let you link two Atari STs with a serial cable to race against each other.
Here that cable goes through the network: each player runs scr.exe on their own PC and finds
the other player as the opponent on the track.

Both players need the same version of scr.exe and the same disk image (checked when
connecting). Physics settings are made identical automatically: online, the server's; on a
local network, those of the player who chose the opponent.
The online server announces the latest version of the game: if yours is older, the game says so
at start-up and in the "4. Online Multiplayer" menu ("Nouvelle version vXX disponible" = new
version vXX available).
(The game's menus are in English by default - the French labels quoted here are what you see
with the French language chosen in the F1 options menu; the English meaning is given in brackets.)


1. Connecting and starting the race
-----------------------------------

  The game's first menu offers:

    1. Single Player League
    2. Multiplayer - Local            several drivers taking turns on this PC (original game)
    3. Computer Link - Local LAN      two PCs on the same (home) network
    4. Online Multiplayer             two PCs over the Internet, through the relay server

  Arrows + Enter (or the gamepad) in the menus, Escape to cancel.

  3. Computer Link - Local LAN
     Each game waiting in this menu shows up on the other one: one of the two players chooses
     "Affronter ..." (race against ...); the two games link up and the original game's link
     handshake starts by itself ("Link complete"). On first start, Windows asks whether to allow
     scr.exe on the network: accept.
     If the other player does not appear:
       * both games must be in this menu at the same time (the search only runs there);
       * Windows firewall: allow scr.exe (Windows Security > Firewall > Allow an app) for
         private AND public networks, or set your router's network to "Private"; do it on BOTH
         PCs;
       * both PCs must be on the same router (not a "guest" network or an isolated Wi-Fi);
       * otherwise: the line "Ce PC : 192.168.x.y" (this PC) in the menu gives each PC's address;
         one player chooses "Par adresse IP" (by IP address) and types the address shown on the
         other PC.

  4. Online Multiplayer
     By default the game connects to the official server ("Serveur : serveur officiel" in the
     menu): nothing to set up. To play on another relay server, either (this overrides the
     official server):
       * in the menu: "Changer l'adresse du serveur" (change server address), type for example
         192.168.1.110 then Enter (port 27420 by default): it is saved in scr.ini; an empty
         address goes back to the official server;
       * or in scr.ini, next to scr.exe:   relais = 192.168.1.110:27420
         WARNING: no # at the start of the line (a line starting with # is ignored);
       * or on the command line: scr.exe --relais 192.168.1.110:27420 (overrides scr.ini).
     When this menu opens, the game tests the server and shows:
       "En ligne - ping 25 ms - 3 joueurs"   (green) : online, the server answers;
       "Hors ligne : ..."                    (red)   : offline, with the reason (server stopped,
                                                       address not found, no answer...).
     "Tester à nouveau le serveur" (test the server again) repeats the test.
     "Server status (games, players, rooms)" shows, updated every 5 s, the games in progress
     (track, laps, players), the connected players and their state, and the open private rooms;
     left / right arrows scroll, "Back" or Escape returns (server v80 or later; the same as a web
     page: README_SERVER.txt, section 6).
     Nickname: "Pseudo : ... (changer)"; each player enters their own (12 characters, saved in
     scr.ini, line nom =): it is the name shown above their car and in the standings.
     When the server answers:
       * "Premier joueur disponible" (first available player): the server puts you in a
         TWO-PLAYER RACE with the first player waiting (or makes you wait for the next one). No
         Computer Link: each player runs their own game, without waiting for the other to go
         through the menus. A track is drawn at random (among the 8 original ones, or 16 with the
         TNT tracks if both players have them) and offered to both: "Accepter ce circuit"
         (accept this track); the race starts as soon as both have accepted. "Autre circuit"
         (another track) refuses: another track is offered, to be accepted again by both.
         Grid: one on the left, one on the right, the crane brings each car in from its own side
         (as in the multi-car race below: boost flames, collisions, live standings).
         Tracks are drawn without repeats: each one is offered once before a track comes back.
         Chat during the two-player race: F9 or "Écrire un message" (write a message).
         After the finish, a new track is offered for a rematch; "Quitter la course" (leave the
         race) or Escape disconnects you, and the other player waits for a new opponent;
       * "Public room": the room open to every player on the server (the same room as the code
         PUBLIC), with the same options as a private room. When a race is started there, the players
         in the room who are not taking part become spectators (see below);
       * "Salle privée (code)" (private room): type a code (letters or digits; empty = the game
         makes one up, give it to your friends). The menu shows the players in the room:
         "Affronter ..." (race against ...) to pick one: same two-player race (track offered,
         accepted by both). Several players can wait in the same room; those already racing no
         longer appear in it.
     Multi-car race (2 to 6 players): in the private room, "Course à plusieurs (2 à 6 joueurs)".
     The first one in is the organiser: they choose the track and the number of laps (left /
     right arrows; with the TNT tracks, 16 to choose from: the 8 original then the 8 TNT ones,
     which every player must have, see README.txt), then "Lancer la course" (start the race)
     as soon as at least two players are there.
     Each game sets itself up on the track and waits until everyone is ready: the start is given
     to all at the same time. Starting grid: 3 rows of 2 (left / right), order drawn at random for
     each race; rows 2 and 3 start a little further back (time and standings count from the start
     line). The crane brings each car in from its own side: a left-hand place arrives from the
     left of the road, a right-hand place from the right (the original crane's path, mirrored for
     the left); no car goes through the others. You see the other players driving (with their
     name above, and the V8 engine's 8 flames, in a V, shading from yellow to orange and red, when
     they boost). Cars collide: each one is pushed out of the other, speeds are shared in the
     impact (you can push a car or be pushed), and a violent impact damages the car like a bad
     landing. Each game computes the impact for its own car from the position received from the
     others: with lag, the contact can look slightly off. The game switches to HD (F8) at the
     start, because the original renderer can only draw one opponent car.
     Top right: lap, time and live standings. The number of laps is the organiser's (1 to 9; the
     original practice mode's stop after 3 laps is disabled).
     Room spectators: when a race is started in a room (public or private), the players in the room
     who are not taking part watch it at once from the grandstands, with each player's name and lap;
     left / right arrows: another car; Enter: the room menu, where "Watch the race (grandstands)"
     takes you back to the stands (server v82 or later).
     Spectator: a player who is out (car wrecked), or has already finished, watches the rest of the
     race from the grandstands of the scenery (HD mode, with scenery): the camera sits in the
     grandstand closest to the car being followed and follows it, zooming in as it gets further away.
     Left / right arrows: another car; Enter: the group menu (live standings), where "Watch the race
     (grandstands)" takes you back to the stands.
     If all the other players are eliminated (car wrecked) or gone, the last one still racing
     wins: "VICTOIRE !" (victory) stays on screen until they press Enter (or the gamepad's A
     button), then the race menu opens.
     As soon as a player crosses the finish line, the group menu opens for them (live standings).
     Car wrecked: the player is out, the group menu opens for them and "abandon" (retired) is
     shown next to their name. At the end (all finished or out, or two minutes after the first
     finisher), the menu shows the times and the organiser can start another race. "Quitter la
     course à plusieurs" (leave the multi-car race) or Escape goes back to the game's title.
     Horn: K key (or the gamepad's Y button) to taunt the others: they hear it, louder when your
     car is close (also in Computer Link).
     Chat: in the private room, "Écrire un message (F9)" (write a message) or the F9 key: the
     message goes to every player in the room; the latest ones are shown in the menu. Once the
     game has started, F9 writes to the opponent (also on a local network); their messages appear
     at the bottom of the picture for 10 seconds. Enter sends, Escape closes the input.
     No port to open on the players' routers: the PCs call the relay server.
     Settings: online, the server imposes the physics and the network rate on all players (the
     server's scr_relay.ini file); the menu shows them ("Réglages du serveur" = server settings).
     On a local network (menu 3), the joining game takes the settings of the one who chose it.
     By IP address (no server): menu 3, "Par adresse IP".

  Computer Link on a local network (menu 3): the player who chose the opponent is the "master":
  they enter the drivers (at least two: both players) with "1. Enter another driver", then
  "2. Continue", and run the menus (league, practice, track). The other one shows "Please wait"
  then follows the same screens. In the race, each player drives their own car and sees the
  other as the opponent.

  F10 disconnects. A banner at the top of the picture shows the state (linked, ping, or the
  reason for a failure).

  On the command line (without going through the menu):
    scr.exe --online auto                 relay server, first available player
    scr.exe --online CODE                 relay server, private room (relais = in scr.ini)
    scr.exe --join ADDRESS[:PORT]         address of another player who is waiting (--host)
    scr.exe --host / --joinlan            host / join on a local network


2. Ping and game speed
----------------------

  The original game waits for the other machine at each step of the race. With a ping (round
  trip) of up to about 50 ms - the normal case between players in the same country - the speed
  is normal. Beyond that, the game slows down a little (about 87 % at 80 ms, 76 % at 120 ms) but
  does not get out of sync.

  HD mode (F8) and high detail (F5) also work in multiplayer.

  Exchange rate (scr.ini):

    reseau_cadence = 400

  This is how many times per second the game reads and sends race data (positions,
  joystick...): each game frame is emulated in small slices spread over time, and the network is
  handled at each slice. The higher the value, the sooner received data is taken into account
  (less "lag"); 400 = every 2.5 ms. From 50 (once per frame, as before) to 2000. If the game
  stutters: try 800 or 1000; on a slow PC, 200.
  The original game exchanges its data at each step of the race and waits for the other machine:
  the rate reduces the waiting added by the PC, but not the ping itself (distance, Wi-Fi, router).
  Tips against lag: Ethernet cable rather than Wi-Fi, relay server close to the players, original
  display mode or HD without high detail (F5) on a modest PC.


3. The relay server
-------------------

  It is only needed to play over the Internet. It is a small stand-alone program, for Windows
  (scr_relay.exe: double-click) or for a Linux server (see the serveur folder and
  serveur/README_SERVER.txt). Its window shows live the connected players, their mode, their
  room and the races in progress. A small virtual server at a few euros a month is more than
  enough: the traffic is about 300 bytes per second per player.

  Server behind a web reverse proxy (HTTPS): relais = wss://domain (or wss://domain:port/path);
  the game then uses an encrypted WebSocket (Windows). See serveur/README_SERVER.txt,
  section 6.
