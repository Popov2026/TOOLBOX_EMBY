# Stunt Car Racer — rétro-ingénierie et réplique PC paramétrable

Projet de décompilation de **Stunt Car Racer** (Geoff Crammond, Micro Style, 1989),
version **Atari ST**, et réplique jouable sur PC dont tous les réglages sont modifiables.

> **Aucune donnée du jeu n'est incluse dans ce dépôt.** Les outils et la réplique lisent
> les circuits directement depuis **votre** image disque (`.st`). Sans disquette, la
> réplique propose des circuits de démonstration originaux.

## Contenu

| Dossier | Contenu |
|---|---|
| `replica/` | la réplique (HTML5 / JavaScript, sans dépendance) |
| `tools/` | outils de rétro-ingénierie : extraction, décompression, décodage des circuits, émulateur ST de test, désassembleur |
| `docs/RETRO_INGENIERIE.md` | formats, adresses, routines décompilées, méthode de vérification |

## Lancer la réplique

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
node test/sim.js disque.st 150      # pilote automatique sur les 8 circuits (tours, chutes, dégâts)
node test/trace.js disque.st 0 30   # trace détaillée d'un circuit
NODE_PATH=$(npm root -g) node test/shot.js disque.st 0 /tmp/capture   # captures Chromium
```

## État d'avancement

* Disquette, décompression, isolation du programme du jeu : **fait**.
* Jeu exécuté dans l'émulateur de test, du menu à la course : **fait**.
* Décodage des 8 circuits : **fait**, vérifié octet par octet contre le jeu en cours d'exécution.
* Réplique : rendu 3D façon ST, cockpit, chrono, tours, boost, dégâts, grue, adversaire,
  réglages : **jouable** ; la physique est un modèle approché, réglable.
* Prochaines étapes : décompiler la physique et la projection d'origine pour remplacer
  les valeurs approchées (voir `docs/RETRO_INGENIERIE.md`).
