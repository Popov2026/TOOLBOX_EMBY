# Stunt Car Racer — rétro-ingénierie et réplique PC paramétrable

Projet de décompilation de **Stunt Car Racer** (Geoff Crammond, Micro Style, 1989),
version **Atari ST**, et réplique jouable sur PC dont tous les réglages sont modifiables.

> **Aucune donnée du jeu n'est incluse dans ce dépôt.** Les outils et la réplique lisent
> les circuits directement depuis **votre** image disque (`.st`). Sans disquette, la
> réplique propose des circuits de démonstration originaux.

## Contenu

| Dossier | Contenu |
|---|---|
| `native/` | **version native PC** (C++17 / SDL2) : le jeu d'origine exécuté à pleine vitesse, réglable |
| `replica/` | la réplique (HTML5 / JavaScript, sans dépendance) |
| `tools/` | outils de rétro-ingénierie : extraction, décompression, décodage des circuits, émulateur ST de test, désassembleur |
| `docs/RETRO_INGENIERIE.md` | formats, adresses, routines décompilées, méthode de vérification |

## Deux moteurs

* **Original (code du jeu)** : le jeu d'origine complet — menus, course, rendu, tableau de
  bord — exécuté par un **CPU 68000 écrit en JavaScript** (`replica/js/cpu68k.js`) à partir
  du programme lu sur votre disquette, avec un TOS minimal simulé. Fidélité totale : validé
  instruction par instruction contre l'émulateur Musashi. Le clavier et le joystick (flèches,
  espace, manette) sont injectés directement dans la mémoire du jeu. Bouton **Jeu complet**
  pour partir de l'écran titre, ou choix d'un circuit pour aller directement à l'entraînement.
* **Remake (paramétrable)** : moteur réécrit (rendu 3D à polygones, physique réglable),
  utilisant la géométrie exacte des circuits décodés. Tous les paramètres sont exposés.

## Version native (C++ / SDL2) — recommandée

Le programme du jeu, lu sur votre disquette, est exécuté par le CPU 68000 Musashi (C, MIT)
avec un TOS minimal simulé ; rendu, clavier, joystick et manette via SDL2.
Environ **110 à 120× le temps réel** sur un PC actuel (une partie ne consomme que
quelques % d'un cœur).

**Windows** : l'onglet *Actions* du dépôt (workflow « Stunt Car Racer natif ») produit
l'archive `stunt-car-racer-windows-x64` (`scr.exe`, `SDL2.dll`, `scr.ini`). Placez votre
image `.st` dans le dossier `DISK` à côté de `scr.exe` et double-cliquez.

**Compilation** (Linux, macOS, Windows ; CMake ≥ 3.16, SDL2) :

```sh
cmake -S native -B native/build && cmake --build native/build --config Release
native/build/scr Stunt_Car_Racer.st              # jeu complet (écran titre, menus)
native/build/scr Stunt_Car_Racer.st --track 3    # entraînement direct sur le circuit 3
native/build/scr_headless Stunt_Car_Racer.st --track 1 --frames 1500 --up --shot c.ppm   # test sans fenêtre
```

Commandes : **flèches** = joystick (haut accélère, bas freine), **Espace/Ctrl** = bouton
(boost), clavier ST complet pour les menus, manette reconnue. **F1** mode HD, **F3** son coupé/rétabli, **F11** plein écran,
**F5** recommencer, **Page préc./suiv.** changer de circuit, **F12** quitter.

**Son** : la puce YM2149 de l'Atari ST est émulée (3 voix carrées, bruit, enveloppe, volumes
logarithmiques) ; chaque écriture du jeu dans ses registres est horodatée au cycle près et
appliquée à l'échantillon correspondant (44,1 kHz). Options `son = 0`, `volume = 0.8`.

### Mode HD (touche F1)

