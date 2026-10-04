# Changelog

## 2026-10

### Français
- **Doublons – suppression :** fin des faux « timeout ». Le délai d'attente de la
  réponse d'Emby passe de 15 s à 180 s. S'il est quand même dépassé, l'outil
  vérifie pendant 90 s auprès d'Emby que l'élément a bien disparu avant de
  signaler une erreur. Statut « Suppression en cours… » et protection contre le
  double-clic.
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

### English
- **Duplicates – deletion:** no more false "timeout". The wait for Emby's reply
  goes from 15 s to 180 s. If it is still exceeded, the tool checks with Emby
  for 90 s that the item is really gone before reporting an error. "Deleting…"
  status and double-click protection.
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
