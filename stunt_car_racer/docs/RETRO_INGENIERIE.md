# Stunt Car Racer (Atari ST) — notes de rétro-ingénierie

Version étudiée : compilation « Compact Disk 2.1 » (V8 / Fallen Angels / Highlanders),
disquette `Stunt_Car_Racer_1989Micro_Styleb2.st` (720 Ko). Aucune donnée du jeu n'est
reproduite ici : seuls les formats et les adresses sont documentés.

## 1. Disquette

* Image `.st` brute : 80 pistes × 2 faces × 9 secteurs × 512 octets, FAT12 standard
  **sauf** le BPB qui annonce 0 secteur par cluster (TOS le traite comme 1) :
  `scr_tool.py` force `spc = 1`.
* Fichiers :

| Fichier | Rôle |
|---|---|
| `AUTO/DEBUT.PRG` | menu de la compilation (« 1) HARD AND HEAVY  2) STUNTCAR ») |
| `GAME.PUT` | **Stunt Car Racer** (compressé, intro du cracker + jeu) |
| `HIGHSCOR.ES` | meilleurs scores (512 o.) |
| `HARD.PUT`, `MAIN.DAT`, `MUSIC.DAT`, `GRAPHIX.IMG`, `TITLE.PIC`, `RELINE.PIC` | l'autre jeu de la compilation (Hard'n'Heavy) |

## 2. Compression

