# Vers une version Falcon030 — ce que contient ce projet et par où commencer

## Ce qui ne se porte pas tel quel

La version PC (`native/`) **n'est pas une réécriture** du jeu : elle exécute le code 68000
d'origine dans un émulateur (Musashi) et redessine l'image en HD à partir de l'état du jeu.
Elle ne peut donc pas servir directement sur Falcon, qui a de toute façon un vrai 68030.
Un émulateur 68000 écrit en C serait beaucoup trop lent sur un 68030 à 16 MHz. Le moteur HD
(rendu logiciel 1080p multicœur) l'est aussi.

Une version Falcon030 consiste donc à **reprendre le programme d'origine** (code 68000 du jeu,
extrait de votre disquette) et à le modifier. Ce projet fournit les outils et la documentation
nécessaires pour cela.

## Ce qui sert

| Élément | Usage pour le Falcon |
|---|---|
| `tools/scr_tool.py` | lecture de la disquette (FAT12 au BPB faux), décompression JEK/ByteKiller, extraction du programme du jeu (TEXT $5D6C6 + BSS $5BAA) |
| `tools/dasm.c` | désassembleur 68000 pour produire un source réassemblable |
| `tools/stemu.c` | émulateur ST sans affichage : points d'arrêt, journal des PC, surveillance mémoire (pratique pour tester un patch) |
| `docs/RETRO_INGENIERIE.md` | adresses des routines et variables (boucle, physique, rendu, remplissages, sprites, VBL, clavier) |
| `native/src/machine.cpp` | liste exacte de ce que le jeu demande au système : TOS minimal (Super, Cursconf, Keytbl, Jenabint, Physbase, Floprd), registres matériel touchés, vecteurs détournés |
| `native/src/track.cpp` | décodage des circuits (sections gauche/droite, hauteurs) en C lisible |
| `native/src/render3d.cpp` | principe d'un rendu par polygones avec tampon 1/z et découpage aux bords, si vous réécrivez le rendu |
| `native/src/hdview.cpp` | caméra calée sur l'original (focale, centre, hauteur de l'œil), couleurs et alternances (route, flancs, lignes de bord) |

## Points d'attention connus sur 68030 / Falcon

Ce sont des règles générales du portage ST → Falcon, appliquées à ce que l'on sait du jeu.
Rien n'a encore été testé sur un vrai Falcon.

* **Matériel ST détourné directement** : vecteur clavier `$118` (routine `$104B4`), VBL `$70` →
  `$4EC24`, base vidéo écrite en `$FF8201/03`, YM2149 pour le son. Le Falcon garde ces
  registres en mode compatible ST. Si vous passez à un mode VIDEL, la base vidéo et le
  format de l'écran changent (voir plus bas).
* **Caches du 68030** : le code est décompressé puis relogé en mémoire avant d'être exécuté.
  Il faut vider le cache d'instructions après ces étapes, ou couper les caches.
* **Cadence** : la logique est réglée par la VBL (compteur `$4EC20` rechargé à 6, soit
  8,33 ticks/s). Le jeu ne devrait donc pas accélérer à 16 MHz. Le rendu (`$51BCC`,
  ≈ 91 000 instructions) sera simplement plus rapide, ce qui peut rendre l'image plus fluide.
* **Protection** : la Copylock (mode trace, vecteur `$24`, `$49E38`) est déjà neutralisée sur
  la version étudiée (octet `$51898`). Une autre version de la disquette peut la contenir encore.
* **Code issu du 6502** : variables d'un octet à adresses absolues, pointeurs petit-boutistes
  convertis par `rol.w #8`. Il n'y a rien de spécifique au 68000 là-dedans, mais il faut le
  savoir en lisant le désassemblage.

## Idées d'améliorations propres au Falcon

* **Écran** : toutes les écritures à l'écran passent par peu de routines. Le décor passe par
  les remplissages `$53166`, `$533EC`-`$53456`, `$5461A`-`$546BE` et `$553D8`-`$55442`. Le
  cockpit passe par les blits `$567B0`-`$56880`, le tableau de bord par `$46C3E`-`$46D06`,
  `$4B890` et `$53212`-`$5327C`. Les réécrire suffit pour passer à un autre format d'écran
  (par exemple 256 couleurs en plans entrelacés, ou 16 bits « true colour »), voire à une
  résolution plus haute si la projection est adaptée.
* **DSP 56001** : les transformations 3D et la projection des points du circuit dans `$51BCC`
  sont de bons candidats pour un déport sur le DSP.
* **Son** : le moteur est joué par la VBL sur le YM2149. Le DMA son 16 bits du Falcon permet
  de mieux faire.

## Données du jeu

Aucune donnée ni aucun code du jeu n'est fourni ici. Tout s'extrait de **votre** image disque
`.st` avec `tools/scr_tool.py`.
