(English version below / version anglaise plus bas)

Stunt Car Racer (Atari ST, Geoff Crammond / Micro Style 1989) - version native PC

Aucune donnée du jeu n'est fournie : il faut votre propre image disque .st.

Lancement : placez votre image .st dans le dossier DISK et double-cliquez sur scr.exe
(ou glissez l'image sur scr.exe, ou : scr.exe Stunt_Car_Racer.st --track 1).

Commandes : flèches = joystick (haut accélère, bas freine, gauche/droite),
Espace, Ctrl, Maj ou Alt = bouton (boost), clavier ST complet pour les menus.
Échap remonte les menus : retour au menu précédent (choix « Cancel », menus d'entraînement,
saisie du nom, menu des pilotes du multijoueur) ; menu « Hall of Fame / Practise / Start the
Racing Season » : retour au titre ; titre et menu de départ : Échap deux fois = quitter le jeu.
En course : abandon (comme l'original) ; entraînement lancé directement : retour au titre ;
course à plusieurs : abandon, puis menu du groupe.
Si la direction semble bloquée pendant le boost avec Espace, c'est le clavier qui ne lit
pas trois touches à la fois : prenez Maj ou Alt pour le boost.
Manette XInput reconnue. F8 mode HD (1920x1080, 16/9, image fluide à la
fréquence de l'écran), F2 recharger les images HD, F3 couper le son,
F4, F11 ou Alt+Entrée plein écran, F5 détails élevés (sols, route et voiture
adverse texturés : moteur V8 et pare-brise sur le dessus, roues rondes ; active le mode HD), F6 recommencer,
Page préc./suiv. circuit précédent/suivant (mode entraînement), F7 circuits d'origine / TNT,
F1 menu d'options, F12 quitter.

Menu d'options (F1, ou bouton Start de la manette) : son et volume, mode HD, détails élevés,
textures HD (images du dossier hd), ciel, décor et sa densité, encouragements de la foule et leur volume, ombres des voitures, étincelles et fumée, lissage des bords, plein écran, circuits d'origine /
TNT, manette (et vibrations lors des chocs), touches de conduite (accélérer, freiner, gauche,
droite, boost : Entrée puis la nouvelle touche ; les flèches restent actives pour les menus).
Langue (dernière ligne du menu) : les menus et messages ajoutés au jeu sont en anglais par défaut,
en français au choix (langue = fr dans scr.ini) ; les écrans du jeu d'origine restent en anglais.
Chaque changement est enregistré aussitôt dans scr.ini. Le jeu est en pause pendant ce menu
(sauf en multijoueur). Flèches + Entrée (ou gauche/droite) pour changer, Échap pour fermer.

Mode HD : ciel dégradé avec soleil et nuages qui dérivent lentement, décor autour des circuits
(tribunes garnies de public, panneaux publicitaires, sapins et feuillus, voitures garées sur
les parkings derrière les tribunes ; « Densité du décor » de 1 à 4 dans le menu d'options pour
en mettre plus), ombres sous les voitures, gerbes d'étincelles et fumée lors des chocs et des
réceptions brutales (les vôtres et celles des autres joueurs en course à plusieurs), bords
lissés (anticrénelage). Chaque effet se coupe dans le menu d'options. La voiture adverse suit
pendant les sauts la trajectoire calculée par le jeu d'origine (elle reste en l'air jusqu'à
l'atterrissage) et fait des étincelles quand elle retombe brutalement ; ses suspensions
encaissent les réceptions (la caisse s'enfonce puis remonte, les roues ne passent jamais sous
la route). Après une grosse chute hors piste, la caméra ne descend plus sous le sol.

Rendu 3D (menu d'options, « Rendu 3D » ; rendu = opengl ou processeur dans scr.ini) : en mode HD,
la 3D (route, décor, voitures, ciel, ombres, lissage) est dessinée par la carte graphique avec
OpenGL 3.3, ce qui soulage le processeur ; le cockpit et les images HD sont posés ensuite comme
avant. Si la carte graphique ne gère pas OpenGL 3.3 (pilote ancien), le jeu dessine tout seul sur le
processeur, comme les versions précédentes. Le titre de la fenêtre indique « OpenGL » quand la carte
graphique est utilisée.

Son du moteur (menu d'options, « Son du moteur ») : « d'origine » ou « V8 réaliste ». Le V8 est
synthétisé par le jeu : vilebrequin croisé (le « glouglou » irrégulier d'un V8 américain),
ralenti grave et coupleux de gros arbre à cames (régime qui « pompe », explosions inégales),
échappements résonants, régime qui suit directement celui du jeu (sans passage de vitesse),
grondement plus fort à l'accélération et au boost, moteur qui s'emballe roues en l'air. Seul le
son du moteur d'origine est remplacé, les autres bruitages du jeu restent.

Klaxon (touche K, réglable dans le menu d'options ; bouton Y de la manette) : en multijoueur
(course à deux, course à plusieurs, Computer Link), les autres joueurs l'entendent aussi, plus
ou moins fort selon la distance de votre voiture. Seul, vous êtes le seul à l'entendre.

Encouragements de la foule (mode HD, avec le décor) : chaque tribune a son public qui crie et
applaudit ; on l'entend de plus en plus fort en s'approchant, avec l'effet Doppler (plus aigu
en arrivant, plus grave en s'éloignant). Son fourni par défaut (synthétisé) ; pour utiliser
votre propre enregistrement, placez un fichier foule.wav dans le dossier hd (voir README_HD.txt).

Mises à jour : au démarrage, le jeu interroge le serveur online ; si une version plus récente
est sortie, un bandeau le signale (« Nouvelle version vXX disponible »), ainsi que le menu
« 4. Online Multiplayer ».

Multijoueur (à deux, en réseau) : premier menu du jeu, « 3. Computer Link - Local LAN » (même
réseau : les adversaires trouvés sont proposés, ou une adresse IP) ou « 4. Online Multiplayer »
(par Internet, via le serveur relais de scr.ini, dont la disponibilité est testée : duel, ou
course à plusieurs de 2 à 6 joueurs dans une salle privée).
F9 discuter avec les autres joueurs, F10 coupe la connexion. Voir README_MULTIPLAYER.txt.

Circuits TNT : les 8 circuits de « Stunt Car Racer TNT » (The New Tracks, version Amiga :
DIZZY DESCENT, WITTY WAY, CRAZY CAPER, AMAZING ADEPT, JERKILY JUMP, EVILLY EPISODE, TEASING
TEMPER, RAT RACE). Aucune donnée n'est fournie : placez votre fichier SCR-TNT, ou directement
l'archive WHDLoad StuntCarRacerTNT_v1.4.lha qui le contient, dans le dossier TNT à côté de
scr.exe (ou indiquez-le dans scr.ini : tnt = chemin). Ensuite :
  - touche F7 (ou menu d'options) : circuits d'origine <-> circuits TNT, en direct, SANS
    relancer le jeu : les menus du jeu (entraînement, championnat) proposent aussitôt l'autre
    série, la saison en cours est conservée. Pendant une course, le changement se fait à la fin
    de la course. Le choix est enregistré dans scr.ini (circuits = tnt ; aussi scr.exe
    --circuits tnt). L'écran des divisions garde les noms d'origine (image du jeu) ;
  - en entraînement, Page préc./suiv. passent par les 16 circuits (8 d'origine puis 8 TNT) ;
  - en course à plusieurs, l'organisateur choisit parmi 16 circuits : les 8 d'origine puis les
    8 TNT (marqués « (TNT) »). Un joueur qui n'a pas le fichier est prévenu et ne court pas.

Son : puce sonore YM2149 de l'Atari ST émulée (moteur, chocs...).

Réglages : scr.ini (gravité, poussée, freinage, pas de temps, amortissement,
consommation du boost, taille de fenêtre...). Voir scr --help. Le jeu est livré avec un modèle,
scr.ini.exemple : au premier lancement, scr.ini est créé à partir de lui. Une nouvelle version
décompressée par-dessus l'ancienne ne touche donc plus à votre scr.ini (adresse du serveur
relais, touches, options).

Images HD : le dossier hd contient des images qui remplacent les sprites du cockpit (et le
cockpit fixe : cockpit.png) en mode HD ; sans image, les flammes du boost sont les sprites d'origine agrandis en HD. Voir hd/README_HD.txt.


==============================================================================
ENGLISH
==============================================================================

Stunt Car Racer (Atari ST, Geoff Crammond / Micro Style 1989) - native PC version

No game data is supplied: you need your own .st disk image.

Starting: put your .st image in the DISK folder and double-click scr.exe
(or drag the image onto scr.exe, or: scr.exe Stunt_Car_Racer.st --track 1).

Controls: arrow keys = joystick (up accelerates, down brakes, left/right),
Space, Ctrl, Shift or Alt = fire button (boost), full ST keyboard for the menus.
Escape goes back up the menus: back to the previous menu ("Cancel" choice, practice menus,
name entry, multiplayer driver menu); "Hall of Fame / Practise / Start the Racing Season"
menu: back to the title; title and start menu: Escape twice = quit the game.
During a race: give up (as in the original); practice started directly: back to the title;
multi-car race: give up, then the group menu.
If steering seems stuck while boosting with Space, your keyboard cannot read three keys at
once: use Shift or Alt for the boost.
XInput gamepads are supported. F8 HD mode (1920x1080, 16:9, smooth picture at the screen's
refresh rate), F2 reload the HD images, F3 mute,
F4, F11 or Alt+Enter full screen, F5 high detail (textured ground, road and opponent car:
V8 engine and windscreen on top, round wheels; turns HD mode on), F6 restart,
Page Up/Down previous/next track (practice mode), F7 original / TNT tracks,
F1 options menu, F12 quit.

Options menu (F1, or the gamepad's Start button): sound and volume, HD mode, high detail,
HD textures (images from the hd folder), sky, scenery and its density, crowd cheering and its
volume, car shadows, sparks and smoke, edge smoothing, full screen, original / TNT tracks,
gamepad (and rumble on impacts), driving keys (accelerate, brake, left, right, boost: Enter
then the new key; the arrow keys always work in the menus).
Every change is saved immediately in scr.ini. The game is paused while this menu is open
(except in multiplayer). Arrows + Enter (or left/right) to change, Escape to close.
The menus and messages added to the game are in English by default; the last line of this menu
switches them to French (langue = fr in scr.ini). The original game's screens stay in English.

HD mode: gradient sky with a sun and slowly drifting clouds, scenery around the tracks
(grandstands full of spectators, advertising boards, fir and leafy trees, cars parked behind
the grandstands; "Densité du décor" (scenery density) 1 to 4 in the options menu for more),
shadows under the cars, showers of sparks and smoke on impacts and hard landings (yours and
the other players' in multi-car races), smoothed edges (anti-aliasing). Each effect can be
turned off in the options menu. During jumps the opponent car follows the trajectory computed
by the original game (it stays in the air until it lands) and throws sparks when it lands
hard; its suspension absorbs the landings (the body dips then rises, the wheels never go
through the road). After a big fall off the track, the camera no longer goes below the ground.

3D renderer (options menu, "Rendu 3D"; rendu = opengl or processeur in scr.ini): in HD mode the
3D (road, scenery, cars, sky, shadows, smoothing) is drawn by the graphics card with OpenGL 3.3,
which takes load off the processor; the cockpit and the HD images are then laid on top as
before. If the graphics card does not support OpenGL 3.3 (old driver), the game automatically
draws everything on the processor, like the previous versions. The window title shows "OpenGL"
when the graphics card is used.

Engine sound (options menu, "Son du moteur"): "d'origine" (original) or "V8 réaliste" (realistic
V8). The V8 is synthesised by the game: cross-plane crankshaft (the irregular burble of an
American V8), deep, torquey big-cam idle (hunting revs, uneven firing), resonant exhausts, revs
that follow the game's directly (no gear changes), louder roar when accelerating and boosting,
engine that races with the wheels in the air. Only the original engine sound is replaced; the
game's other sound effects remain.

Horn (K key, can be changed in the options menu; gamepad Y button): in multiplayer (two-player
race, multi-car race, Computer Link) the other players hear it too, louder or quieter depending
on how close your car is. Alone, only you hear it.

Crowd cheering (HD mode, with scenery): each grandstand has its own crowd that shouts and
claps; it gets louder as you approach, with the Doppler effect (higher pitched as you arrive,
lower as you drive away). A synthesised sound is supplied; to use your own recording, put a
foule.wav file in the hd folder (see README_HD.txt).

Updates: at start-up the game asks the online server; if a newer version is out, a banner says
so ("Nouvelle version vXX disponible" = new version vXX available), as does the
"4. Online Multiplayer" menu.

Multiplayer (two players, over a network): first game menu, "3. Computer Link - Local LAN" (same
network: opponents found are listed, or type an IP address) or "4. Online Multiplayer" (over the
Internet, through the relay server, whose availability is tested: one-to-one race, or a
multi-car race of 2 to 6 players in a private room).
F9 chat with the other players, F10 disconnects. See README_MULTIPLAYER.txt.

TNT tracks: the 8 tracks of "Stunt Car Racer TNT" (The New Tracks, Amiga version: DIZZY DESCENT,
WITTY WAY, CRAZY CAPER, AMAZING ADEPT, JERKILY JUMP, EVILLY EPISODE, TEASING TEMPER, RAT RACE).
No data is supplied: put your SCR-TNT file, or directly the WHDLoad archive
StuntCarRacerTNT_v1.4.lha that contains it, in the TNT folder next to scr.exe (or give its path
in scr.ini: tnt = path). Then:
  - F7 key (or options menu): original tracks <-> TNT tracks, live, WITHOUT restarting the game:
    the game's menus (practice, league) immediately offer the other set and the current season
    is kept. During a race the switch happens at the end of the race. The choice is saved in
    scr.ini (circuits = tnt; also scr.exe --circuits tnt). The divisions screen keeps the
    original names (it is a picture from the game);
  - in practice, Page Up/Down go through the 16 tracks (8 original then 8 TNT);
  - in a multi-car race the organiser chooses among 16 tracks: the 8 original then the 8 TNT
    (marked "(TNT)"). A player who does not have the file is told and does not race.

Sound: the Atari ST's YM2149 sound chip is emulated (engine, impacts...).

Settings: scr.ini (gravity, thrust, braking, time step, damping, boost use, window size...).
See scr --help. The game comes with a template, scr.ini.exemple: on first start, scr.ini is
created from it. A new version unzipped over the old one therefore never touches your scr.ini
(relay server address, keys, options).

HD images: the hd folder holds images that replace the cockpit sprites (and the fixed cockpit:
cockpit.png) in HD mode; without images, the boost flames are the original sprites enlarged in
HD. See hd/README_HD.txt.
