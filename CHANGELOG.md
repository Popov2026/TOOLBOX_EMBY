# Changelog

## 2026.10.3

### Français
- **Fichier audio comparatif mixé :** les deux versions ne sont plus jouées
  l'une après l'autre mais **superposées**, la 2e décalée d'**1 seconde** (1re
  plutôt à gauche, 2e plutôt à droite). Même doublage = la même voix en écho ;
  doublages différents = deux voix différentes. 3 répliques, chacune après un
  bip.

### English
- **Mixed comparison audio file:** the two versions are no longer played one
  after the other but **overlaid**, the 2nd delayed by **1 second** (1st mostly
  left, 2nd mostly right). Same dub = the same voice echoed; different dubs =
  two different voices. 3 lines, each after a beep.

## 2026.10.2

### Français
- **Doublons – vérification du son des pistes françaises :** « SUPPRIMER (sûr) »
  exige maintenant que le SON des pistes françaises ait été vérifié identique
  (même doublage) ; sur les seules étiquettes, le verdict est au mieux
  « probable ». Une piste « VF » sans variante n'est jamais « sûre » sans
  vérification.
- **VFF / VFQ contrôlées par le son :** toutes les pistes françaises de chaque
  version sont comparées entre elles (bande de la voix, 300–3 400 Hz). Sont
  signalés : même étiquette mais doublages différents (« l'un est peut-être une
  VFQ »), étiquettes différentes mais même doublage (« une des deux est
  fausse »), piste VF identifiée comme VFF ou VFQ. Versions PAL (25 i/s)
  compensées.
- **Fichier audio comparatif par film** (`comparaisons_audio\*.m4a`) : 3
  répliques ; pour chacune, 1 bip + la réplique de la 1re version, puis 2 bips
  + la même réplique dans la 2e version. Bouton « Écouter » dans le groupe et
  dans la fenêtre « Audio ».
- **Fenêtre « Suggestions » :** bouton « Vérifier le son des pistes
  françaises » pour tous les films proposés ; seuls les clones dont le son est
  confirmé sont cochés d'office.

### English
- **Duplicates – sound check of the French tracks:** "DELETE (safe)" now
  requires the SOUND of the French tracks to be checked identical (same dub);
  on labels alone the verdict is at best "likely". A "VF" track with no variant
  is never "safe" without a check.