`GAME.PUT` et `DEBUT.PRG` sont des exécutables TOS compressés par **JEK Packer 1.3**,
qui utilise l'algorithme **ByteKiller** : flux de bits lu à l'envers depuis la fin des
données, mots longs `longueur`, `checksum`, premier mot de bits ; décompression arrière.
Codes (bits lus poids fort d'abord) :

| Préfixe | Action |
|---|---|
| `00` + 3 bits *n* | *n*+1 octets littéraux |
| `01` + 8 bits *o* | copie 2 octets depuis +*o* |
| `1 00` + 9 bits *o* | copie 3 octets |
| `1 01` + 10 bits *o* | copie 4 octets |
| `1 10` + 8 bits *n* + 12 bits *o* | copie *n*+1 octets |
| `1 11` + 8 bits *n* | *n*+9 octets littéraux |

Le checksum (XOR de tous les mots longs lus) doit valoir 0 à la fin.

`GAME.PUT` décompressé (391 763 o.) = intro du cracker (TEXT de $838 octets, texte
défilant « cracked by Yoda ») dont le segment DATA contient **un second exécutable** :
le jeu lui-même (TEXT $5D6C6, BSS $5BAA). L'intro le reloge à son adresse de chargement
(dans l'émulateur : base TEXT = **$10100**) et y saute.

## 3. Le programme du jeu

* Le code 68000 est une **traduction quasi mécanique du code 6502 de la version C64** :
  variables d'un octet aux adresses absolues ($109xx…$111xx tiennent lieu de page zéro),
  tables indexées `(An,Dn.w)`, pointeurs 16 bits **petit-boutistes** reconvertis à
  l'exécution par `rol.w #8` (cf. `$4881C`).
* Les données 6502 sont restées telles quelles : l'adresse 6502 **$B100** correspond à
  **$13670** dans le jeu chargé (`adresse = $13670 + (adr6502 − $B100)`).
* On trouve même des restes de code 6502 et des fragments du source assembleur
  (`FETCH  move.b 0(a5,d5.w),d0`…) dans le binaire.
* Au démarrage : somme de contrôle du code (`$0006`…), lecture du secteur 5 piste 0 par
  `Floprd` (XBIOS 8), puis interception directe du matériel : vecteur clavier `$118`
  (`$104B4`, table des touches en `$6F02C`, valeur `$B3` = enfoncée), VBL `$70` → `$4EC24`,
  tables clavier TOS via `Keytbl` (XBIOS 16) pour la saisie du nom.

### Variables utiles (jeu chargé à $10100)

| Adresse | Contenu |
|---|---|
| `$10906`, `$10985` | pièce de circuit où se trouve le joueur |
| `$10A1E` | (probable) section dans la pièce |
| `$11114` | nombre de pièces du circuit courant |
| `$11116` | pièce de la ligne de départ |
| `$10C82[]` | case de grille de chaque pièce (Z×16 + X) |
| `$10CE6[]` | type de chaque pièce (forme + orientation) |
| `$10BBA[]`, `$10C1E[]` | profil de hauteur gauche / droit de chaque pièce |
| `$10D4A[]`, `$10E12[]` | hauteur (mot) du bord gauche / droit au début de chaque pièce |
| `$10EDA[]` | distance cumulée (mot, en sections × 32) |
| `$6F02C` | état des 128 touches |

## 4. Les circuits

Table des 8 circuits : `$13790` (pointeurs 6502), dans l'ordre des noms stockés en
`$13498` : LITTLE RAMP, STEPPING STONES, HUMP BACK, BIG RAMP, SKI JUMP, DRAW BRIDGE,
HIGH JUMP, ROLLER COASTER. Chaque circuit tient en 130 à 210 octets.

Routine de décodage `$48390` (portée à l'identique dans `tools/scr_tool.py` et
`replica/js/scrdata.js`, **vérifiée octet par octet** contre la mémoire du jeu pour
Little Ramp et Hump Back) :

```
en-tête : nbPièces, ?, pièceDépart, ?        (4 octets → $11114..$11117)
hauteur initiale : 2 octets petit-boutistes
pour chaque pièce :
  type  (1 octet) : bits 0-3 forme, bit 4 parcours inverse, bit 5 profil droit = gauche,
                    bits 6-7 orientation (quart de tour) ;  type $nF = répéter n fois
                    la pièce précédente sur les cases suivantes
  case  (1 octet) : Z<<4 | X  (absente pour une pièce répétée)
  profil gauche (1 octet), profil droit (1 octet, sauf bit 5)   [formes 12..15 : tables $48802/$48804]
suivi de 6 octets ($11124..), puis listes optionnelles ($10FA2/$10FC2, $10FE2)
```

* **Grille** de 16 × 16 cases de $800 unités.
* **Formes** (`$13670`, pointeurs 6502) : chaque forme liste des sections transversales
  (point gauche, point droit) en coordonnées locales de case. Formes utilisées :
  0 droite (8 segments), 1/3 virage de 45° à droite/gauche, 6/7 virages de 45° longs,
  4 et 10 diagonales. La route fait $180 unités de large, centrée dans la case.
* **Orientation** : bits 7-6 = rotation (0 : +Z, 1 : (z, $800−x), 2 : demi-tour,
  3 : ($800−z, x)) ; bit 4 = sections parcourues à l'envers **et** bords gauche/droit
  permutés. Trouvée par recherche exhaustive en imposant la continuité des pièces :
  erreur cumulée de 72 unités sur l'ensemble des 8 circuits.
* **Hauteurs** : profils (`$13690`) indexés par section ; format 1 octet
  `((b & $0F) << 8) | ((b << 1) & $E0)` ou, si le profil gauche a le bit 7, 2 octets
  `((b0 & $7F) << 8) | b1`. Le bit 7 d'un octet de profil marque un sommet de rupture de
  pente (utilisé par le rendu, `$525CC`). Les sauts sont de vraies chutes de hauteur
  (rampe → fosse au niveau du sol → réception).
* L'échelle verticale n'est pas 1:1 : le code physique utilise hauteur/32 pour 1/16 en
  x/z, soit une échelle de 0,5 (valeur par défaut de la réplique, réglable).

## 5. Boucle de jeu et moteur fidèle

* Boucle principale `$4A5B6` : menus `$49032` → vue d'ensemble + chargement du circuit
  `$4A80A` (circuit en `$1112D`, attente « feu » en `$4AF5C`) → course `$4A924` → `$51312`.
* Course : préparation, grue, puis boucle `$4AA74`…`$4AC0E`. Une itération = un *tick* :
  entrées `$4AE7E` (joystick `$106A6` → `$10931`), physique `$4EEB0`, …, rendu et suivi de
  la pièce courante `$51BCC` (≈ 91 000 instructions, contient aussi de la logique), échange
  d'écrans et attente `$4B0A6` : le compteur `$4EC20`, rechargé à 6 et décrémenté par la VBL
  (`$4EC24`), cadence la logique à **8,33 ticks/s** (Δt fixe de 0,12 s).
* La VBL gère aussi le son du moteur (`$10A1E += $109DE`, registres du YM2149) et le
  chronomètre (`$51F38` : `$109CD += $10A4C`).
* Position de la voiture : `$10AC2`, `$10AC6`, `$10ACA` (x, y, z en 16.16 ; 128 unités
  par case, soit 16 unités de géométrie ; y = hauteur brute / 32 → échelle verticale
  physique **0,5**, cf. `$495F2` qui calcule `(profil + base) >> 5`).
* Protection Copylock (code chiffré par le mode trace, vecteur `$24`, `$49E38`) neutralisée
  par le crack via l'octet `$51898`.
* Le jeu n'utilise que quelques appels système : Super, Cursconf, Keytbl, Jenabint,
  Physbase et Floprd (secteur 5 piste 0, sert seulement à une clé inutilisée `$4A52C`).

Le moteur fidèle (`replica/js/engine.js`) charge et reloge le programme, simule ce TOS
minimal et les registres matériels utiles, injecte les VBL, et peut soit exécuter le jeu en
temps réel (mode Original), soit l'amorcer automatiquement jusqu'à l'entraînement sur un
circuit donné (utilisateur simulé pour les menus, réponse immédiate à la vue d'ensemble).

## 8. Physique de la voiture (routine `$4EEB0`, une fois par tick)

Corps rigide à **trois points de contact**, intégré en virgule fixe avec un facteur de pas
`0xEE/256` présent sur 21 sites (`move.b #$EE,D2` + `muls` + `asr.l #8`).

| Grandeur | Adresse | Notes |
|---|---|---|
| position x, y, z | `$10AC2`, `$10AC6`, `$10ACA` | 16.16 ; x,z : 128 / case ; y bornée à 1000 |
| tangage, lacet, roulis | `$10ACE`, `$10AD0`, `$10AD2` | 0x10000 = 360° ; lacet 0 = +z, 90° = +x |
| vitesse (monde) | `$10AD4/6/8` | position += v × Δt × 2⁶ (x,z) ou 2⁷ (y) — `$4EFA4` |
| vitesses angulaires | `$10ADA/C/E` | angles += ω × Δt ; tangage/roulis bornés (table `$4F128`) |
| accélérations (monde) | `$10AE0/2/4` | v += a × Δt — `$4F130` |
| accélérations angulaires | `$10AE6/8/A` | `$4F17A` |
| forces locales | `$10B1C/1E/20` | latérale / normale / longitudinale, ramenées au monde par la matrice |
| matrice d'orientation | `$6EEDC` | construite par `$4E8B2` (cos `$51A88`, sin `$51A90`, Q15, table `$11130`) |

Enchaînement : matrice (`$4E8B2`) → points de contact (`$4EF22`, `$49718`, `$4F1C4`) →
vitesses locales (`$4EAD6`) → **gravité** dans le repère voiture (`$4EB30`, constante
**0x13D** en `$4E88C`, −0x13D en `$4E884`) → **suspension** (`$4F220`) : pour chaque roue,
compression = sol − roue bornée [−0x300, 0x1400], ressort + amortisseur (`$4EE62`,
coefficient **0x114**), force bornée à 0x11FF ; une force > 0x700 + (`$108EB` << 8) ajoute
des **dégâts** à la roue (`$10939/A/B`) → répartition (`$4F8E6`) → adhérence latérale
limitée à 2 × la charge (`$4F784`, `$4F7E4`) → propulsion (`$4F6C2`) → **direction** et couple
de lacet (`$4E55C`, avec un contrôle anti-piratage : la clé `$51894` = 0xC0BE145F doit être
présente, sinon la voiture ne tourne plus) → frottement proportionnel à la charge
(`$4F7FE`) → intégration.

**Commandes** (`$4AEDC`) : joystick haut → poussée `$10B14` = mot lu dans `$108E4/$108E5`
(petit-boutiste) ; bas → freinage fixe 0xFF10 (−240). **Boost** (`$4DE20`) : bouton → poussée
doublée tant qu'il reste du carburant `$1111A` (BCD), décrémenté tous les `$108E8` ticks ; la
réserve initiale vient des octets de fin du circuit (`$11126`, super ligue `$11127`).

**Caractéristiques de la voiture** : au départ de chaque course, `$4AD10` copie 11 octets
depuis `$1455A` (ligue normale) ou `$1455A+11` (super ligue, drapeau `$110CA`) vers
`$108E2…$108EC` : poussée 240 / 320, allure de l'adversaire (`$108E6/7`, 0x00EC / 0x013A),
période de consommation du boost (16 / 12 ticks), tolérance aux chocs (0 / 1).

**Port JavaScript** : `replica/js/physics_orig.js` réécrit toute la routine `$4EEB0` et ses
36 sous-routines en code lisible (`physicsStep`), en virgule fixe identique. Validation :
`test/shadow_test.js` rejoue chaque routine sur une copie de la mémoire d'entrée et compare
aux écritures de l'original (8 circuits × 600 ticks : exact) ; `test/jsphysics_test.js`
joue une partie avec la physique d'origine et une avec le port : trajectoires identiques
bit à bit. Particularités conservées : `move.b $FF.l,D0` au lieu de `#$FF` (`$497C8`, lit un
octet de la table des vecteurs), contrôle anti-piratage de la direction, roue « dans le
vide » (`$49D50` : sol forcé à 0x1000, registre `$10984`).

Ces constantes sont exposées comme réglages du mode Original (`Engine.applyTuning`). Elles
sont réécrites **après** la somme de contrôle du démarrage (`$10106`, qui couvre tout le
code et bloquerait le jeu).

## 6. Outils

* `tools/scr_tool.py` : liste/extraction FAT12, décompression, extraction du programme du
  jeu, décodage des circuits en JSON, aperçu PNG.
* `tools/stemu.c` : émulateur Atari ST minimal **sans affichage** (CPU Musashi, TOS simulé
  en C, MFP, IKBD, shifter). Captures d'écran, injection clavier/joystick, vidages mémoire,
  points d'arrêt, journal des PC exécutés, surveillance des lectures/écritures mémoire.
  Suffisant pour faire tourner le jeu du menu jusqu'à la course.
* `tools/stemu_script.py` : scénarios d'entrées (voir `tools/scenarios/`).
* `tools/dasm` : désassembleur 68000.

## 7. Reste à faire

* Porter le reste de la logique par tick (entrées `$4AE7E`, suivi de pièce et tours dans
  `$51BCC`, adversaire) pour que le Remake tourne entièrement sur le code décompilé.
* Décompiler la projection 3D pour l'échelle verticale et le champ de vision exacts.
* IA des adversaires, pont-levis animé (Draw Bridge), ligue / divisions.
