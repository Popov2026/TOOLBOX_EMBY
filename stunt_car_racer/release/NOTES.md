## Stunt Car Racer v70 : rendu 3D sur la carte graphique (OpenGL 3.3)

- La 3D (circuit, décor, ciel, textures, ombres, fumée, lissage) est maintenant dessinée par la carte graphique ; le cockpit et les images HD sont posés par-dessus comme avant.
- Option « Rendu 3D : OpenGL / Processeur » dans le menu d'options (F1), ou `rendu = opengl | processeur` dans `scr.ini`. OpenGL par défaut.
- Sans OpenGL 3.3, le jeu dessine automatiquement sur le processeur. « OpenGL » s'affiche dans le titre de la fenêtre quand la carte graphique est utilisée.

### Fichiers
- `StuntCarRacer-win64-v70.zip` : le jeu pour Windows 64 bits (aucune donnée du jeu d'origine : placez votre image disque .st dans `DISK`, voir `LISEZMOI.txt`).
- `ServeurRelaisStuntCar-v70.zip` : le serveur relais pour le multijoueur en ligne (même version que le jeu).