- **VFF / VFQ checked by sound:** every French track of each version is
  compared with the others (voice band, 300–3,400 Hz). Reported: same label but
  different dubs ("one may be a VFQ"), different labels but same dub ("one of
  them is wrong"), VF track identified as VFF or VFQ. PAL versions (25 fps)
  compensated.
- **Comparison audio file per movie** (`comparaisons_audio\*.m4a`): 3 lines;
  for each, 1 beep + the line from the 1st version, then 2 beeps + the same line
  in the 2nd version. "Listen" button in the group and in the "Audio" window.
- **"Suggestions" window:** "Check the sound of the French tracks" button for
  every proposed movie; only clones whose sound is confirmed are ticked by
  default.

## 2026.10.1

### Français
- **Doublons – suggestions de suppression des clones :** langue de chaque piste
  (VFF / VFQ / VF / ENG…), pistes audio identiques ou non, verdict par fichier
  (GARDER / SUPPRIMER sûr ou probable / À VÉRIFIER) avec la raison, langue
  préférée (VFF ou VFQ), fenêtre « Suggestions de suppression » avec
  suppression groupée après confirmation.
- **Doublons – « Analyser son » :** comparaison du son réel des pistes avec
  ffmpeg (clone réencodé ou décalé reconnu, doublage mal étiqueté repéré) ;
  résultat repris dans la suggestion et dans la fenêtre « Audio ».
- **Correction :** une vidéo 1280×720 était classée « 1080p » (le plus grand
  côté de l'image était comparé à 1080) ; elle est maintenant classée 720p.

### English
- **Duplicates – clone deletion suggestions:** language of each track
  (VFF / VFQ / VF / ENG…), identical audio tracks or not, verdict per file
  (KEEP / DELETE safe or likely / TO CHECK) with the reason, preferred language
  (VFF or VFQ), "Deletion suggestions" window with batch deletion after
  confirmation.
- **Duplicates – "Analyse sound":** compares the real sound of the tracks with
  ffmpeg (re-encoded or shifted clone recognised, wrongly labelled dub
  spotted); the result is used in the suggestion and in the "Audio" window.
- **Fix:** a 1280×720 video was classified "1080p" (the longest side of the
  picture was compared to 1080); it is now classified 720p.

## 2026.10.0

### Français
- **Doublons – suppression :** fin des faux « timeout ». Le délai d'attente de la
  réponse d'Emby passe de 15 s à 180 s. S'il est quand même dépassé, l'outil
  vérifie pendant 90 s auprès d'Emby que l'élément a bien disparu avant de
  signaler une erreur. Sablier animé + chrono pendant la suppression, et
  protection contre le double-clic.
- **Base SQLite `emby_toolbox_web.db`** (remplace `emby_enrich_cache.json`,
  importé automatiquement puis renommé en `.json.imported`) :
  - âges / notes OMDB et TMDB enregistrés au fil de l'eau (rien n'est perdu si
    l'outil est fermé pendant un enrichissement) ;
  - Explorateur de genres : la colonne « Âge web » se remplit dès la fin du
    scan, sans appel réseau ; repli sur la base si OMDB/TMDB est indisponible ;
  - case **Forcer** (réinterroger OMDB/TMDB) et bouton **Base** (statistiques,
    vidage) ;
  - validité : 365 jours (film classé), 30 jours (non classé) ;
  - IDFinder (RefMatch) : les recherches TMDB sont aussi enregistrées (30 jours).
- **Doublons – « Ouvrir tout » :** timecode de départ réglé en amont dans le
  champ « Départ Ouvrir tout » (pas de popup), transmis à MPC-HC / MPC-BE
  (`/start`), VLC (`--start-time`), mpv (`--start`) et PotPlayer (`/seek`).
  Mémorisé automatiquement.
- **Lecteur vidéo :** lu dans le bandeau du haut (le champ caché de l'onglet
  Doublons n'était synchronisé qu'au clic sur « Enregistrer », d'où le message
  « lecteur non défini » alors que MPC-HC était renseigné). Guillemets acceptés,
  repli sur le lecteur associé par Windows puis sur les installations standard.
- **Explorateur de genres – âges en base :** un film déjà présent dans la base
  (même « non classé » ou ancien) n'est plus jamais recherché de nouveau sur
  OMDB/TMDB ; seule la case « Forcer » relance la recherche.
- **Explorateur de genres – récapitulatif avant application :** le bouton
  d'âge web de chaque ligne demande maintenant une validation (âge actuel →
  nouvel âge) ; « Appliquer âges sup. » affiche un tableau récapitulatif à
  cases à cocher (Tout cocher / Tout décocher, compteur).
- **IDFinder – fiche du candidat :** fenêtre détaillée ouverte sur le meilleur
  candidat après « Rechercher candidats » (+ bouton « Détails » par carte) :
  affiche, durée, genres, réalisation, acteurs, note, résumé, liens TMDB/IMDb,
  comparaison avec la fiche Emby. Détails TMDB enregistrés dans la base.
- **IDFinder – auto-correction :** analyse sans écriture puis récapitulatif à
  cases à cocher (sûrs cochés, douteux décochés) ; seuls les films cochés sont
  corrigés.
- **Configuration :** l'ancien `emby_toolbox_dpg.ini` (et les `.ini` des autres
  outils) est relu à chaque lancement : tout champ vide ou par défaut de
  `emby_toolbox_creds.ini` en est complété, sans rien écraser. Avant, il était
  ignoré dès que `emby_toolbox_creds.ini` existait (bandeau vide).