**F1** bascule en **1920×1080, 16/9, image fluide** (60 i/s ou la fréquence de l'écran) :

* la logique et la physique restent celles du jeu d'origine, exécutées à l'identique ;
* la scène 3D est redessinée en haute définition par un moteur logiciel multi-cœur
  (tampon de profondeur), à partir de la géométrie exacte des circuits lue sur la disquette
  et de l'état de la voiture lu en mémoire ; elle est **interpolée** entre deux ticks du jeu
  (8,33 par seconde) au lieu de sauter d'une image à la suivante ;
* le champ de vision est élargi au 16/9 : on voit la piste à travers les vitres latérales ;
* la **voiture adverse** est redessinée en 3D : sa position est lue dans le jeu (pièce `$10907`,
  section `$108F6` + fraction `$108F7`, position en travers de la route `$109D6`), orientée
  selon la route (pente, dévers), avec une trajectoire balistique au-dessus des sauts, et
  interpolée comme la caméra ;
* le cockpit, les roues et le tableau de bord sont ceux que dessine le
  jeu d'origine, isolés pixel par pixel (chaque écriture à l'écran est attribuée à la routine
  qui l'a faite) et incrustés par-dessus.

Options : `hd = 1` (démarrer en HD), `hdres = 2560x1440` (autre définition 16/9). Le titre de
la fenêtre affiche le nombre d'images par seconde.

**Images de remplacement** : les PNG du dossier `hd/` (à côté de `scr.exe`) remplacent les
sprites du cockpit en mode HD, au même endroit et au même moment (`sprite_06.png`, ou
`sprite_06_1.png`, `_2`… pour une animation). Sans transparence, l'image est ajoutée en
lumière (flammes sur fond noir). Réglages par sprite dans `hd/hd.ini` (taille, ancrage,
décalage, cadence), **F2** recharge le dossier en jeu. Mode d'emploi : `native/LISEZMOI_HD.txt` ;
flammes d'exemple : `tools/hd_flammes.py`. Le jeu enregistre chaque appel de ses routines
de sprites (`$56762`, `$5687E`) et ce qu'il recouvre, pour effacer proprement l'original.

Réglages dans `scr.ini` (modèle : `native/scr.ini.example`) ou en ligne de commande :
`--gravity --thrust --brake --timestep --damping --boostuse --shock` (facteurs, 1 = jeu
d'origine), `--scale`, `--fullscreen`, `--smooth`, `--speed`.

## Lancer la réplique (navigateur)

1. Ouvrir `replica/index.html` dans un navigateur récent (double-clic suffit).
2. Cliquer sur **Charger la disquette** (ou glisser-déposer) : image `.st`, `GAME.PUT`
   ou `tracks.json` produit par `scr_tool.py`. Les circuits sont mémorisés dans le navigateur.
3. Choisir le circuit, appuyer sur **Entrée** (ou *Départ*).

Commandes : **↑/W** accélérer, **↓/S** freiner / marche arrière, **←→** diriger,
**Espace** boost, **C** caméra extérieure, **R** appeler la grue, **P** pause.
Les manettes (API Gamepad) sont reconnues.

### Réglages

Le panneau de droite expose tous les paramètres (échelle verticale des circuits, nombre de
tours, gravité, puissance, vitesse de pointe, boost, freinage, adhérence, suspension,
seuils de dégâts, niveau de l'adversaire, champ de vision, résolution, palette ST…).
Ils sont appliqués en direct, mémorisés, et exportables / importables en JSON.

Des circuits personnalisés peuvent être construits avec `SCR.Track.fromCommands`
(voir les démos dans `replica/js/main.js`).

## Outils

```sh
cd tools
python3 scr_tool.py ls      disque.st              # fichiers de la disquette
python3 scr_tool.py extract disque.st fichiers/    # extraction
python3 scr_tool.py game    disque.st jeu.prg      # programme du jeu décompressé
python3 scr_tool.py image   disque.st jeu.bin      # le même, relogé à $10100 (comme en mémoire)
python3 scr_tool.py tracks  disque.st tracks.json  # les 8 circuits (géométrie complète)
python3 scr_tool.py preview disque.st circuits.png # vue de dessus (Pillow)

./build_stemu.sh                                    # compile l'émulateur de test + désassembleur
./stemu_script.py scenarios/practice_little_ramp.txt --disk disque.st --dir fichiers/ \
    --shot 3300:depart.ppm --dump 3300:ram.bin      # joue un scénario, capture écran / mémoire
./dasm jeu.bin 0x10100 0 0x48390 0x48810           # désassemble (fichier, base, offset, début, fin)
```

Tests de la réplique (Node.js) :

```sh
cd replica
node test/sim.js disque.st 150      # Remake : pilote automatique sur les 8 circuits (tours, chutes, dégâts)
node test/engine_test.js disque.st 0 60     # moteur fidèle : démarrage, entraînement, ticks
node test/cpu_vs_musashi.js snap/in snap/out 4EEB0   # CPU JS contre Musashi (instantanés stemu --snap)
node test/engine_vs_emu.js snap/loop disque.st 0 149 # boucle de course JS contre l'émulateur
node test/shadow_test.js disque.st 0 600    # chaque routine de physique portée contre l'original
node test/jsphysics_test.js disque.st 0 400 # partie complète : physique 68000 contre physique JS
node test/trace.js disque.st 0 30   # trace détaillée d'un circuit
NODE_PATH=$(npm root -g) node test/shot.js disque.st 0 /tmp/capture   # captures Chromium
```

## État d'avancement

* Disquette, décompression, isolation du programme du jeu : **fait**.
* Jeu exécuté dans l'émulateur de test, du menu à la course : **fait**.
* Décodage des 8 circuits : **fait**, vérifié octet par octet contre le jeu en cours d'exécution.
* Moteur fidèle : CPU 68000 JavaScript + TOS minimal ; le jeu complet tourne dans le
  navigateur (≈ 20× le temps réel en Node). Validé contre Musashi : la physique d'origine
  (≈ 2 240 instructions/tick) donne des états identiques, et la boucle de course complète
  reste identique pendant 78 ticks (seule l'horloge, cadencée par la VBL, diffère ensuite).
* Remake : rendu 3D façon ST, cockpit, chrono, tours, boost, dégâts, grue, adversaire,
  réglages : **jouable** ; la physique est un modèle approché, réglable.
* Physique d'origine décompilée dans ses grandes lignes (docs §8) ; ses constantes
  (gravité, poussée, freinage, pas de temps, amortissement, boost, tolérance aux chocs)
  sont **réglables dans le mode Original**.
* Physique d'origine **entièrement portée en JavaScript lisible** (`replica/js/physics_orig.js`),
  validée bit à bit contre l'original ; le mode Original peut l'utiliser à la place du code
  68000 (réglage « Physique exécutée par le code JS décompilé »), avec une adhérence réglable.
* Prochaine étape : porter le reste de la logique par tick pour un Remake 100 % décompilé.
