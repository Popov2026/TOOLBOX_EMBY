# Changelog

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
- **Windows build:** GitHub Actions workflow (PyInstaller build, self-test,
  Azure Artifact Signing when configured, release on `v*` tag). Version number
  shown in the window title.
