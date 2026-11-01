(English version below / version anglaise plus bas)

Images HD de remplacement (mode HD, touche F8)
==============================================

Déposez ici des images PNG (ou JPG) : en mode HD, elles remplacent les sprites du cockpit
d'origine, au même endroit et au même moment. F2 en jeu recharge le dossier (pratique
pendant que vous retouchez vos images).

Noms des fichiers
  sprite_06.png                une image fixe pour le sprite n° 6
  sprite_06_1.png, _2, _3 ...  plusieurs images = animation (cadence : fps, 15 par défaut)

Numéros utiles (planche complète : voir le paquet des graphismes)
  flammes du boost : 6, 7, 49 (échappements gauches) et 8, 9, 50 (droits) — 64x28 d'origine
                     le jeu alterne 6+8, 7+9, 49+50 ; vous pouvez mettre les mêmes images aux trois
  chaînes de la grue : 19 à 22

Cockpit fixe (cadre, montants, tableau de bord)
  cockpit.png                  image aux proportions de l'écran d'origine (320x200, par exemple
                               1280x800), fenêtre et vitres latérales transparentes. Elle remplace
                               le cockpit dessiné par le jeu. Laissez les afficheurs (plaques grises)
                               et le compteur de vitesse SANS chiffres ni repère : les chiffres du
                               jeu (tour, boost, dégâts, temps) et la barre de vitesse sont posés
                               dessus. La fissure des dégâts et tout ce qui change en course restent
                               visibles par-dessus l'image. Le bas du moteur (sous le sprite 10) fait
                               partie du cockpit fixe : dessinez-le dans cockpit.png.
                               La fissure des dégâts (barre du haut de l'écran d'origine) est replacée
                               sur le tube du haut de cockpit.png, quelle que soit sa hauteur.
  cockpit_avant.png            (facultatif) partie du cockpit à dessiner DEVANT les roues, par exemple
                               la base du moteur ; mêmes proportions que cockpit.png, le reste
                               transparent. Elle passe sous le moteur (sprite 10) et les flammes.
  cockpit_fond.png             (facultatif) fond dessiné SOUS les roues et le moteur, mêmes proportions
                               que cockpit.png : bouche par exemple le jour entre le moteur et le tube,
                               sans masquer les roues.

  foule.wav                    (facultatif) son de foule joué en boucle par chaque tribune à la place
                               de la foule synthétisée : WAV PCM 16 bits, mono ou stéréo, quelques
                               secondes ou plus (le début et la fin sont fondus l'un dans l'autre).
                               Utilisez un son dont vous avez les droits (enregistrement personnel
                               ou bruitage libre de droits).

Les textures HD se coupent dans le menu d'options (touche F1) ou avec textures_hd = 0 dans scr.ini.

Règles pour les sprites
  - même proportions que le sprite d'origine (planche complète) ;
  - le dessin doit remplir le cadre comme l'original : pas de grande marge vide autour
    (l'image entière est ajustée à l'emplacement du sprite) ;
  - là où l'original est plein (noir compris), ne laissez pas de trou transparent.

Transparence
  - PNG avec transparence  -> collé tel quel (mélange « alpha »)
  - image sans transparence -> mélange « add » : le noir disparaît, le reste s'ajoute en
    lumière. Idéal pour du feu, des étincelles, des reflets : prenez des flammes SUR FOND NOIR.

Taille
  N'importe quelle définition : l'image est ajustée à l'emplacement du sprite d'origine
  (gardez ses proportions, par exemple 64x28 -> 512x224). Pour déborder (flammes plus
  longues), utilisez scale dans hd.ini.

hd.ini (facultatif)
  [sprite_06]
  blend = add       ; add ou alpha
  scale = 1.3       ; taille par rapport à l'emplacement d'origine
  anchor = bottom   ; point fixe pour scale : bottom, center ou top
  dx = 0            ; décalage, en pixels de l'écran d'origine (320x200)
  dy = -2
  fps = 20          ; cadence de l'animation
  wind = 1          ; effet du vent de la course : l'image se couche vers l'extérieur et s'aplatit
                    ; avec la vitesse (1 par défaut pour les flammes, 0 = sans)
  pad_top = 0.4     ; l'image déborde de l'emplacement du sprite (fraction de sa hauteur ou
  pad_bottom = 0    ; largeur) : pad_top, pad_bottom, pad_left, pad_right — par exemple des
                    ; flammes recalées plus haut que l'emplacement d'origine
  layer = 1         ; ordre de dessin : plus grand = par-dessus. Par défaut : flammes 2,
                    ; moteur (sprite 10) et bas des gros tuyaux (11, 12) 1, roues (0-5) et
                    ; leurs pièces (37-48) -1, le reste 0. Un plan négatif est dessiné SOUS le
                    ; cockpit (cockpit.png) : les roues passent derrière le cadre et le moteur

