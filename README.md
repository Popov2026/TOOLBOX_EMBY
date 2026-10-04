# Emby Toolbox

**[Français](#français) · [English](#english)**

Five tools for an [Emby](https://emby.media) server in a single tabbed window
(Python + Dear PyGui). One script: `emby_toolbox_dpg.pyw`.

Cinq outils pour un serveur [Emby](https://emby.media) dans une seule fenêtre à
onglets (Python + Dear PyGui). Un seul script : `emby_toolbox_dpg.pyw`.

---

## Français

### Bilingue FR / EN
L'interface est **entièrement bilingue français / anglais**. Le bouton de langue
du bandeau du haut retraduit les cinq onglets, les infobulles et les messages,
sans redémarrer. Le choix est mémorisé.

### Les cinq onglets

| Onglet | À quoi il sert |
|---|---|
| **IDFinder** (RefMatch) | Repère les films / séries mal identifiés et les ré-identifie |
| **Doublons** | Détecte les doublons et permet de les comparer, lire, supprimer |
| **Explorateur de genres** | Liste les films par genre et contrôle les âges (classification) |
| **MKV Renamer** | Renomme les fichiers vidéo selon leur contenu réel |
| **Sessions** | Qui est connecté, qui regarde quoi, en direct |

#### IDFinder (RefMatch)
- Scanne les médiathèques sélectionnées (**films ou séries**) et liste les
  éléments **sans identifiant TMDB/IMDB**, **sans visuel**, ou tous.
- **Recherche manuelle** : candidats TMDB (avec affiches) puis, en secours,
  la recherche distante d'Emby ; tolérance d'année réglable ; application du
  bon résultat en un clic.
- **Fiche du candidat** : à la fin de la recherche, une fenêtre s'ouvre sur le
  meilleur candidat (bouton **Détails** pour les autres) : grande affiche,
  titre original, sortie, durée, genres, réalisation, acteurs, pays, note TMDB,
  résumé complet, liens TMDB / IMDb, et rappel de la fiche Emby actuelle pour
  comparer. Bouton « Appliquer ce candidat ».
- **Auto-correction** en deux temps : analyse **sans aucune écriture**, puis
  **récapitulatif à valider** avec une case à cocher par film. Les
  correspondances sûres (titre ≥ 95 %, année dans la tolérance) sont cochées,
  les douteuses décochées ; « Détails » ouvre la fiche de chaque proposition.
  Seuls les films cochés sont corrigés, après « Appliquer la sélection ».
- **Scan des fiches fusionnées** : repère les fiches Emby qui regroupent
  plusieurs fichiers de films différents (fusion suspecte) ; export possible.
- Les recherches TMDB sont enregistrées dans la base `emby_toolbox_web.db`.

#### Doublons
- Détection par **IMDB (100 %)**, **TMDB (85 %)**, **titre (60 %)** et
  **similarité floue (40 %)**, avec seuil de score.
- Sépare les **vrais doublons** des **versions intentionnelles** selon des
  critères cochables : 4K/1080p, HDR/SDR, AV1, 3D, Remastered, version
  longue / Director's Cut, bonus.
- Filtres (résolution, codec, même durée, même taille), tris, pagination.
- Par groupe : **Ouvrir tout** (fenêtres du lecteur en mosaïque, toutes les
  versions démarrent au **timecode réglé dans le champ « Départ Ouvrir
  tout »** de la barre de l'onglet, sans popup), **Comparer** (métadonnées côte à côte, différences surlignées),
  **Audio** (pistes audio côte à côte), **Ignorer** (faux positif).
- Par fichier : lire, ouvrir le dossier, copier le chemin, **supprimer** via
  l'API Emby (retirer de la médiathèque ou supprimer le fichier), avec
  confirmation. Les suppressions lentes sur NAS ne déclenchent plus de faux
  timeout.
- Sauvegarde / rechargement du scan (hors ligne), export **HTML** ou **CSV**,
  rafraîchissement des médiathèques Emby.

#### Explorateur de genres
- Liste les films d'un ou plusieurs genres (ou d'un genre libre) dans les
  médiathèques choisies : titre, année, taille, chemin, âge enregistré.
- **Âge web** : récupère classification d'âge et note via **OMDB** ou **TMDB**
  (repli automatique sur l'autre source si la première ne classe pas le film).
- Les résultats sont gardés dans la base **`emby_toolbox_web.db`** : affichage
  dès la fin du scan, aucun quota consommé pour les films déjà connus, valeurs
  disponibles même si OMDB/TMDB est en panne. Case **Forcer** pour tout
  réinterroger, bouton **Base** pour voir / vider la base.
- Modifier l'âge d'un film, **appliquer en masse** les âges web supérieurs,
  **annuler** la dernière application en masse, masquer les âges identiques,
  filtrer par âge.
- Ouvrir le fichier / le dossier, export CSV.

#### MKV Renamer
- Analyse les fichiers d'un dossier avec **ffprobe** et propose un nom
  normalisé : `Titre.Année.Langue.Résolution.Source.Codec.ext`
  (ex. `Dune.2021.MULTI.2160p.WEB-DL.X265.mkv`).
- Détection de la langue des pistes, de la résolution, du codec et de la
  source probable ; tout est modifiable ligne par ligne.
- Années manquantes complétées via **TMDB** (cache SQLite local) ; les années
  lues dans le nom ou saisies à la main ne sont pas écrasées (option).
- Renomme aussi les fichiers compagnons (sous-titres `.srt`, `.nfo`, images…), avec **journal et
  annulation** du dernier lot.
- Auto-test : `python emby_toolbox_dpg.pyw --mkv-selftest`.

#### Sessions
- Tableau rafraîchi automatiquement (intervalle réglable) : utilisateur,
  appareil, IP, état, contenu, progression, mode de lecture
  (direct / remux / transcodage), inactivité, détection des sessions
  « fantômes ».
- Journal d'activité du serveur.
- Actions à distance : **pause / reprise / arrêt** et **message à l'écran**.

### Ce dont le script a besoin

#### 1. Système
- **Windows 10/11 recommandé.** Le script tourne aussi sous Linux/macOS, mais
  la mosaïque des fenêtres du lecteur, le chiffrement DPAPI et « ouvrir dans
  l'explorateur » sont propres à Windows.
- **Python 3.9 ou plus récent** (avec `tkinter`, inclus dans l'installeur
  Windows de python.org), GPU compatible DirectX 11 (Dear PyGui).

#### 2. Bibliothèques Python
```
pip install -r requirements.txt
```
| Paquet | Rôle | Obligatoire |
|---|---|---|
| `dearpygui` | Interface graphique | oui |
| `requests` | Appels HTTP (IDFinder) | oui |
| `pillow` | Affiches dans IDFinder | non (pas d'affiches sans) |
| `cryptography` | Chiffrement des clés hors Windows | non sous Windows (DPAPI) |

#### 3. Programmes externes (.exe)
| Programme | Utilisé par | Notes |
|---|---|---|
| **ffprobe.exe** (fourni avec **FFmpeg**) | MKV Renamer | Obligatoire pour cet onglet. Détection auto : dossier du script, `bin\`, `ffmpeg\bin\`, `C:\ffmpeg\bin`, `C:\Program Files\ffmpeg\bin`, WinGet, Scoop, Chocolatey, PATH. Sinon : bouton « Choisir ffprobe… ». Téléchargement : <https://ffmpeg.org/download.html> (ou `winget install ffmpeg`). |
| **Lecteur vidéo** : **MPC-HC**, **MPC-BE**, **VLC**, **mpv** ou **PotPlayer** | Doublons, Explorateur | Chemin du `.exe` dans le champ « Lecteur » (ex. `C:\Program Files\MPC-HC\mpc-hc64.exe`). Le chemin est lu dans le bandeau du haut (guillemets acceptés). S'il est vide ou invalide, l'outil prend le lecteur associé aux `.mkv` par Windows, puis une installation standard de MPC-HC / MPC-BE / VLC / PotPlayer. |

> Pour « Ouvrir tout », le lecteur doit accepter **plusieurs instances** :
> VLC : *Préférences > Interface > décocher « Une seule instance »* ;
> MPC-HC / MPC-BE : *Options > Lecteur > « Permettre plusieurs instances »*.

#### 4. Clés API et paramètres (bandeau du haut, communs aux 5 onglets)
| Paramètre | Où l'obtenir | Utilisé par |
|---|---|---|
| **URL Emby** | ex. `http://192.168.1.10:8096` | tous |
| **Clé API Emby** | Emby : *Tableau de bord > Avancé > Clés API > Nouvelle clé*. Une clé **administrateur** est nécessaire pour supprimer, modifier les âges, ré-identifier, piloter les sessions. | tous |
| **User ID Emby** (optionnel) | Emby : *Tableau de bord > Utilisateurs*, identifiant dans l'URL du profil | Explorateur |
| **Clé TMDB** (gratuite, « API Key » v3) | <https://www.themoviedb.org/settings/api> | IDFinder, Explorateur, MKV Renamer |
| **Clé OMDB** (gratuite, 1000 req./jour) | <https://www.omdbapi.com/apikey.aspx> | Explorateur (âge web) |
| **Préfixe NAS + chemin UNC** | ex. `/volume1` → `\\192.168.1.29` : traduit les chemins Linux vus par Emby en chemins Windows | Doublons, Explorateur (lecture, dossiers) |

Les clés sont **chiffrées** sur disque (DPAPI sous Windows, sinon Fernet avec
`emby_secret.key`).

### Installation et lancement
```
pip install -r requirements.txt
pythonw emby_toolbox_dpg.pyw        (sans console)
python  emby_toolbox_dpg.pyw        (avec console, pour diagnostiquer)
```
Puis renseigner le bandeau du haut, **Enregistrer**, **Connecter**.

### Fichiers créés à côté du script
| Fichier | Contenu |
|---|---|
| `emby_toolbox_creds.ini` | Paramètres communs (clés chiffrées) |
| `emby_secret.key` | Clé de chiffrement locale (hors DPAPI) — ne pas partager |
| `emby_toolbox_dpg.ini`, `emby_toolbox_dpg_api_config.json`, `emby_refmatch.ini` | Réglages des onglets |
| `emby_toolbox_web.db` | **Base OMDB/TMDB** (âges, notes, recherches) |
| `emby_toolbox_mkv_cache.db`, `emby_toolbox_mkvrename.ini` | Cache TMDB et réglages du MKV Renamer |
| `emby_toolbox_sessions.ini` | Réglages de l'onglet Sessions |
| `emby_toolbox_dpg_resultats.json`, `emby_toolbox_dpg_ignores.json` | Scan de doublons sauvegardé, groupes ignorés |
| `emby_toolbox_timecode.json` | Dernier timecode de « Ouvrir tout » |
| `*_debug.log`, `emby_toolbox_trace.log` | Journaux de diagnostic |

Si le dossier du script n'est pas accessible en écriture, les fichiers du
MKV Renamer et des Sessions vont dans `%APPDATA%\EmbyToolbox`.

L'historique des changements est dans [CHANGELOG.md](CHANGELOG.md).

---

## English

### Bilingual FR / EN
The interface is **fully bilingual French / English**. The language button in
the top bar re-translates all five tabs, tooltips and messages without a
restart. The choice is remembered.

### The five tabs

| Tab | What it does |
|---|---|
| **IDFinder** (RefMatch) | Finds badly identified movies / series and re-identifies them |
| **Duplicates** | Detects duplicates and lets you compare, play, delete them |
| **Genre explorer** | Lists movies by genre and checks age ratings |
| **MKV Renamer** | Renames video files from their actual content |
| **Sessions** | Who is connected, who is watching what, live |

#### IDFinder (RefMatch)
- Scans the selected libraries (**movies or series**) and lists items **with no
  TMDB/IMDB id**, **with no artwork**, or everything.
- **Manual search**: TMDB candidates (with posters), then Emby's remote search
  as a fallback; adjustable year tolerance; apply the right match in one click.
- **Candidate details**: when the search ends, a window opens on the best
  candidate (**Details** button for the others): large poster, original title,
  release, runtime, genres, director, cast, country, TMDB rating, full overview,
  TMDB / IMDb links, and the current Emby record for comparison. "Apply this
  match" button.
- **Auto-fix** in two steps: analysis **with no write at all**, then a
  **summary to validate** with one checkbox per movie. Safe matches (title
  ≥ 95 %, year within tolerance) are ticked, doubtful ones unticked; "Details"
  opens each proposal's record. Only ticked movies are fixed, after
  "Apply selection".
- **Merged-entries scan**: finds Emby entries that group several files of
  different movies (suspicious merge); exportable.
- TMDB searches are saved in the `emby_toolbox_web.db` database.

#### Duplicates
- Detection by **IMDB (100 %)**, **TMDB (85 %)**, **title (60 %)** and
  **fuzzy similarity (40 %)**, with a score threshold.
- Separates **real duplicates** from **intentional versions** using checkable
  criteria: 4K/1080p, HDR/SDR, AV1, 3D, Remastered, extended / Director's Cut,
  bonus.
- Filters (resolution, codec, same duration, same size), sorting, paging.
- Per group: **Open all** (player windows tiled, every version starts at
  the **timecode set in the "Open all start" field** of the tab bar, no popup), **Compare** (metadata side by side,
  differences highlighted), **Audio** (audio tracks side by side), **Ignore**
  (false positive).
- Per file: play, open folder, copy path, **delete** through the Emby API
  (remove from library or delete the file), with confirmation. Slow deletions
  on a NAS no longer raise a false timeout.
- Save / reload the scan (offline), **HTML** or **CSV** export, Emby library
  refresh.

#### Genre explorer
- Lists the movies of one or more genres (or a free-text genre) in the chosen
  libraries: title, year, size, path, recorded age rating.
- **Web age**: fetches age rating and score from **OMDB** or **TMDB**
  (automatic fallback to the other source when the first one has no rating).
- Results are kept in the **`emby_toolbox_web.db`** database: shown right after
  the scan, no quota used for already known movies, values still available when
  OMDB/TMDB is down. **Force** checkbox to query everything again, **DB**
  button to view / clear the database.
- Edit a movie's age rating, **bulk-apply** higher web ages, **undo** the last
  bulk apply, hide identical ages, filter by age.
- Open file / folder, CSV export.

#### MKV Renamer
- Analyses the files of a folder with **ffprobe** and suggests a normalised
  name: `Title.Year.Language.Resolution.Source.Codec.ext`
  (e.g. `Dune.2021.MULTI.2160p.WEB-DL.X265.mkv`).
- Detects track languages, resolution, codec and likely source; everything can
  be edited row by row.
- Missing years filled in from **TMDB** (local SQLite cache); years read from
  the name or typed by hand are not overwritten (optional).
- Also renames companion files (`.srt` subtitles, `.nfo`, images…), with a **journal and undo**
  of the last batch.
- Self-test: `python emby_toolbox_dpg.pyw --mkv-selftest`.

#### Sessions
- Auto-refreshed table (adjustable interval): user, device, IP, state, content,
  progress, play mode (direct / remux / transcode), idle time, "ghost" session
  detection.
- Server activity log.
- Remote actions: **pause / resume / stop** and **on-screen message**.

### What the script needs

#### 1. System
- **Windows 10/11 recommended.** The script also runs on Linux/macOS, but
  player window tiling, DPAPI encryption and "show in Explorer" are
  Windows-only.
- **Python 3.9 or newer** (with `tkinter`, included in the python.org Windows
  installer), a DirectX 11 capable GPU (Dear PyGui).

#### 2. Python libraries
```
pip install -r requirements.txt
```
| Package | Purpose | Required |
|---|---|---|
| `dearpygui` | GUI | yes |
| `requests` | HTTP calls (IDFinder) | yes |
| `pillow` | Posters in IDFinder | no (no posters without it) |
| `cryptography` | Key encryption outside Windows | no on Windows (DPAPI) |

#### 3. External programs (.exe)
| Program | Used by | Notes |
|---|---|---|
| **ffprobe.exe** (ships with **FFmpeg**) | MKV Renamer | Required for this tab. Auto-detected in: script folder, `bin\`, `ffmpeg\bin\`, `C:\ffmpeg\bin`, `C:\Program Files\ffmpeg\bin`, WinGet, Scoop, Chocolatey, PATH. Otherwise: "Choose ffprobe…" button. Download: <https://ffmpeg.org/download.html> (or `winget install ffmpeg`). |
| **Video player**: **MPC-HC**, **MPC-BE**, **VLC**, **mpv** or **PotPlayer** | Duplicates, Genre explorer | Path of the `.exe` in the "Player" field (e.g. `C:\Program Files\MPC-HC\mpc-hc64.exe`). The path is read from the top bar (quotes accepted). If it is empty or invalid, the tool uses the player Windows associates with `.mkv`, then a standard MPC-HC / MPC-BE / VLC / PotPlayer install. |

> For "Open all", the player must allow **multiple instances**:
> VLC: *Preferences > Interface > uncheck "Allow only one instance"*;
> MPC-HC / MPC-BE: *Options > Player > "Allow multiple instances"*.

#### 4. API keys and settings (top bar, shared by the 5 tabs)
| Setting | Where to get it | Used by |
|---|---|---|
| **Emby URL** | e.g. `http://192.168.1.10:8096` | all |
| **Emby API key** | Emby: *Dashboard > Advanced > API Keys > New key*. An **administrator** key is needed to delete, edit age ratings, re-identify, control sessions. | all |
| **Emby User ID** (optional) | Emby: *Dashboard > Users*, id in the profile URL | Genre explorer |
| **TMDB key** (free, v3 "API Key") | <https://www.themoviedb.org/settings/api> | IDFinder, Genre explorer, MKV Renamer |
| **OMDB key** (free, 1000 requests/day) | <https://www.omdbapi.com/apikey.aspx> | Genre explorer (web age) |
| **NAS prefix + UNC path** | e.g. `/volume1` → `\\192.168.1.29`: maps the Linux paths seen by Emby to Windows paths | Duplicates, Genre explorer (playback, folders) |

Keys are **encrypted** on disk (DPAPI on Windows, otherwise Fernet with
`emby_secret.key`).

### Install and run
```
pip install -r requirements.txt
pythonw emby_toolbox_dpg.pyw        (no console)
python  emby_toolbox_dpg.pyw        (with console, for troubleshooting)
```
Then fill in the top bar, **Save**, **Connect**.

### Files created next to the script
| File | Content |
|---|---|
| `emby_toolbox_creds.ini` | Shared settings (encrypted keys) |
| `emby_secret.key` | Local encryption key (non-DPAPI) — do not share |
| `emby_toolbox_dpg.ini`, `emby_toolbox_dpg_api_config.json`, `emby_refmatch.ini` | Tab settings |
| `emby_toolbox_web.db` | **OMDB/TMDB database** (ages, scores, searches) |
| `emby_toolbox_mkv_cache.db`, `emby_toolbox_mkvrename.ini` | MKV Renamer TMDB cache and settings |
| `emby_toolbox_sessions.ini` | Sessions tab settings |
| `emby_toolbox_dpg_resultats.json`, `emby_toolbox_dpg_ignores.json` | Saved duplicate scan, ignored groups |
| `emby_toolbox_timecode.json` | Last "Open all" timecode |
| `*_debug.log`, `emby_toolbox_trace.log` | Diagnostic logs |

If the script folder is not writable, the MKV Renamer and Sessions files go to
`%APPDATA%\EmbyToolbox`.

The change history is in [CHANGELOG.md](CHANGELOG.md).