- **Chargement des médiathèques :** la progression affiche le nom de la
  médiathèque en cours (« Médiathèque 2/3 : Films 4K – Page 1 – 1 250 films »)
  dans les Doublons et l'Explorateur de genres.
- **Chemin UNC :** la valeur d'exemple `\\192.168.1.x` n'est plus mise dans le
  champ (seulement affichée en grisé) ; si elle y était restée, elle est
  remplacée par la valeur de l'ancien `.ini`.
- **Version Windows :** workflow GitHub Actions (build PyInstaller, auto-test,
  signature Azure Artifact Signing si configurée, release sur tag `v*`).
  Numéro de version affiché dans le titre de la fenêtre.

### English
- **Duplicates – deletion:** no more false "timeout". The wait for Emby's reply
  goes from 15 s to 180 s. If it is still exceeded, the tool checks with Emby
  for 90 s that the item is really gone before reporting an error. Animated
  spinner + timer while deleting, and double-click protection.
- **SQLite database `emby_toolbox_web.db`** (replaces `emby_enrich_cache.json`,
  imported automatically then renamed to `.json.imported`):
  - OMDB and TMDB ages / scores saved as they arrive (nothing is lost if the
    tool is closed during an enrichment);
  - Genre explorer: the "Web age" column is filled right after the scan, with
    no network call; falls back to the database when OMDB/TMDB is unavailable;
  - **Force** checkbox (query OMDB/TMDB again) and **DB** button (stats,
    clearing);
  - validity: 365 days (rated movie), 30 days (unrated);
  - IDFinder (RefMatch): TMDB searches are saved as well (30 days).
- **Duplicates – "Open all":** start timecode set beforehand in the
  "Open all start" field (no popup), passed to MPC-HC / MPC-BE (`/start`),
  VLC (`--start-time`), mpv (`--start`) and PotPlayer (`/seek`). Saved
  automatically.
- **Video player:** read from the top bar (the Duplicates tab's hidden field was
  only synced on "Save", hence the "player not set" message although MPC-HC was
  set). Quotes accepted, fallback to the Windows-associated player then to
  standard installs.
- **Genre explorer – ages in the database:** a movie already in the database
  (even "unrated" or old) is never looked up again on OMDB/TMDB; only the
  "Force" checkbox triggers a new lookup.
- **Genre explorer – summary before applying:** each row's web-age button now
  asks for confirmation (current age → new age); "Apply higher ages" shows a
  summary table with checkboxes (Tick all / Untick all, counter).
- **IDFinder – candidate details:** detailed window opened on the best
  candidate after "Search candidates" (+ "Details" button per card): poster,
  runtime, genres, director, cast, rating, overview, TMDB/IMDb links,
  comparison with the Emby record. TMDB details saved in the database.
- **IDFinder – auto-fix:** no-write analysis then a checkbox summary (safe ones
  ticked, doubtful ones unticked); only ticked movies are fixed.
- **Configuration:** the old `emby_toolbox_dpg.ini` (and the other tools'
  `.ini` files) is read at every start: any empty or default field of
  `emby_toolbox_creds.ini` is filled from it, nothing is overwritten. Before,
  it was ignored as soon as `emby_toolbox_creds.ini` existed (empty top bar).
- **Library loading:** progress shows the name of the library being read
  ("Library 2/3: Films 4K – Page 1 – 1,250 movies") in Duplicates and the Genre
  explorer.
- **UNC path:** the example value `\\192.168.1.x` is no longer put in the field
  (only shown greyed out); if it was left there, it is replaced by the value
  from the old `.ini`.
- **Windows build:** GitHub Actions workflow (PyInstaller build, self-test,
  Azure Artifact Signing when configured, release on `v*` tag). Version number
  shown in the window title.