Les flammes du boost utilisent les images sprite_06, 07, 08, 09, 49 et 50 de ce dossier quand
l'option « Textures HD » est active (menu F1, textures_hd = 1 dans scr.ini). Sans ces images,
ou avec les textures HD coupées, ce sont les sprites d'origine agrandis en haute définition.


==============================================================================
ENGLISH
==============================================================================

Replacement HD images (HD mode, F8 key)
=======================================

Put PNG (or JPG) images here: in HD mode they replace the original cockpit sprites, in the same
place and at the same moment. F2 in the game reloads the folder (handy while you are touching
up your images).

File names
  sprite_06.png                a still image for sprite no. 6
  sprite_06_1.png, _2, _3 ...  several images = animation (rate: fps, 15 by default)

Useful numbers (full sheet: see the graphics pack)
  boost flames: 6, 7, 49 (left exhausts) and 8, 9, 50 (right) - 64x28 originally
                the game alternates 6+8, 7+9, 49+50; you may use the same images for all three
  crane chains: 19 to 22

Fixed cockpit (frame, pillars, dashboard)
  cockpit.png                  image with the proportions of the original screen (320x200, for
                               example 1280x800), windscreen and side windows transparent. It
                               replaces the cockpit drawn by the game. Leave the displays (grey
                               plates) and the speedometer WITHOUT digits or marks: the game's
                               figures (lap, boost, damage, time) and the speed bar are drawn on
                               top. The damage crack and everything that changes during the race
                               stay visible over the image. The bottom of the engine (under sprite
                               10) is part of the fixed cockpit: draw it in cockpit.png.
                               The damage crack (top bar of the original screen) is moved onto the
                               top tube of cockpit.png, whatever its height.
  cockpit_avant.png            (optional) part of the cockpit drawn IN FRONT of the wheels, for
                               example the base of the engine; same proportions as cockpit.png,
                               the rest transparent. It goes under the engine (sprite 10) and the
                               flames.
  cockpit_fond.png             (optional) background drawn UNDER the wheels and the engine, same
                               proportions as cockpit.png: for example it fills the gap between
                               the engine and the tube without hiding the wheels.

  foule.wav                    (optional) crowd sound looped by each grandstand instead of the
                               synthesised crowd: 16-bit PCM WAV, mono or stereo, a few seconds
                               or more (start and end are cross-faded). Use a sound you have the
                               rights to (your own recording or a royalty-free sound effect).

HD textures can be turned off in the options menu (F1 key) or with textures_hd = 0 in scr.ini.

Rules for sprites
  - same proportions as the original sprite (full sheet);
  - the drawing must fill the frame like the original: no large empty margin around it
    (the whole image is fitted to the sprite's place);
  - where the original is solid (black included), do not leave a transparent hole.

Transparency
  - PNG with transparency   -> pasted as is ("alpha" blending)
  - image without transparency -> "add" blending: black disappears, the rest is added as
    light. Ideal for fire, sparks, reflections: use flames ON A BLACK BACKGROUND.

Size
  Any resolution: the image is fitted to the original sprite's place (keep its proportions,
  for example 64x28 -> 512x224). To overflow (longer flames), use scale in hd.ini.

hd.ini (optional)
  [sprite_06]
  blend = add       ; add or alpha
  scale = 1.3       ; size relative to the original place
  anchor = bottom   ; fixed point for scale: bottom, center or top
  dx = 0            ; offset, in pixels of the original screen (320x200)
  dy = -2
  fps = 20          ; animation rate
  wind = 1          ; effect of the race wind: the image leans outwards and flattens with
                    ; speed (1 by default for the flames, 0 = none)
  pad_top = 0.4     ; the image extends beyond the sprite's place (fraction of its height or
  pad_bottom = 0    ; width): pad_top, pad_bottom, pad_left, pad_right - for example flames
                    ; placed higher than the original place
  layer = 1         ; drawing order: higher = on top. Defaults: flames 2, engine (sprite 10)
                    ; and bottom of the big pipes (11, 12) 1, wheels (0-5) and their pieces
                    ; (37-48) -1, the rest 0. A negative layer is drawn UNDER the cockpit
                    ; (cockpit.png): the wheels go behind the frame and the engine

The boost flames use the images sprite_06, 07, 08, 09, 49 and 50 from this folder when the
"Textures HD" option is on (F1 menu, textures_hd = 1 in scr.ini). Without these images, or with
HD textures off, they are the original sprites enlarged in high definition.
