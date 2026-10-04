#!/usr/bin/env pythonw
# -*- coding: utf-8 -*-
"""
Emby Toolbox  -  cinq outils dans une seule fenetre a onglets :
  - Explorateur de genres
  - RefMatch (verification / re-referencement, films ET series)
  - Doublons (detecteur de doublons, lecture seule)
  - MKV Renamer (renommage des fichiers : Titre.Annee.Langue.Res.Source.Codec)
  - Sessions (qui est connecte / qui lit quoi, rafraichi automatiquement)

Clefs API communes aux cinq outils (magasin chiffre unique
emby_toolbox_creds.ini, chiffrement DPAPI/Fernet via emby_secret.key) :
la cle TMDB du bandeau du haut sert aussi bien a RefMatch, a
l'enrichissement des genres qu'au MKV Renamer.
Toutes les modifications UI issues d'un thread passent par _ui_queue,
videe une fois par frame dans la boucle principale.
La bascule FR/EN du bandeau commun retraduit les cinq onglets.

Version 2026-10 :
  - Doublons : suppression sans faux timeout (attente 180 s puis
    verification de la disparition de l'item cote Emby).
  - Base SQLite emby_toolbox_web.db : sauvegarde des ages/notes OMDB-TMDB
    (explorateur de genres) et des recherches TMDB (RefMatch).
  - Doublons : "Ouvrir tout" demande un timecode de depart
    (MPC-HC/MPC-BE, VLC, mpv, PotPlayer).

Lignes de commande utiles :
    python emby_toolbox_dpg.pyw --mkv-selftest   tests du moteur de renommage
"""

import dearpygui.dearpygui as dpg
import threading, queue, json, os, sys, subprocess, time, socket
import re, configparser, csv, difflib, base64, traceback, webbrowser
import sqlite3, unicodedata, warnings
from pathlib import Path
from io import BytesIO
from difflib import SequenceMatcher
import urllib.request, urllib.parse, urllib.error

# requests rale des l'import quand urllib3 / chardet / charset_normalizer sortent
# de la plage de versions qu'il declare (RequestsDependencyWarning). C'est sans
# consequence ici, mais ca polluait la console au lancement : on le tait.
warnings.filterwarnings("ignore", message=r".*doesn't match a supported version.*")
try:
    import requests
except Exception:
    requests = None

try:
    from PIL import Image
    PIL_OK = True
except Exception:
    PIL_OK = False

# =====================================================================
#  Chiffrement partage (clef unique : emby_secret.key)
# =====================================================================
_SECRET_KEYFILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "emby_secret.key")

try:
    from cryptography.fernet import Fernet
    _CRYPTO_OK = True
except Exception:
    _CRYPTO_OK = False

_FERNET = None
_IS_WIN = sys.platform.startswith("win")

if _IS_WIN:
    import ctypes
    from ctypes import wintypes

    class _DATA_BLOB(ctypes.Structure):
        _fields_ = [("cbData", wintypes.DWORD),
                    ("pbData", ctypes.POINTER(ctypes.c_char))]

    def _dpapi(data, fn):
        buf = ctypes.create_string_buffer(data, len(data))
        blob_in = _DATA_BLOB(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char)))
        blob_out = _DATA_BLOB()
        ok = fn(ctypes.byref(blob_in), None, None, None, None, 0, ctypes.byref(blob_out))
        if not ok:
            raise OSError("DPAPI a échoué")
        try:
            return ctypes.string_at(blob_out.pbData, blob_out.cbData)
        finally:
            ctypes.windll.kernel32.LocalFree(blob_out.pbData)

    def _dpapi_encrypt(data):
        return _dpapi(data, ctypes.windll.crypt32.CryptProtectData)

    def _dpapi_decrypt(data):
        return _dpapi(data, ctypes.windll.crypt32.CryptUnprotectData)


def _fernet():
    global _FERNET
    if _FERNET is None:
        if os.path.exists(_SECRET_KEYFILE):
            key = open(_SECRET_KEYFILE, "rb").read().strip()
        else:
            key = Fernet.generate_key()
            with open(_SECRET_KEYFILE, "wb") as fh:
                fh.write(key)
            try:
                os.chmod(_SECRET_KEYFILE, 0o600)
            except Exception:
                pass
        _FERNET = Fernet(key)
    return _FERNET


def crypto_method():
    if _IS_WIN:
        return "dpapi"
    if _CRYPTO_OK:
        return "fernet"
    return "clair"


def encrypt_secret(plain):
    """Chiffre une clé. Renvoie 'enc:<methode>:...' ou le clair en dernier recours."""
    if not plain or plain.startswith("enc:"):
        return plain or ""
    if _IS_WIN:
        try:
            blob = _dpapi_encrypt(plain.encode("utf-8"))
            return "enc:dpapi:" + base64.b64encode(blob).decode("ascii")
        except Exception:
            pass
    if _CRYPTO_OK:
        try:
            return "enc:fernet:" + _fernet().encrypt(plain.encode("utf-8")).decode("ascii")
        except Exception:
            pass
    return plain  # aucun moyen de chiffrer disponible


def decrypt_secret(stored):
    """Déchiffre une valeur du .ini. Renvoie '' si le déchiffrement échoue."""
    if not stored:
        return ""
    if stored.startswith("enc:dpapi:"):
        if not _IS_WIN:
            return ""
        try:
            return _dpapi_decrypt(base64.b64decode(stored[10:])).decode("utf-8")
        except Exception:
            return ""
    if stored.startswith("enc:fernet:"):
        if not _CRYPTO_OK:
            return ""
        try:
            return _fernet().decrypt(stored[11:].encode("ascii")).decode("utf-8")
        except Exception:
            return ""
    return stored  # valeur en clair (ancien format)


# =====================================================================
#  File UI partagee
# =====================================================================
_ui_queue = queue.Queue()

def ui(fn):
    _ui_queue.put(fn)

def ui_post(fn):
    _ui_queue.put(fn)

def drain_ui_queue():
    while True:
        try:
            fn = _ui_queue.get_nowait()
        except queue.Empty:
            break
        try:
            try:
                fn()
            except Exception:
                pass   # un callback UI defaillant ne doit jamais tuer l'application
        except Exception:
            traceback.print_exc()

# =====================================================================
#  Identifiants partages entre les quatre outils
# =====================================================================
def _app_dir():
    """Dossier de l'application : a cote de l'exe si gele (PyInstaller), sinon du script."""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent

APP_DIR = _app_dir()
APP_STEM = "emby_toolbox_dpg"
SHARED_CREDS_FILE = APP_DIR / "emby_toolbox_creds.ini"
_SHARED_FIELDS = ["url", "api_key", "user_id", "nas_prefix", "nas_unc",
                  "player", "omdb_key", "tmdb_key", "provider", "lang"]
_SHARED_SECRET = {"api_key", "omdb_key", "tmdb_key"}
_shared_cache = None

def _shared_defaults():
    return {"url": "http://localhost:8096", "api_key": "", "user_id": "",
            "nas_prefix": "/volume1", "nas_unc": r"\\\\192.168.1.x",
            "player": "", "omdb_key": "", "tmdb_key": "", "provider": "tmdb",
            "lang": "FR"}

def load_shared_creds():
    global _shared_cache
    d = _shared_defaults()
    if SHARED_CREDS_FILE.exists():
        cfg = configparser.ConfigParser()
        try:
            cfg.read(SHARED_CREDS_FILE, encoding="utf-8")
            if cfg.has_section("emby"):
                for k in _SHARED_FIELDS:
                    v = cfg["emby"].get(k)
                    if v:
                        d[k] = v
        except Exception:
            pass
        for k in _SHARED_SECRET:
            d[k] = decrypt_secret(d.get(k, ""))
    _shared_cache = d
    return d

def get_shared_creds():
    return _shared_cache if _shared_cache is not None else load_shared_creds()

def save_shared_creds(**updates):
    global _shared_cache
    d = dict(get_shared_creds())
    for k, v in updates.items():
        if k in _SHARED_FIELDS and v is not None and str(v) != "":
            d[k] = v
    _shared_cache = d
    out = dict(d)
    for k in _SHARED_SECRET:
        out[k] = encrypt_secret(d.get(k, ""))
    cfg = configparser.ConfigParser(); cfg["emby"] = out
    try:
        with open(SHARED_CREDS_FILE, "w", encoding="utf-8") as f:
            cfg.write(f)
    except Exception:
        pass
    return d

_TRACE_FILE = Path(__file__).with_name("emby_toolbox_trace.log")


def _trace(msg):
    """Trace des erreurs autrement invisibles (lance en .pyw : pas de console)."""
    line = "[%s] %s" % (__import__("time").strftime("%d/%m %H:%M:%S"), msg)
    try:
        with open(_TRACE_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass
    print(line)


_SHARED_PUSHERS = []

def register_shared_pusher(fn):
    _SHARED_PUSHERS.append(fn)

def push_shared_to_all_tabs():
    c = get_shared_creds()
    for fn in list(_SHARED_PUSHERS):
        try:
            fn(c)
        except Exception as exc:
            # un pusher defaillant laissait un onglet sans identifiants,
            # silencieusement : on trace au lieu d'ignorer.
            _trace("push %s : %s" % (getattr(fn, "__name__", fn), exc))

# =====================================================================
#  OUTIL 1 : Explorateur de genres
# =====================================================================
CONFIG_FILE = APP_DIR / (APP_STEM + ".ini")

# Cherche aussi le .ini du script doublons dans le même dossier
_sib = APP_DIR / "emby_doublons_dpg.ini"
if not CONFIG_FILE.exists() and _sib.exists():
    CONFIG_FILE = _sib

def load_config():
    cfg = configparser.ConfigParser()
    cfg["emby"] = {"url":"http://localhost:8096","api_key":"","user_id":"",
                   "nas_prefix":"/volume1","nas_unc":r"\\192.168.1.x","player":""}
    if CONFIG_FILE.exists():
        cfg.read(CONFIG_FILE, encoding="utf-8")
    if cfg.has_section("emby"):
        cfg["emby"]["api_key"] = decrypt_secret(cfg["emby"].get("api_key", ""))
    return cfg

def save_config(d):
    p = APP_DIR / (APP_STEM + ".ini")
    d = dict(d)
    try:
        save_shared_creds(url=d.get("url", ""), api_key=d.get("api_key", ""),
                          user_id=d.get("user_id", ""),
                          nas_prefix=d.get("nas_prefix", ""),
                          nas_unc=d.get("nas_unc", ""), player=d.get("player", ""))
        push_shared_to_all_tabs()
    except Exception:
        pass
    d["api_key"] = encrypt_secret(d.get("api_key", ""))
    cfg = configparser.ConfigParser(); cfg["emby"] = d
    with open(p,"w",encoding="utf-8") as f: cfg.write(f)

# ══════════════════════════════════════════════════════════════
#  CONFIG API ENRICHISSEMENT  (JSON séparé)
# ══════════════════════════════════════════════════════════════
API_CFG_FILE = APP_DIR / (APP_STEM + "_api_config.json")

_API_CFG_DEFAULTS = {
    "provider": "omdb",   # "omdb" ou "tmdb"
    "omdb_key": "",
    "tmdb_key": "",
}

def load_api_config():
    if API_CFG_FILE.exists():
        try:
            d = json.loads(API_CFG_FILE.read_text("utf-8"))
            cfg = dict(_API_CFG_DEFAULTS); cfg.update(d)
            cfg["omdb_key"] = decrypt_secret(cfg.get("omdb_key", ""))
            cfg["tmdb_key"] = decrypt_secret(cfg.get("tmdb_key", ""))
            return cfg
        except Exception:
            pass
    return dict(_API_CFG_DEFAULTS)

def save_api_config(d):
    d = dict(d)
    d["omdb_key"] = encrypt_secret(d.get("omdb_key", ""))
    d["tmdb_key"] = encrypt_secret(d.get("tmdb_key", ""))
    API_CFG_FILE.write_text(json.dumps(d, ensure_ascii=False, indent=2), "utf-8")

API_CFG = load_api_config()


# ══════════════════════════════════════════════════════════════
#  API EMBY
# ══════════════════════════════════════════════════════════════
def emby_get(base, key, path, params=None):
    p = dict(params or {}); p["api_key"] = key
    url = f"{base.rstrip('/')}{path}?{urllib.parse.urlencode(p)}"
    req = urllib.request.Request(url, headers={"Accept":"application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode())


def emby_post(base, key, path, body):
    """POST vers Emby avec les deux formes d'auth (query param + header)."""
    data = json.dumps(body).encode("utf-8")
    url  = f"{base.rstrip('/')}{path}?api_key={key}"
    req  = urllib.request.Request(url, data=data, method="POST",
           headers={
               "Content-Type":    "application/json",
               "Accept":          "application/json",
               "X-Emby-Authorization": (
                   f'MediaBrowser Client="EmbyGenreExplorer",'
                   f'Device="PC",DeviceId="emby-genre-1",Version="1.0",'
                   f'Token="{key}"'),
           })
    with urllib.request.urlopen(req, timeout=15) as r:
        return r.status   # 204 = succes Emby


def open_file(win_path, player=""):
    if not win_path:
        return
    try:
        if player and Path(player).exists():
            subprocess.Popen([player, win_path])
        elif sys.platform == "win32":
            os.startfile(win_path)
        else:
            subprocess.Popen(["xdg-open", win_path])
    except Exception as e:
        ui(lambda err=e: modal_err(gx("Erreur lecture","Read error"), str(err)))


def get_player():
    try:    return (dpg.get_value("inp_player") or "").strip()
    except: return ""


LOG_FILE = APP_DIR / (APP_STEM + "_debug.log")

def _log(msg):
    """Ecrit dans le fichier log ET dans la console."""
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line)
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


def do_set_rating(item_id, title, current_rating, conn):
    global _mid; _mid += 1
    win_tag = f"agewin{_mid}"
    inp_tag = f"ageinp{_mid}"

    with dpg.window(label=f"Modifier age  --  {title}", tag=win_tag,
                    modal=True, width=480, height=160, pos=[200,220], no_resize=True):
        dpg.add_text("Nouvelle classification (ex: 12, PG-13, R, 16, 18) :")
        dpg.add_input_text(tag=inp_tag, default_value=current_rating,
                           width=-1, hint="PG-13 / 12 / 16 / R ...")
        dpg.add_separator()

        def do_save(_s=None, _a=None, _u=None,
                    wt=win_tag, it=inp_tag, iid=item_id, c=conn):
            nr = (dpg.get_value(it) or "").strip()
            if not nr:
                return
            dpg.delete_item(wt)
            _log(f"=== do_set_rating : iid={iid} titre={title!r} nr={nr!r} ===")

            def thread(nr=nr, iid=iid, c=c):
                try:
                    uid = (c.get("uid") or "").strip()

                    # ── GET via /Items?Ids= (endpoint liste, toujours dispo) ───
                    get_params = {
                        "Ids":    iid,
                        "Fields": "Path,MediaSources,ProductionYear,Genres,"
                                  "OfficialRating,ProviderIds,Name,Overview,"
                                  "DateCreated,CommunityRating,Tags,Studios,People",
                    }
                    if uid:
                        get_params["UserId"] = uid
                    get_url = f"{c['url'].rstrip('/')}/Items?Ids={iid}&api_key=***"
                    _log(f"GET {get_url}")

                    data = emby_get(c["url"], c["key"], "/Items", get_params)
                    items = data.get("Items", [])
                    if not items:
                        raise ValueError("Item introuvable (liste vide).")
                    full = items[0]

                    _log(f"GET OK  Id={full.get('Id','')}  "
                         f"Name={full.get('Name','')!r}  "
                         f"OfficialRating actuel={full.get('OfficialRating','(vide)')!r}")

                    # ── POST ─────────────────────────────────────
                    full["OfficialRating"] = nr
                    post_url = f"{c['url'].rstrip('/')}/Items/{iid}?api_key=***"
                    _log(f"POST {post_url}  body[OfficialRating]={nr!r}")

                    status = emby_post(c["url"], c["key"], f"/Items/{iid}", full)
                    _log(f"POST reponse HTTP : {status}")

                    if status not in (200, 204):
                        raise ValueError(f"HTTP {status} inattendu (200 ou 204 attendu)")

                    _log("SUCCES")
                    for r in G["results"]:
                        if r.get("item_id") == iid:
                            r["rating"] = nr

                    ui(lambda: (
                        _rebuild_age_filter(),
                        render_results(),
                        modal_info(gx("Âge mis à jour","Age updated"),
                            gx(f"{title}\n\nOfficialRating => {nr!r}\nHTTP {status}\n\nLog complet : {LOG_FILE}",
                               f"{title}\n\nOfficialRating => {nr!r}\nHTTP {status}\n\nFull log: {LOG_FILE}"))))

                except urllib.error.HTTPError as e:
                    body = ""
                    try: body = e.read().decode()[:500]
                    except: pass
                    _log(f"HTTPError {e.code}: {e.reason}  body={body[:200]}")
                    msg = f"HTTP {e.code}: {e.reason}\n\n{body}\n\nLog : {LOG_FILE}"
                    ui(lambda m=msg: modal_err(gx("Erreur Emby","Emby error"), m))

                except Exception as e:
                    _log(f"Exception {type(e).__name__}: {e}")
                    msg = f"{type(e).__name__}:\n{e}\n\nLog : {LOG_FILE}"
                    ui(lambda m=msg: modal_err(gx("Erreur","Error"), m))

            threading.Thread(target=thread, daemon=True).start()

        with dpg.group(horizontal=True):
            dpg.add_button(label="Enregistrer sur Emby", width=180,
                           callback=lambda s,a,u: do_save())
            dpg.add_spacer(width=10)
            dpg.add_button(label="Annuler", width=80,
                           user_data=win_tag,
                           callback=lambda s,a,u: dpg.delete_item(u))


def fetch_genres_from_server(base, key, uid, movie_lib_ids=None):
    """Genres reels depuis Emby. Strategie 1: /Genres  Strategie 2: /Items fallback.
    Entierement isole dans try/except - un echec ne bloque jamais la connexion."""
    params_base = {"Recursive":"true","IncludeItemTypes":"Movie"}
    if uid:
        params_base["UserId"] = uid
    seen, genres = set(), []
    scopes = movie_lib_ids if movie_lib_ids else [None]
    # Strategie 1 : /Genres
    try:
        for pid in scopes:
            p2 = dict(params_base)
            if pid: p2["ParentId"] = pid
            data = emby_get(base, key, "/Genres", p2)
            for g in data.get("Items", []):
                name = g.get("Name","").strip()
                if name and name not in seen:
                    seen.add(name); genres.append(name)
        if genres:
            return sorted(genres, key=str.casefold)
    except Exception:
        pass
    # Strategie 2 : /Items (toujours disponible)
    try:
        for pid in scopes:
            p2 = dict(params_base)
            p2["Fields"] = "Genres"; p2["Limit"] = 500; p2["StartIndex"] = 0
            if pid: p2["ParentId"] = pid
            while True:
                data  = emby_get(base, key, "/Items", dict(p2))
                items = data.get("Items", [])
                for it in items:
                    for g in it.get("Genres", []):
                        g = g.strip()
                        if g and g not in seen:
                            seen.add(g); genres.append(g)
                if len(items) < p2["Limit"]: break
                p2["StartIndex"] += len(items)
    except Exception:
        pass
    return sorted(genres, key=str.casefold)


def fetch_by_genres(base, key, uid, genres, parent_ids, cb):
    """
    Récupère les films correspondant à AU MOINS UN des genres listés.
    genres    : liste de chaînes (valeurs API Emby, ex. ["Animation","Horror"])
    parent_ids: liste d'IDs de médiathèques (None = toutes)
    cb        : callback(fetched, total, page)
    """
    base_params = {
        "Recursive":         "true",
        "IncludeItemTypes":  "Movie",
        "Fields":            "Path,MediaSources,ProductionYear,Genres,DateCreated,OfficialRating,ProviderIds",
        "Limit":             500,
        "Genres":            "|".join(genres),   # opérateur OR dans Emby
    }
    if uid:
        base_params["UserId"] = uid

    scopes = parent_ids if parent_ids else [None]
    all_items, seen_ids = [], set()

    for pid in scopes:
        params = dict(base_params); params["StartIndex"] = 0
        if pid: params["ParentId"] = pid
        page = 0
        while True:
            data  = emby_get(base, key, "/Items", dict(params))
            items = data.get("Items", [])
            for it in items:
                if it.get("Id") not in seen_ids:
                    seen_ids.add(it.get("Id")); all_items.append(it)
            total = data.get("TotalRecordCount", 0)
            page += 1
            cb(len(all_items), len(all_items) + max(0, total - len(items)), page)
            if len(items) < params["Limit"] or not items: break
            params["StartIndex"] += len(items)

    return all_items


# ══════════════════════════════════════════════════════════════
#  CONVERSION CHEMIN
# ══════════════════════════════════════════════════════════════
def to_win(path, prefix=None, unc=None):
    if not path: return path
    p = (prefix or G.get("nas_prefix","")).rstrip("/")
    u = unc or G.get("nas_unc","")
    if not u: return path.replace("/", "\\")
    base = re.sub(r'\d+$', '', p) or "/volume"
    m = re.match(r'^' + re.escape(base) + r'\d*', path, re.IGNORECASE)
    if m:
        return u.rstrip("\\") + path[m.end():].replace("/", "\\")
    return path.replace("/", "\\")


def fmt_size(b):
    if not b: return "-"
    for u,d in [("Go",1e9),("Mo",1e6),("Ko",1e3)]:
        if b >= d: return f"{b/d:.1f} {u}"
    return f"{b} o"


# ══════════════════════════════════════════════════════════════
#  ÉTAT GLOBAL
# ══════════════════════════════════════════════════════════════
CFG = load_config()
G = {
    "libraries":    [],
    "lib_selected": set(),
    "genres_list":  [],        # genres chargés depuis Emby
    "genres_sel":   set(),    # valeurs API des genres cochés
    "custom_genre": "",       # genre libre saisi par l'utilisateur
    "results":      [],       # liste de dicts {title, year, genres, path, win_path, size}
    "filter":       "",
    "age_filter":   set(),   # ratings coches pour filtrer
    "omdb_cache":   {},      # {item_id: {"rated":"R","age":"16+","note":"7.2"}} -- cle = ID Emby
    "omdb_key":     "",      # cle API omdbapi.com (gratuite)
    "api_provider": API_CFG["provider"],  # "omdb" ou "tmdb"
    "nas_prefix":   CFG["emby"].get("nas_prefix", "/volume1"),
    "nas_unc":      CFG["emby"].get("nas_unc", ""),
    "player":       CFG["emby"].get("player", ""),
    "lang":         "fr",     # langue courante (fr/en) pilotee par le bandeau commun
    "hide_same":    False,
    "age_undo":     [],       # pile d'annulation de la derniere application en masse    # masquer les films dont age web == age enregistre
}

def gx(fr, en):
    """Retourne la chaine FR ou EN selon la langue courante de l'onglet Genres."""
    return en if G.get("lang") == "en" else fr


_GTIPS = []     # infobulles statiques traduisibles : [(text_tag, fr, en), ...]
_gtip_n = 0

def gtip(fr, en, wrap=300):
    """Infobulle statique traduisible : tague le texte et l'enregistre pour la bascule de langue."""
    global _gtip_n
    _gtip_n += 1
    tag = f"g_tip_{_gtip_n}"
    with dpg.tooltip(dpg.last_item()):
        dpg.add_text(gx(fr, en), tag=tag, wrap=wrap)
    _GTIPS.append((tag, fr, en))
_mid = 0
_render_timer = 0.0
_RENDER_DELAY = 0.25

# ══════════════════════════════════════════════════════════════
#  MODALES
# ══════════════════════════════════════════════════════════════
def modal_err(title, msg):
    global _mid; _mid += 1; tag = f"_err{_mid}"
    with dpg.window(label=title, tag=tag, modal=True,
                    width=520, autosize=True, pos=[160,200], no_resize=True):
        dpg.add_text(msg, wrap=500)
        dpg.add_separator()
        dpg.add_button(label="OK", width=-1,
                       user_data=tag, callback=lambda s, a, u: dpg.delete_item(u))

def modal_info(title, msg):
    global _mid; _mid += 1; tag = f"_inf{_mid}"
    with dpg.window(label=title, tag=tag, modal=True,
                    width=520, autosize=True, pos=[160,200], no_resize=True):
        dpg.add_text(msg, wrap=500)
        dpg.add_separator()
        dpg.add_button(label="OK", width=-1,
                       user_data=tag, callback=lambda s, a, u: dpg.delete_item(u))


# ══════════════════════════════════════════════════════════════
#  OUVERTURE DOSSIER
# ══════════════════════════════════════════════════════════════
def open_folder(win_path, linux_path=""):
    """
    Ouvre le dossier du fichier dans l'explorateur Windows.
    Stratégie en cascade :
      1. explorer /select,"chemin"  →  sélectionne le fichier dans son dossier
      2. os.startfile(dossier parent)  →  ouvre juste le dossier
      3. modal_path()  →  affiche les chemins pour copier/coller manuellement
    """
    # Choisir le meilleur chemin disponible
    path = win_path or linux_path
    if not path:
        _modal_no_path(linux_path)
        return

    if sys.platform != "win32":
        # Linux / Mac : xdg-open sur le dossier parent
        try:
            subprocess.Popen(["xdg-open", str(Path(path).parent)])
        except Exception as e:
            msg = str(e)
            ui(lambda: modal_err(gx("Erreur dossier","Folder error"), msg))
        return

    # ── Windows ──────────────────────────────────────────────
    # Étape 1 : explorer /select (sélectionne le fichier)
    opened = False
    try:
        subprocess.Popen(f'explorer /select,"{path}"', shell=True)
        opened = True
    except Exception:
        pass

    if not opened:
        # Étape 2 : os.startfile sur le dossier parent
        try:
            parent = str(Path(path).parent)
            os.startfile(parent)
            opened = True
        except Exception:
            pass

    if not opened:
        # Étape 3 : modal avec les chemins bruts pour copier/coller
        _modal_no_path(linux_path, win_path)


def _modal_no_path(linux_path="", win_path=""):
    """Modal de secours : affiche les chemins pour copier manuellement."""
    global _mid; _mid += 1; tag = f"_pth{_mid}"
    msg = "Impossible d'ouvrir le dossier automatiquement.\n\n"
    if win_path:
        msg += f"Chemin Windows :\n{win_path}\n\n"
    if linux_path:
        msg += f"Chemin Emby (Linux) :\n{linux_path}\n"
    msg += "\nVérifiez votre configuration Préfixe / UNC."
    with dpg.window(label="Chemin fichier", tag=tag, modal=True,
                    width=600, height=260, pos=[100,200], no_resize=True):
        dpg.add_text(msg, wrap=580)
        dpg.add_separator()
        dpg.add_button(label="OK", width=-1,
                       user_data=tag, callback=lambda s, a, u: dpg.delete_item(u))


# ══════════════════════════════════════════════════════════════
#  RENDU RÉSULTATS
# ══════════════════════════════════════════════════════════════
def render_results():
    dpg.delete_item("results_area", children_only=True)
    G["_webcells"] = {}
    ft = G["filter"].lower().strip()

    af   = G["age_filter"]

    def _same_age(r):
        omdb = G["omdb_cache"].get(r.get("item_id", ""))
        if not isinstance(omdb, dict):
            return False
        w = _age_to_num(omdb.get("age", ""))
        rec = _age_to_num(_rated_to_age(r.get("rating", "")))
        return w is not None and rec is not None and w == rec

    rows = [r for r in G["results"]
            if (not ft or ft in r["title"].lower() or ft in r["genres_str"].lower())
            and (not af or (r.get("rating","") or "-") in af)
            and (not G.get("hide_same") or not _same_age(r))]

    total = len(G["results"])
    shown = len(rows)

    # Compteur
    with dpg.group(parent="results_area", horizontal=True):
        dpg.add_text(f"{shown}", color=(233,69,96))
        dpg.add_text(gx("film(s) affiché(s)", "movie(s) shown"), color=(136,136,170))
        if shown != total:
            dpg.add_spacer(width=6)
            dpg.add_text(gx(f"(sur {total} trouvés)", f"(of {total} found)"), color=(136,136,170))
    dpg.add_separator(parent="results_area")
    dpg.add_spacer(height=4, parent="results_area")

    if not rows:
        dpg.add_text(gx("Aucun résultat.", "No results."), parent="results_area", color=(136,136,170))
        return

    with dpg.table(parent="results_area", header_row=True, row_background=True,
                   borders_innerH=True, borders_outerH=True,
                   borders_innerV=True, borders_outerV=True,
                   scrollY=True, scrollX=False,
                   policy=dpg.mvTable_SizingStretchProp,
                   height=-1):

        dpg.add_table_column(label=gx("Titre","Title"),       width_stretch=True, init_width_or_weight=0.28)
        dpg.add_table_column(label=gx("Année","Year"),        width_fixed=True,   init_width_or_weight=52)
        dpg.add_table_column(label=gx("Genres","Genres"),     width_stretch=True, init_width_or_weight=0.18)
        dpg.add_table_column(label=gx("Dossier NAS","NAS folder"), width_stretch=True, init_width_or_weight=0.38)
        dpg.add_table_column(label=gx("Taille","Size"),       width_fixed=True,   init_width_or_weight=72)
        dpg.add_table_column(label="Age",           width_fixed=True,   init_width_or_weight=52)
        dpg.add_table_column(label="Age web",       width_fixed=True,   init_width_or_weight=62)
        dpg.add_table_column(label="Actions",       width_fixed=True,   init_width_or_weight=225)

        for row in rows:
            wp     = row["win_path"]
            lp     = row["path"]
            folder = str(Path(wp).parent) if wp else (
                     str(Path(lp).parent) if lp else "-")
            fd = folder if len(folder) <= 60 else "..." + folder[-57:]
            ud = (wp, lp)   # user_data = (win_path, linux_path)

            with dpg.table_row():
                # Titre cliquable → ouvre le dossier
                dpg.add_button(
                    label=f"  {row['title']}", width=-1,
                    user_data=ud,
                    callback=lambda s,a,u: open_folder(u[0], u[1]))
                with dpg.tooltip(dpg.last_item()):
                    dpg.add_text(
                        gx(f"Fichier : {row.get('filename','-')}\nLinux   : {lp or '-'}\nWindows : {wp or '- (configurer UNC)'}",
                           f"File    : {row.get('filename','-')}\nLinux   : {lp or '-'}\nWindows : {wp or '- (configure UNC)'}"),
                        wrap=620)

                dpg.add_text(str(row["year"]) if row["year"] else "-",
                             color=(180,180,130))
                dpg.add_text(row["genres_str"], color=(136,200,255))
                dpg.add_text(fd, color=(180,180,180))
                dpg.add_text(row["size_str"])

                rating = row.get("rating","")
                dpg.add_text(rating if rating else "-",
                             color=(255,180,80) if rating else (100,100,120))

                # Age web depuis OMDB
                omdb = G["omdb_cache"].get(row.get("item_id",""), None)
                if omdb is None:
                    web_txt = "-"
                    web_col = (100,100,120)
                    web_tip = gx("Cliquez sur Enrichir pour charger.", "Click Enrich to load.")
                elif omdb == "pending":
                    web_txt = "..."
                    web_col = (136,136,170)
                    web_tip = gx("Chargement en cours...", "Loading...")
                elif omdb == "error":
                    web_txt = "err"
                    web_col = (180,60,60)
                    web_tip = gx("Erreur (clé invalide ou film introuvable).", "Error (invalid key or movie not found).")
                else:
                    web_txt = omdb.get("age","-")
                    web_col = (100,255,160)
                    _r = omdb.get('rated','-'); _n = omdb.get('note','-')
                    _src = omdb.get("source","?")
                    web_tip = gx(f"Source : {_src}\nClassification : {_r}\nNote : {_n}/10",
                                 f"Source: {_src}\nRating: {_r}\nScore: {_n}/10")
                _wc = dpg.add_text(web_txt, color=web_col)
                _iid = row.get("item_id", "")
                if _iid:
                    G["_webcells"][_iid] = _wc
                with dpg.tooltip(_wc):
                    dpg.add_text(web_tip, wrap=260)

                with dpg.group(horizontal=True):
                    dpg.add_button(label=gx("Lire","Play"), width=38,
                        user_data=wp,
                        callback=lambda s,a,u: open_file(u, get_player()))
                    with dpg.tooltip(dpg.last_item()):
                        dpg.add_text(gx("Ouvre le fichier avec le lecteur vidéo configuré.", "Opens the file with the configured video player."), wrap=260)
                    dpg.add_button(label=gx("Dossier","Folder"), width=60,
                        user_data=ud,
                        callback=lambda s,a,u: open_folder(u[0], u[1]))
                    with dpg.tooltip(dpg.last_item()):
                        dpg.add_text(gx(f"Win : {wp or '--- (UNC non configuré)'}", f"Win : {wp or '--- (UNC not configured)'}"), wrap=320)
                    _conn2 = {"url": dpg.get_value("inp_url").strip().rstrip("/"),
                              "key": dpg.get_value("inp_key").strip(),
                              "uid": dpg.get_value("inp_uid").strip()}
                    dpg.add_button(label="Age", width=38,
                        user_data=(row.get("item_id",""), row["title"],
                                   row.get("rating",""), _conn2),
                        callback=lambda s,a,u: do_set_rating(u[0],u[1],u[2],u[3]))
                    with dpg.tooltip(dpg.last_item()):
                        dpg.add_text(gx("Modifier la classification d'âge\ndans Emby.", "Edit the age rating\nin Emby."), wrap=240)
                    # Bouton appliquer age web directement
                    _web_age = omdb.get("rated","") if isinstance(omdb,dict) else ""
                    dpg.add_button(label=f">{_web_age or '?'}", width=46,
                        user_data=(row.get("item_id",""), row["title"], _conn2),
                        callback=lambda s,a,u: do_apply_web_age(u[0],u[1],u[2]))
                    with dpg.tooltip(dpg.last_item()):
                        dpg.add_text(
                            gx(f"Appliquer l'âge web ({_web_age or '-'}) directement\nsur Emby sans confirmation.",
                               f"Apply the web age ({_web_age or '-'}) directly\nto Emby without confirmation."), wrap=260)


def _schedule_render():
    global _render_timer
    _render_timer = time.time() + _RENDER_DELAY

def on_filter(s, v, u):
    G["filter"] = v; _schedule_render()


# ══════════════════════════════════════════════════════════════
#  PANEL MÉDIATHÈQUES
# ══════════════════════════════════════════════════════════════
def _rebuild_library_panel(libs):
    dpg.delete_item("lib_panel", children_only=True)
    if not libs:
        dpg.add_text(gx("Aucune médiathèque trouvée.", "No library found."),
                     parent="lib_panel", color=(200,80,80))
        return

    with dpg.group(horizontal=True, parent="lib_panel"):
        dpg.add_text("Médiathèques :", color=(136,136,170))
        dpg.add_spacer(width=8)
        dpg.add_button(label="Tout cocher", width=90,
            user_data=libs,
            callback=lambda s,a,u: _select_all_libs(u, True))
        dpg.add_button(label="Tout décocher", width=100,
            user_data=libs,
            callback=lambda s,a,u: _select_all_libs(u, False))
    dpg.add_spacer(height=4, parent="lib_panel")

    COLS = 4
    with dpg.table(parent="lib_panel", header_row=False,
                   policy=dpg.mvTable_SizingStretchSame):
        for _ in range(COLS):
            dpg.add_table_column()
        for i in range(0, len(libs), COLS):
            with dpg.table_row():
                batch = libs[i:i+COLS]
                for lib in batch:
                    icon = {"movies":"Films","tvshows":"Séries","music":"Musique",
                            "books":"Livres","photos":"Photos"}.get(lib["type"],
                            lib["type"] or "")
                    dpg.add_checkbox(
                        label=f"{lib['name']}  [{icon}]",
                        tag=f"chk_lib_{lib['id']}",
                        default_value=False,
                        user_data=lib["id"],
                        callback=lambda s,v,u: _toggle_lib(u, v))
                for _ in range(COLS - len(batch)):
                    dpg.add_text("")


def _select_all_libs(libs, checked):
    for lib in libs:
        try: dpg.set_value(f"chk_lib_{lib['id']}", checked)
        except: pass
        _toggle_lib(lib["id"], checked)

def _toggle_lib(lib_id, checked):
    if checked: G["lib_selected"].add(lib_id)
    else:       G["lib_selected"].discard(lib_id)


# ══════════════════════════════════════════════════════════════
#  CONNECT
# ══════════════════════════════════════════════════════════════
def _get_params():
    return {
        "url":    dpg.get_value("inp_url").strip().rstrip("/"),
        "key":    dpg.get_value("inp_key").strip(),
        "uid":    dpg.get_value("inp_uid").strip(),
        "prefix": dpg.get_value("inp_prefix").strip(),
        "unc":    dpg.get_value("inp_unc").strip(),
    }

def do_connect():
    p = _get_params()
    if not p["url"] or not p["key"]:
        modal_err(gx("Paramètres manquants","Missing parameters"), gx("L'URL et la clé API sont obligatoires.","Emby URL and API key are required."))
        return

    def thread():
        try:
            emby_get(p["url"], p["key"], "/System/Info/Public")
            libs_raw = emby_get(p["url"], p["key"], "/Library/VirtualFolders")
            libs = []
            for lib in libs_raw:
                lib_id   = lib.get("ItemId","") or lib.get("Id","")
                lib_name = lib.get("Name","?")
                lib_type = lib.get("CollectionType","") or ""
                if lib_id:
                    libs.append({"id":lib_id,"name":lib_name,"type":lib_type})

            # Genres : isole, un echec ne bloque pas la connexion
            genres = []
            try:
                movie_ids = [l["id"] for l in libs
                             if l["type"] in ("movies","mixed","","homevideos")]
                genres = fetch_genres_from_server(
                    p["url"], p["key"], p["uid"],
                    movie_ids if movie_ids else None)
            except Exception:
                genres = []

            def on_done(libs=libs, genres=genres):
                G["libraries"]    = libs
                G["lib_selected"] = set()  # rien de coche par defaut
                _rebuild_library_panel(libs)
                try:
                    _rebuild_genre_panel(genres)
                except Exception:
                    pass  # le panel genre ne doit JAMAIS bloquer la connexion
                dpg.configure_item("btn_scan", enabled=True)  # toujours active
                status = gx(f"Connecté - {len(libs)} médiathèque(s)",
                            f"Connected - {len(libs)} library(ies)")
                if genres:
                    status += gx(f"  -  {len(genres)} genres chargés",
                                 f"  -  {len(genres)} genres loaded")
                dpg.set_value("lbl_status", status)

            ui(on_done)

        except urllib.error.HTTPError as e:
            msg = "Clé API invalide (401)." if e.code==401 else f"HTTP {e.code}: {e.reason}"
            ui(lambda m=msg: modal_err(gx("Erreur connexion","Connection error"), m))
        except Exception as e:
            ui(lambda m=str(e): modal_err(gx("Erreur connexion","Connection error"), m))

    threading.Thread(target=thread, daemon=True).start()


# ══════════════════════════════════════════════════════════════
#  SCAN PAR GENRE
# ══════════════════════════════════════════════════════════════
def start_scan():
    p = _get_params()
    if not p["url"] or not p["key"]:
        modal_err(gx("Paramètres manquants","Missing parameters"), gx("L'URL et la clé API sont obligatoires.","Emby URL and API key are required."))
        return

    # Collecte des genres cochés
    genres_api = list(G["genres_sel"])
    custom = dpg.get_value("inp_custom_genre").strip()
    if custom:
        genres_api.append(custom)

    if not genres_api:
        modal_err(gx("Aucun genre","No genre"), gx("Cochez au moins un genre ou saisissez un genre libre.","Check at least one genre or type a custom genre."))
        return

    G["nas_prefix"] = p["prefix"]; G["nas_unc"] = p["unc"]
    _player = dpg.get_value("inp_player").strip()
    save_config({"url":p["url"],"api_key":p["key"],"user_id":p["uid"],
                 "nas_prefix":p["prefix"],"nas_unc":p["unc"],"player":_player})

    parent_ids = list(G["lib_selected"]) if G["lib_selected"] else None

    dpg.configure_item("btn_scan", enabled=False, label="Scan...")
    dpg.configure_item("scan_popup", show=True)
    dpg.set_value("scan_step", "Connexion...")
    dpg.set_value("scan_pb", 0.0)

    def thread():
        def set_step(msg, pct):
            ui(lambda m=msg, pp=pct: (
                dpg.set_value("scan_step", m),
                dpg.set_value("scan_pb", pp)))

        try:
            set_step(gx(f"Connexion à {p['url']}...", f"Connecting to {p['url']}..."), 0.05)
            emby_get(p["url"], p["key"], "/System/Info/Public")

            genres_label = ", ".join(genres_api)
            scope_msg = (f"{len(parent_ids)} médiathèque(s)"
                         if parent_ids else "toutes les médiathèques")
            set_step(gx(f"Recherche [{genres_label}] dans {scope_msg}...",
                        f"Searching [{genres_label}] in {scope_msg}..."), 0.10)

            t0 = time.time()
            def on_page(fetched, total, page):
                pct = 0.10 + 0.80 * (fetched / max(total, 1))
                el  = time.time() - t0; rate = fetched / el if el > 0 else 0
                eta = (total - fetched) / rate if rate > 0 else 0
                msg = (f"Page {page} - {fetched} films ({rate:.0f}/s)"
                       + (f" ~{eta:.0f}s" if eta > 2 else ""))
                set_step(msg, pct)

            movies = fetch_by_genres(p["url"], p["key"], p["uid"],
                                     genres_api, parent_ids, on_page)

            set_step(gx(f"{len(movies)} films - construction de la liste...",
                        f"{len(movies)} movies - building the list..."), 0.92)

            # Construire les lignes
            results = []
            prefix = p["prefix"]; unc = p["unc"]
            for m in movies:
                srcs = m.get("MediaSources", [])
                path = srcs[0].get("Path","") if srcs else m.get("Path","")
                size = srcs[0].get("Size",0) if srcs else 0
                win  = to_win(path, prefix, unc) if path else ""
                results.append({
                    "title":      m.get("Name","?"),
                    "year":       m.get("ProductionYear",""),
                    "genres":     m.get("Genres",[]),
                    "genres_str": ", ".join(m.get("Genres",[])),
                    "path":       path,
                    "win_path":   win,
                    "filename":   Path(path).name if path else "-",
                    "size":       size,
                    "size_str":   fmt_size(size),
                    "rating":     m.get("OfficialRating","") or "",
                    "imdb_id":    (m.get("ProviderIds") or {}).get("Imdb",""),
                    "tmdb_id":    (m.get("ProviderIds") or {}).get("Tmdb",""),
                    "item_id":    m.get("Id",""),
                })

            # Tri alphabétique par défaut
            results.sort(key=lambda r: r["title"].lower())

            # Age web deja connu (base SQLite) : affichage immediat, sans reseau
            try:
                n_db = _prefill_web_from_db(results, G.get("api_provider", "tmdb"))
            except Exception:
                n_db = 0

            def finish(res=results, gl=genres_label, n=len(movies), n_db=n_db):
                G["results"] = res
                dpg.set_value("scan_pb", 1.0)
                dpg.configure_item("scan_popup", show=False)
                dpg.configure_item("btn_scan", enabled=True, label="Rechercher")
                _set_status2(f"{n} film(s) trouvé(s) pour : {gl}"
                             + (f"  -  âge web en base : {n_db}" if n_db else ""),
                             f"{n} movie(s) found for: {gl}"
                             + (f"  -  web age from DB: {n_db}" if n_db else ""))
                _rebuild_age_filter()
                render_results()

            ui(finish)

        except urllib.error.HTTPError as e:
            msg = "Clé API invalide (401)." if e.code==401 else f"HTTP {e.code}: {e.reason}"
            ui(lambda m=msg: (modal_err(gx("Erreur API","API error"), m),
               dpg.configure_item("scan_popup", show=False),
               dpg.configure_item("btn_scan", enabled=True, label="Rechercher")))
        except Exception as e:
            msg = str(e)
            ui(lambda m=msg: (modal_err(gx("Erreur scan","Scan error"), m),
               dpg.configure_item("scan_popup", show=False),
               dpg.configure_item("btn_scan", enabled=True, label="Rechercher")))

    threading.Thread(target=thread, daemon=True).start()


# ══════════════════════════════════════════════════════════════
#  GENRE CHECKBOXES - callbacks
# ══════════════════════════════════════════════════════════════
def _toggle_genre(api_val, checked):
    if checked: G["genres_sel"].add(api_val)
    else:       G["genres_sel"].discard(api_val)

def _all_genres(checked):
    for gi, name in enumerate(G["genres_list"]):
        try: dpg.set_value(f"chk_genre_{gi}", checked)
        except Exception: pass
        _toggle_genre(name, checked)

def _none_genres():
    _all_genres(False)


# ══════════════════════════════════════════════════════════════
#  EXPORT CSV RAPIDE
# ══════════════════════════════════════════════════════════════
def do_export_csv():
    import csv as _csv
    try:
        import tkinter as tk; from tkinter import filedialog
        r = tk.Tk(); r.withdraw()
        fp = filedialog.asksaveasfilename(
            title="Exporter en CSV",
            initialfile=f"emby_genres_{time.strftime('%Y%m%d_%H%M')}.csv",
            defaultextension=".csv",
            filetypes=[("CSV","*.csv"),("Tous","*.*")])
        r.destroy()
        if not fp: return
        with open(fp,"w",newline="",encoding="utf-8-sig") as f:
            w = _csv.writer(f, delimiter=";")
            w.writerow(["Titre","Année","Genres","Dossier","Fichier","Taille","Age","Age web","Note IMDB"])
            for row in G["results"]:
                folder = str(Path(row["win_path"]).parent) if row["win_path"] else "-"
                omdb_r = G["omdb_cache"].get(row.get("item_id",""), {})
                age_web = omdb_r.get("age","") if isinstance(omdb_r, dict) else ""
                note    = omdb_r.get("note","") if isinstance(omdb_r, dict) else ""
                w.writerow([row["title"], row["year"], row["genres_str"],
                            folder, row["filename"], row["size_str"],
                            row.get("rating",""), age_web, note])
        modal_info(gx("Export CSV","CSV export"), gx(f"Fichier :\n{fp}", f"File:\n{fp}"))
    except Exception as e:
        modal_err(gx("Erreur export","Export error"), str(e))


# ══════════════════════════════════════════════════════════════
#  PANEL GENRES DYNAMIQUE
# ══════════════════════════════════════════════════════════════
def _rebuild_genre_panel(genres):
    """Reconstruit le panel genres avec les valeurs reelles du serveur Emby."""
    dpg.delete_item("genre_panel", children_only=True)
    G["genres_list"] = genres
    G["genres_sel"]  = set()

    if not genres:
        dpg.add_text(gx("Aucun genre trouvé - vérifiez les médiathèques sélectionnées.",
                        "No genre found - check the selected libraries."),
                     parent="genre_panel", color=(200,80,80))
        return

    dpg.add_text(gx(f"{len(genres)} genres chargés depuis votre Emby :",
                    f"{len(genres)} genres loaded from your Emby:"),
                 tag="g_genre_count", parent="genre_panel", color=(46,204,113))
    dpg.add_spacer(height=3, parent="genre_panel")
    COLS = 6
    with dpg.table(parent="genre_panel", header_row=False,
                   policy=dpg.mvTable_SizingStretchSame):
        for _ in range(COLS): dpg.add_table_column()
        for i in range(0, len(genres), COLS):
            with dpg.table_row():
                batch = genres[i:i+COLS]
                for j, name in enumerate(batch):
                    gi = i + j           # index global = tag unique (anti-collision)
                    dpg.add_checkbox(
                        label=name,
                        tag=f"chk_genre_{gi}",
                        default_value=False,
                        user_data=name,
                        callback=lambda s,v,u: _toggle_genre(u, v))
                for _ in range(COLS - len(batch)):
                    dpg.add_text("")


def _browse_player():
    try:
        import tkinter as tk; from tkinter import filedialog
        r = tk.Tk(); r.withdraw()
        fp = filedialog.askopenfilename(title="Lecteur video",
             filetypes=[("Executables","*.exe"),("Tous","*.*")],
             initialdir=r"C:\Program Files")
        r.destroy()
        if fp: dpg.set_value("inp_player", fp)
    except Exception: pass


def do_apply_web_age(item_id, title, conn):
    """Applique directement l'age web (OMDB/TMDB) sur Emby, sans modal."""
    omdb = G["omdb_cache"].get(item_id)
    if not omdb or not isinstance(omdb, dict):
        ui(lambda: modal_err(gx("Âge web indisponible","Web age unavailable"),
            gx("Enrichissez d'abord les données via le bouton Enrichir.",
               "Enrich the data first using the Enrich button.")))
        return
    age = omdb.get("rated", "")   # valeur brute ex: "PG-13", "12", "R"
    if not age or age in ("N/A", "NOT RATED", "UNRATED", "NR", "?"):
        ui(lambda: modal_err(gx("Âge web indisponible","Web age unavailable"),
            gx(f"Pas de classification disponible pour ce film.\nValeur OMDB/TMDB : {age!r}",
               f"No classification available for this movie.\nOMDB/TMDB value: {age!r}")))
        return
    # Appliquer directement sans modal de confirmation
    _log(f"=== apply_web_age : iid={item_id} titre={title!r} age={age!r} ===")
    def thread(iid=item_id, nr=age, c=conn, ttl=title):
        try:
            uid = (c.get("uid") or "").strip()
            get_params = {
                "Ids":    iid,
                "Fields": "Path,MediaSources,ProductionYear,Genres,"
                          "OfficialRating,ProviderIds,Name,Overview,"
                          "DateCreated,CommunityRating,Tags,Studios,People",
            }
            if uid:
                get_params["UserId"] = uid
            _log(f"GET /Items?Ids={iid}")
            data = emby_get(c["url"], c["key"], "/Items", get_params)
            items = data.get("Items", [])
            if not items:
                raise ValueError("Item introuvable.")
            full = items[0]
            _log(f"GET OK  OfficialRating actuel={full.get('OfficialRating','(vide)')!r}  => {nr!r}")
            full["OfficialRating"] = nr
            status = emby_post(c["url"], c["key"], f"/Items/{iid}", full)
            _log(f"POST HTTP {status}")
            if status not in (200, 204):
                raise ValueError(f"HTTP {status} inattendu")
            for r in G["results"]:
                if r.get("item_id") == iid:
                    r["rating"] = nr
            ui(lambda: (_rebuild_age_filter(), render_results()))
        except urllib.error.HTTPError as e:
            body = ""
            try: body = e.read().decode()[:300]
            except: pass
            _log(f"HTTPError {e.code}: {body}")
            ui(lambda m=f"HTTP {e.code}: {e.reason}\n{body}":
                modal_err(gx("Erreur Emby","Emby error"), m))
        except Exception as e:
            _log(f"Exception: {e}")
            ui(lambda m=str(e): modal_err(gx("Erreur","Error"), m))
    threading.Thread(target=thread, daemon=True).start()


# ══════════════════════════════════════════════════════════════
#  ENRICHISSEMENT - OMDB + TMDB
# ══════════════════════════════════════════════════════════════
_RATING_AGE = {
    # USA (MPAA)
    "G":"Tous","PG":"10+","PG-13":"13+","R":"16+","NC-17":"18+",
    "APPROVED":"Tous","PASSED":"Tous","M":"16+","GP":"10+","X":"18+",
    # USA TV
    "TV-Y":"Tous","TV-Y7":"7+","TV-G":"Tous","TV-PG":"10+",
    "TV-14":"14+","TV-MA":"18+",
    # France (CNC)
    "TOUS PUBLICS":"Tous","TOUS":"Tous","TOUT PUBLIC":"Tous",
    "-10":"10+","-12":"12+","-16":"16+","-18":"18+",
    "INTERDIT AUX MOINS DE 12 ANS":"12+","INTERDIT AUX MOINS DE 16 ANS":"16+",
    "INTERDIT AUX MOINS DE 18 ANS":"18+",
    # Royaume-Uni (BBFC)
    "U":"Tous","UC":"Tous","12":"12+","12A":"12+","15":"15+","18":"18+",
    "PG-12":"12+",
    # Allemagne (FSK)
    "0":"Tous","6":"6+","16":"16+",
    # Quebec / divers
    "G ":"Tous","13+":"13+","16+":"16+","18+":"18+","8+":"8+","13":"13+",
    "7":"7+","10":"10+","14":"14+",
    # explicitement non classe
    "NR":"n/c","NOT RATED":"n/c","UNRATED":"n/c","N/A":"n/c","":"n/c",
}

def _rated_to_age(rated):
    return _RATING_AGE.get((rated or "").upper().strip(), rated or "-")


def _age_to_num(age):
    """Convertit un age affiche ('Tous','10+','16+','n/c','?') en entier ou None."""
    a = (age or "").strip().lower()
    if a in ("tous", "tout public", "tous publics", "u", "g", "0", "0+"):
        return 0
    if a in ("", "?", "-", "-", "n/c", "n/a", "err", "..."):
        return None
    m = re.search(r"(\d+)", a)
    return int(m.group(1)) if m else None


def _set_status2(fr, en):
    """Statut principal bilingue, re-traduit a la bascule de langue."""
    G["_last_status"] = (fr, en)
    try: dpg.set_value("lbl_status", gx(fr, en))
    except Exception: pass


def _set_hide_same(v):
    G["hide_same"] = bool(v)
    try:
        render_results()
    except Exception:
        pass


def _run_age_updates(pairs, record_undo, lbl_fr, lbl_en, after=None):
    """Applique OfficialRating pour chaque (item_id, titre, nouvel_age).
    record_undo : memorise les anciennes valeurs pour 'Annuler ages'."""
    conn = {"url": dpg.get_value("inp_url").strip().rstrip("/"),
            "key": dpg.get_value("inp_key").strip(),
            "uid": dpg.get_value("inp_uid").strip()}
    dpg.configure_item("apply_popup", show=True, label=gx(lbl_fr, lbl_en))
    dpg.set_value("apply_pb", 0.0)
    dpg.set_value("apply_step", gx(f"0 / {len(pairs)} film(s) traité(s)",
                                   f"0 / {len(pairs)} movie(s) processed"))
    dpg.set_value("apply_detail", "")
    dpg.configure_item("g_btn_applyhigher", enabled=False)
    dpg.configure_item("g_btn_undoages", enabled=False)

    def thread(pairs=pairs, conn=conn):
        ok = 0; total = len(pairs); undo = []
        fields = ("Path,MediaSources,ProductionYear,Genres,OfficialRating,"
                  "ProviderIds,Name,Overview,DateCreated,CommunityRating,"
                  "Tags,Studios,People")
        for idx, (iid, title, raw) in enumerate(pairs):
            ui(lambda i=idx, tt=title, rw=raw, n=total: (
                dpg.set_value("apply_pb", i / max(n, 1)),
                dpg.set_value("apply_step",
                              gx(f"{i} / {n} film(s) traité(s)",
                                 f"{i} / {n} movie(s) processed")),
                dpg.set_value("apply_detail",
                              (tt[:70] + ("..." if len(tt) > 70 else ""))
                              + f"  ->  {rw}")))
            try:
                gp = {"Ids": iid, "Fields": fields}
                if conn["uid"]:
                    gp["UserId"] = conn["uid"]
                data = emby_get(conn["url"], conn["key"], "/Items", gp)
                items = data.get("Items", [])
                if not items:
                    continue
                full = items[0]
                old = full.get("OfficialRating", "") or ""
                full["OfficialRating"] = raw
                st = emby_post(conn["url"], conn["key"], f"/Items/{iid}", full)
                if st in (200, 204):
                    ok += 1
                    if record_undo:
                        undo.append((iid, title, old))
                    for r in G["results"]:
                        if r.get("item_id") == iid:
                            r["rating"] = raw
            except Exception:
                pass
            time.sleep(0.05)

        def done(o=ok, n=total, u=undo):
            dpg.set_value("apply_pb", 1.0)
            dpg.set_value("apply_step", gx(f"{o} / {n} âge(s) appliqué(s)",
                                           f"{o} / {n} age(s) applied"))
            dpg.configure_item("apply_popup", show=False)
            dpg.configure_item("g_btn_applyhigher", enabled=True)
            if record_undo:
                G["age_undo"] = u
            dpg.configure_item("g_btn_undoages", enabled=bool(G.get("age_undo")))
            _set_status2(f"{o}/{n} âge(s) appliqué(s) sur Emby.",
                         f"{o}/{n} age(s) applied to Emby.")
            if after:
                try: after()
                except Exception: pass
            try:
                _rebuild_age_filter(); render_results()
            except Exception:
                pass
        ui(done)
    threading.Thread(target=thread, daemon=True).start()


def do_undo_ages():
    """Restaure les âges tels qu'ils étaient avant la dernière application en masse."""
    stack = list(G.get("age_undo") or [])
    if not stack:
        return
    pairs = [(iid, title, old) for iid, title, old in reversed(stack)]
    def _clear():
        G["age_undo"] = []
        dpg.configure_item("g_btn_undoages", enabled=False)
    _run_age_updates(pairs, record_undo=False,
                     lbl_fr="Annulation des âges", lbl_en="Undoing ages",
                     after=_clear)


def do_apply_all_higher():
    """Prévisualise puis applique l'âge web quand il est supérieur à l'enregistré."""
    global _mid
    if not dpg.get_value("inp_url").strip() or not dpg.get_value("inp_key").strip():
        modal_err(gx("Paramètres manquants", "Missing parameters"),
                  gx("Connectez-vous d'abord (bandeau de configuration en haut).",
                     "Connect first (configuration bar at the top)."))
        return
    targets = []
    for r in G["results"]:
        omdb = G["omdb_cache"].get(r.get("item_id", ""))
        if not isinstance(omdb, dict):
            continue
        raw = (omdb.get("rated", "") or "").strip()
        if not raw or raw.upper() in ("N/A", "NOT RATED", "UNRATED", "NR", "?"):
            continue
        web_n = _age_to_num(omdb.get("age", ""))
        rec_n = _age_to_num(_rated_to_age(r.get("rating", "")))
        if web_n is not None and rec_n is not None and web_n > rec_n:
            targets.append((r.get("item_id", ""), r.get("title", ""), raw,
                            _rated_to_age(r.get("rating", "")), omdb.get("age", "")))
    if not targets:
        modal_info(gx("Rien à appliquer", "Nothing to apply"),
                   gx("Aucun film n'a un âge web supérieur à l'âge enregistré.\n"
                      "(Pensez à lancer Enrichir d'abord.)",
                      "No movie has a web age higher than the recorded one.\n"
                      "(Remember to run Enrich first.)"))
        return
    # --- prévisualisation : cocher / décocher avant application ---
    _mid += 1
    tag = f"_prev{_mid}"
    def _confirm(s, a, u):
        sel = [(t2[0], t2[1], t2[2]) for i, t2 in enumerate(targets)
               if dpg.get_value(f"{u}_c{i}")]
        dpg.delete_item(u)
        if sel:
            _run_age_updates(sel, record_undo=True,
                             lbl_fr="Application des âges",
                             lbl_en="Applying ages")
    with dpg.window(label=gx("Prévisualisation - âges à appliquer",
                             "Preview - ages to apply"),
                    tag=tag, modal=True, width=640, height=520,
                    pos=[150, 110], no_resize=True):
        dpg.add_text(gx(f"{len(targets)} modification(s) proposée(s) - décochez pour exclure :",
                        f"{len(targets)} proposed change(s) - untick to exclude:"),
                     color=(255, 200, 80), wrap=610)
        with dpg.child_window(height=380, border=True):
            for i, (iid, title, raw, cur, web) in enumerate(targets):
                dpg.add_checkbox(label=f"{title[:52]}   {cur} -> {web}  ({raw})",
                                 tag=f"{tag}_c{i}", default_value=True)
        dpg.add_spacer(height=6)
        with dpg.group(horizontal=True):
            dpg.add_button(label=gx("Appliquer", "Apply"), width=180,
                           user_data=tag, callback=_confirm)
            dpg.add_button(label=gx("Annuler", "Cancel"), width=120,
                           user_data=tag,
                           callback=lambda s, a, u: dpg.delete_item(u))

def _fetch_one_omdb(imdb_id, key):
    """Retourne un dict enrichi ou leve une exception."""
    url = f"http://www.omdbapi.com/?i={imdb_id}&apikey={key}"
    req = urllib.request.Request(url,
          headers={"Accept":"application/json","User-Agent":"Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=8) as r:
        data = json.loads(r.read().decode())
    if data.get("Response") != "True":
        raise ValueError(data.get("Error","Inconnu"))
    rated = data.get("Rated","")
    return {"rated": rated, "age": _rated_to_age(rated),
            "note": data.get("imdbRating","-"), "source": "OMDB"}


def _fetch_one_tmdb(tmdb_id, key):
    """Retourne un dict enrichi depuis TMDB (certification FR puis US)."""
    url = (f"https://api.themoviedb.org/3/movie/{tmdb_id}"
           f"/release_dates?api_key={key}")
    req = urllib.request.Request(url,
          headers={"Accept":"application/json","User-Agent":"Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=8) as r:
        data = json.loads(r.read().decode())

    cert = ""
    results = {item["iso_3166_1"]: item["release_dates"]
               for item in data.get("results",[])}
    # Priorité FR -> DE -> GB -> US
    for country in ("FR","DE","GB","US"):
        dates = results.get(country,[])
        for d in dates:
            c = d.get("certification","").strip()
            if c:
                cert = c; break
        if cert: break

    # Note TMDB
    url2 = f"https://api.themoviedb.org/3/movie/{tmdb_id}?api_key={key}"
    req2 = urllib.request.Request(url2,
           headers={"Accept":"application/json","User-Agent":"Mozilla/5.0"})
    note = "-"
    try:
        with urllib.request.urlopen(req2, timeout=8) as r2:
            d2 = json.loads(r2.read().decode())
            v = d2.get("vote_average",0)
            note = f"{v:.1f}" if v else "-"
    except Exception:
        pass

    return {"rated": cert, "age": _rated_to_age(cert) if cert else "n/c",
            "note": note, "source": "TMDB"}


def _save_api_settings():
    """Sauvegarde clés + provider depuis les champs UI dans le JSON."""
    cfg = {
        "provider": G["api_provider"],
        "omdb_key": (dpg.get_value("inp_omdb_key") or "").strip(),
        "tmdb_key": (dpg.get_value("inp_tmdb_key") or "").strip(),
    }
    save_api_config(cfg)
    try:
        save_shared_creds(omdb_key=cfg["omdb_key"], tmdb_key=cfg["tmdb_key"],
                          provider=cfg["provider"])
        push_shared_to_all_tabs()
    except Exception:
        pass
    return cfg


def _set_webcell(iid, state):
    """Met a jour UNIQUEMENT la cellule 'Age web' d'un film (pas de re-rendu global)."""
    wid = G.get("_webcells", {}).get(iid)
    if not wid or not dpg.does_item_exist(wid):
        return
    try:
        if state == "pending":
            dpg.set_value(wid, "..."); dpg.configure_item(wid, color=(136, 136, 170))
        elif state == "error":
            dpg.set_value(wid, "err"); dpg.configure_item(wid, color=(180, 60, 60))
        elif isinstance(state, dict):
            dpg.set_value(wid, state.get("age", "-"))
            dpg.configure_item(wid, color=(100, 255, 160))
    except Exception:
        pass   # cellule re-rendue entre-temps : ignorer


# ------------------------------------------------------------------
#  Base SQLite de sauvegarde des recherches OMDB/TMDB
#  (economise le quota API et survit aux redemarrages / plantages)
#
#  - table web_meta   : age / note par identifiant (omdb=tt..., tmdb=123)
#  - table tmdb_query : resultats bruts des recherches TMDB (RefMatch)
#  Chaque resultat est ecrit IMMEDIATEMENT (plus de perte si on ferme
#  l'appli en cours d'enrichissement). L'ancien cache JSON est importe
#  automatiquement au premier lancement.
# ------------------------------------------------------------------
import json as _ejson
ENRICH_CACHE_FILE = APP_DIR / "emby_enrich_cache.json"     # ancien format (import)
WEB_DB_FILE       = APP_DIR / "emby_toolbox_web.db"
ENRICH_CACHE_DAYS    = 365   # resultat AVEC classification d'age
ENRICH_CACHE_DAYS_NC = 30    # resultat SANS classification (on retentera)
TMDB_QUERY_DAYS      = 30    # recherches TMDB brutes (RefMatch)
_CACHE_HITS = 0
_WEB_DB_LOCK = threading.Lock()
_WEB_DB = None
G_FORCE_REFRESH = {"on": False}   # case "Forcer" de l'explorateur de genres


def _web_db():
    """Connexion unique (partagee entre threads, protegee par _WEB_DB_LOCK)."""
    global _WEB_DB
    if _WEB_DB is not None:
        return _WEB_DB
    c = sqlite3.connect(str(WEB_DB_FILE), timeout=10, check_same_thread=False)
    c.execute("CREATE TABLE IF NOT EXISTS web_meta ("
              " src TEXT NOT NULL, fid TEXT NOT NULL,"
              " rated TEXT, age TEXT, note TEXT, source TEXT,"
              " title TEXT, ts REAL NOT NULL,"
              " PRIMARY KEY (src, fid))")
    c.execute("CREATE TABLE IF NOT EXISTS tmdb_query ("
              " qkey TEXT PRIMARY KEY, payload TEXT NOT NULL, ts REAL NOT NULL)")
    c.commit()
    _WEB_DB = c
    _import_json_cache(c)
    return c


def _import_json_cache(c):
    """Import unique de l'ancien emby_enrich_cache.json dans la base."""
    try:
        if not ENRICH_CACHE_FILE.exists():
            return
        if c.execute("SELECT COUNT(*) FROM web_meta").fetchone()[0]:
            return
        d = _ejson.loads(ENRICH_CACHE_FILE.read_text(encoding="utf-8"))
        rows = []
        for src in ("omdb", "tmdb"):
            for fid, e in (d.get(src) or {}).items():
                if isinstance(e, dict):
                    rows.append((src, str(fid), e.get("rated", ""), e.get("age", ""),
                                 e.get("note", ""), e.get("source", ""), "",
                                 float(e.get("ts", 0) or 0)))
        if rows:
            c.executemany("INSERT OR IGNORE INTO web_meta VALUES (?,?,?,?,?,?,?,?)", rows)
            c.commit()
        ENRICH_CACHE_FILE.rename(ENRICH_CACHE_FILE.with_suffix(".json.imported"))
    except Exception:
        traceback.print_exc()


def _cache_get(src, fid, count=True, ignore_age=False):
    """Resultat en base pour (src, fid) ou None (absent / perime)."""
    global _CACHE_HITS
    if not fid:
        return None
    try:
        with _WEB_DB_LOCK:
            row = _web_db().execute(
                "SELECT rated, age, note, source, ts FROM web_meta "
                "WHERE src=? AND fid=?", (src, str(fid))).fetchone()
    except Exception:
        return None
    if not row:
        return None
    res = {"rated": row[0] or "", "age": row[1] or "",
           "note": row[2] or "", "source": row[3] or ""}
    if not ignore_age:
        days = ENRICH_CACHE_DAYS if _usable_age(res) else ENRICH_CACHE_DAYS_NC
        if time.time() - (row[4] or 0) > days * 86400:
            return None
    if count:
        _CACHE_HITS += 1
    return res


def _cache_put(src, fid, res, title=""):
    if not isinstance(res, dict) or not fid:
        return
    try:
        with _WEB_DB_LOCK:
            c = _web_db()
            c.execute("INSERT OR REPLACE INTO web_meta VALUES (?,?,?,?,?,?,?,?)",
                      (src, str(fid), res.get("rated", ""), res.get("age", ""),
                       res.get("note", ""), res.get("source", ""), title or "",
                       time.time()))
            c.commit()
    except Exception:
        traceback.print_exc()


def _save_enrich_cache():
    """Conserve pour compatibilite : la base est ecrite au fil de l'eau."""
    return None


def _cache_suffix():
    return gx(f"  ({_CACHE_HITS} en cache)", f"  ({_CACHE_HITS} cached)") if _CACHE_HITS else ""


def web_db_stats():
    try:
        with _WEB_DB_LOCK:
            c = _web_db()
            n1 = c.execute("SELECT COUNT(*) FROM web_meta").fetchone()[0]
            n2 = c.execute("SELECT COUNT(*) FROM tmdb_query").fetchone()[0]
        return n1, n2
    except Exception:
        return 0, 0


def web_db_clear():
    with _WEB_DB_LOCK:
        c = _web_db()
        c.execute("DELETE FROM web_meta")
        c.execute("DELETE FROM tmdb_query")
        c.commit()


def tmdb_query_get(qkey):
    try:
        with _WEB_DB_LOCK:
            row = _web_db().execute("SELECT payload, ts FROM tmdb_query WHERE qkey=?",
                                    (qkey,)).fetchone()
        if row and time.time() - (row[1] or 0) <= TMDB_QUERY_DAYS * 86400:
            return _ejson.loads(row[0])
    except Exception:
        pass
    return None


def _tmdb_query_stale(qkey):
    """Comme tmdb_query_get mais sans limite d'age (secours hors ligne)."""
    try:
        with _WEB_DB_LOCK:
            row = _web_db().execute("SELECT payload FROM tmdb_query WHERE qkey=?",
                                    (qkey,)).fetchone()
        return _ejson.loads(row[0]) if row else None
    except Exception:
        return None


def tmdb_query_put(qkey, payload):
    try:
        with _WEB_DB_LOCK:
            c = _web_db()
            c.execute("INSERT OR REPLACE INTO tmdb_query VALUES (?,?,?)",
                      (qkey, _ejson.dumps(payload), time.time()))
            c.commit()
    except Exception:
        traceback.print_exc()


def _prefill_web_from_db(rows, provider):
    """Remplit G['omdb_cache'] depuis la base, SANS appel reseau, juste apres
    un scan : la colonne 'Age web' s'affiche donc immediatement pour les films
    deja enrichis lors d'une session precedente. Retourne le nombre trouve."""
    order = ("omdb", "tmdb") if provider == "omdb" else ("tmdb", "omdb")
    n = 0
    for r in rows:
        iid = r.get("item_id")
        if not iid:
            continue
        best = None
        for src in order:
            fid = r.get("imdb_id") if src == "omdb" else r.get("tmdb_id")
            res = _cache_get(src, fid, count=False, ignore_age=True)
            if res is None:
                continue
            if best is None:
                best = res
            if _usable_age(res):
                best = res
                break
        if best is not None:
            G["omdb_cache"][iid] = best
            n += 1
    return n


def _usable_age(res):
    """Vrai si le dict d'enrichissement porte une classification d'âge exploitable."""
    return isinstance(res, dict) and _age_to_num(res.get("age", "")) is not None


def _fetch_with_fallback(imdb_id, tmdb_id, provider, omdb_key, tmdb_key,
                         title="", force=False):
    """Interroge la source principale puis, si elle ne classe pas le film,
    se rabat sur l'autre source (OMDB <-> TMDB). Renvoie le meilleur résultat."""
    if provider == "omdb":
        order = [("omdb", imdb_id, omdb_key), ("tmdb", tmdb_id, tmdb_key)]
    else:
        order = [("tmdb", tmdb_id, tmdb_key), ("omdb", imdb_id, omdb_key)]
    best = None
    for src, fid, k in order:
        if not fid:
            continue
        # 1) base SQLite (sauf si "Forcer" est coche)
        res = None if force else _cache_get(src, fid)
        if res is None:
            if not k:
                continue
            try:
                res = _fetch_one_omdb(fid, k) if src == "omdb" else _fetch_one_tmdb(fid, k)
            except Exception:
                # reseau / quota KO : on se rabat sur la base, meme perimee
                res = _cache_get(src, fid, ignore_age=True)
                if res is None:
                    continue
            else:
                _cache_put(src, fid, res, title)
        if best is None:
            best = res
        if _usable_age(res):
            return res          # classification trouvée : on s'arrête
    if best is None:
        raise ValueError("aucune source exploitable")
    return best                  # pas de classification mais note/source dispo


def do_enrich():
    """Lance l'enrichissement avec le provider sélectionné (repli OMDB<->TMDB)."""
    global _CACHE_HITS
    _CACHE_HITS = 0
    cfg = _save_api_settings()
    provider = cfg["provider"]
    omdb_key = cfg["omdb_key"]
    tmdb_key = cfg["tmdb_key"]
    key = omdb_key if provider == "omdb" else tmdb_key

    if not key:
        modal_err(gx("Clé manquante","Missing key"),
                  gx(f"Clé {'OMDB' if provider=='omdb' else 'TMDB'} vide.",
                     f"{'OMDB' if provider=='omdb' else 'TMDB'} key is empty.")
                  + "\n\n"
                  + ("omdbapi.com  (gratuit, 1000/jour)"
                     if provider=="omdb"
                     else "themoviedb.org/settings/api  (gratuit, illimite)"))
        return

    # Collecter les films avec AU MOINS un identifiant (IMDB ou TMDB).
    # Le repli interrogera l'autre source si la principale ne classe pas.
    films = [(r.get("imdb_id",""), r.get("tmdb_id",""), r["title"], r.get("item_id",""))
             for r in G["results"]
             if r.get("item_id") and (r.get("imdb_id") or r.get("tmdb_id"))]

    if not films:
        modal_err(gx("Aucun ID","No ID"),
                  gx(f"Les films n'ont pas d'identifiant {'IMDB' if provider=='omdb' else 'TMDB'} dans Emby.",
                     f"Movies have no {'IMDB' if provider=='omdb' else 'TMDB'} id in Emby."))
        return

    lbl = "OMDB" if provider == "omdb" else "TMDB"
    for _, _, _, iid in films:
        if iid:
            G["omdb_cache"][iid] = "pending"
            _set_webcell(iid, "pending")
    dpg.configure_item("btn_enrich", enabled=False, label="Chargement...")
    dpg.set_value("lbl_status", gx(f"{lbl} : 0 / {len(films)} films enrichis...",
                         f"{lbl}: 0 / {len(films)} movies enriched..."))

    force = bool(G_FORCE_REFRESH["on"])

    def thread(films=films, provider=provider, lbl=lbl,
               omdb_key=omdb_key, tmdb_key=tmdb_key, force=force):
        ok = 0
        for idx, (imdb, tmdb, title, iid) in enumerate(films):
            if not iid or (not imdb and not tmdb):
                if iid:
                    G["omdb_cache"][iid] = "error"
                    ui(lambda iid=iid: _set_webcell(iid, "error"))
                continue
            try:
                hits0 = _CACHE_HITS
                result = _fetch_with_fallback(imdb, tmdb, provider,
                                              omdb_key, tmdb_key,
                                              title=title, force=force)
                G["omdb_cache"][iid] = result
                ok += 1
            except Exception:
                G["omdb_cache"][iid] = "error"
            # MAJ ciblee de la seule cellule concernee (pas de re-rendu global)
            ui(lambda iid=iid, st=G["omdb_cache"][iid]: _set_webcell(iid, st))
            # compteur de progression (leger)
            if (idx+1) % 5 == 0 or idx+1 == len(films):
                ui(lambda total=len(films), o=ok, lb=lbl:
                    dpg.set_value("lbl_status", gx(f"{lb} : {o} / {total} films enrichis",
                                         f"{lb}: {o} / {total} movies enriched")))
            if _CACHE_HITS == hits0:     # appel reseau reel : on menage l'API
                time.sleep(0.12 if provider == "omdb" else 0.05)

        # un seul rendu final : rafraichit tooltips, tri et boutons d'age web
        ui(lambda o=ok, total=len(films), lb=lbl: (
            dpg.configure_item("btn_enrich", enabled=True, label="Enrichir"),
            dpg.set_value("lbl_status", (_save_enrich_cache(),
                    _set_status2(f"{lb} terminé : {o}/{total} films enrichis"
                                 + (f"  ({_CACHE_HITS} en cache)" if _CACHE_HITS else ""),
                                 f"{lb} done: {o}/{total} movies enriched"
                                 + (f"  ({_CACHE_HITS} cached)" if _CACHE_HITS else "")))[1]
                              or dpg.get_value("lbl_status")),
            render_results()))

    threading.Thread(target=thread, daemon=True).start()


def _on_provider_change(sender, val, user_data):
    G["api_provider"] = val
    # cles OMDB/TMDB gerees dans la config commune (en haut) : on ne les reaffiche pas
    try:
        with dpg.theme() as th_a:
            with dpg.theme_component(dpg.mvButton):
                dpg.add_theme_color(dpg.mvThemeCol_Button, (233,69,96))
        with dpg.theme() as th_i:
            with dpg.theme_component(dpg.mvButton):
                dpg.add_theme_color(dpg.mvThemeCol_Button, (15,52,96))
        dpg.bind_item_theme("btn_prov_omdb", th_a if val=="omdb" else th_i)
        dpg.bind_item_theme("btn_prov_tmdb", th_a if val=="tmdb" else th_i)
    except Exception:
        pass


# ══════════════════════════════════════════════════════════════
#  FILTRE AGE
# ══════════════════════════════════════════════════════════════
def _rebuild_age_filter():
    """Reconstruit les cases de filtre age depuis les résultats courants.
    Le parent age_filter_panel est un group horizontal= True, donc
    chaque widget ajouté s'aligne automatiquement sur la même ligne."""
    dpg.delete_item("age_filter_panel", children_only=True)
    G["age_filter"] = set()

    ratings = sorted({(r.get("rating","") or "-") for r in G["results"]})
    if not ratings:
        return

    dpg.add_text("Age :", parent="age_filter_panel", color=(136,136,170))
    for rating in ratings:
        dpg.add_checkbox(
            label=rating,
            parent="age_filter_panel",
            default_value=False,
            user_data=rating,
            callback=lambda s,v,u: _toggle_age(u, v))

def _toggle_age(rating, checked):
    if checked: G["age_filter"].add(rating)
    else:       G["age_filter"].discard(rating)
    render_results()


# ══════════════════════════════════════════════════════════════
#  THÈME  (identique au script doublons)
# ══════════════════════════════════════════════════════════════
def setup_theme():
    ACCENT  = (233, 69, 96)
    ACCENT2 = (255, 110, 130)
    BLUE    = (15, 52, 96)
    BLUE_H  = (26, 80, 144)
    with dpg.theme() as th:
        with dpg.theme_component(dpg.mvAll):
            dpg.add_theme_color(dpg.mvThemeCol_WindowBg,       (26,26,46))
            dpg.add_theme_color(dpg.mvThemeCol_ChildBg,         (22,33,62))
            dpg.add_theme_color(dpg.mvThemeCol_PopupBg,         (24,35,66))
            dpg.add_theme_color(dpg.mvThemeCol_MenuBarBg,       (22,33,62))
            dpg.add_theme_color(dpg.mvThemeCol_FrameBg,         (13,27,42))
            dpg.add_theme_color(dpg.mvThemeCol_FrameBgHovered,  (20,40,60))
            dpg.add_theme_color(dpg.mvThemeCol_FrameBgActive,   (26,52,78))
            dpg.add_theme_color(dpg.mvThemeCol_Button,          BLUE)
            dpg.add_theme_color(dpg.mvThemeCol_ButtonHovered,   BLUE_H)
            dpg.add_theme_color(dpg.mvThemeCol_ButtonActive,    ACCENT)
            dpg.add_theme_color(dpg.mvThemeCol_Header,          BLUE)
            dpg.add_theme_color(dpg.mvThemeCol_HeaderHovered,   BLUE_H)
            dpg.add_theme_color(dpg.mvThemeCol_HeaderActive,    (30,90,160))
            dpg.add_theme_color(dpg.mvThemeCol_TableHeaderBg,   BLUE)
            dpg.add_theme_color(dpg.mvThemeCol_TableRowBg,      (30,42,58))
            dpg.add_theme_color(dpg.mvThemeCol_TableRowBgAlt,   (25,34,50))
            dpg.add_theme_color(dpg.mvThemeCol_TableBorderLight,(40,58,84))
            dpg.add_theme_color(dpg.mvThemeCol_TableBorderStrong,(50,72,104))
            dpg.add_theme_color(dpg.mvThemeCol_Text,            (228,230,236))
            dpg.add_theme_color(dpg.mvThemeCol_TextDisabled,    (136,136,170))
            dpg.add_theme_color(dpg.mvThemeCol_TitleBg,         BLUE)
            dpg.add_theme_color(dpg.mvThemeCol_TitleBgActive,   (20,66,120))
            dpg.add_theme_color(dpg.mvThemeCol_ScrollbarBg,     (16,24,40))
            dpg.add_theme_color(dpg.mvThemeCol_ScrollbarGrab,   (40,60,92))
            dpg.add_theme_color(dpg.mvThemeCol_ScrollbarGrabHovered, (55,82,124))
            dpg.add_theme_color(dpg.mvThemeCol_ScrollbarGrabActive,  ACCENT)
            dpg.add_theme_color(dpg.mvThemeCol_CheckMark,       ACCENT2)
            dpg.add_theme_color(dpg.mvThemeCol_SliderGrab,      ACCENT)
            dpg.add_theme_color(dpg.mvThemeCol_Border,          (38,56,84))
            dpg.add_theme_color(dpg.mvThemeCol_Separator,       (38,56,84))
            dpg.add_theme_color(dpg.mvThemeCol_SeparatorHovered,BLUE_H)
            # onglets : l'onglet actif ressort nettement (accent)
            dpg.add_theme_color(dpg.mvThemeCol_Tab,             (24,38,68))
            dpg.add_theme_color(dpg.mvThemeCol_TabHovered,      BLUE_H)
            dpg.add_theme_color(dpg.mvThemeCol_TabActive,       (176,46,72))
            dpg.add_theme_color(dpg.mvThemeCol_TabUnfocused,    (22,33,62))
            dpg.add_theme_color(dpg.mvThemeCol_TabUnfocusedActive, (30,52,84))
            # arrondis cohérents
            dpg.add_theme_style(dpg.mvStyleVar_FrameRounding,    5)
            dpg.add_theme_style(dpg.mvStyleVar_WindowRounding,   7)
            dpg.add_theme_style(dpg.mvStyleVar_ChildRounding,    6)
            dpg.add_theme_style(dpg.mvStyleVar_PopupRounding,    6)
            dpg.add_theme_style(dpg.mvStyleVar_ScrollbarRounding,6)
            dpg.add_theme_style(dpg.mvStyleVar_GrabRounding,     4)
            dpg.add_theme_style(dpg.mvStyleVar_TabRounding,      5)
            # un peu plus d'air
            dpg.add_theme_style(dpg.mvStyleVar_FramePadding,     7,5)
            dpg.add_theme_style(dpg.mvStyleVar_ItemSpacing,      8,6)
            dpg.add_theme_style(dpg.mvStyleVar_ItemInnerSpacing, 6,5)
            dpg.add_theme_style(dpg.mvStyleVar_WindowPadding,    10,8)
            dpg.add_theme_style(dpg.mvStyleVar_ScrollbarSize,    14)
    dpg.bind_theme(th)


# ══════════════════════════════════════════════════════════════
#  CONSTRUCTION UI
# ══════════════════════════════════════════════════════════════
_GENRES_TR = {
    "g_chk_force_web": ("Forcer", "Force"),
    "g_btn_webdb": ("Base", "DB"),
    "g_t_appname":     ("Explorateur de genres Emby", "Emby Genre Explorer"),
    "g_t_subtitle":    ("- trouver & déplacer par genre  (DirectX 11)",
                        "- find & move by genre  (DirectX 11)"),
    "g_t_ageweb":      ("Age web :", "Web age:"),
    "btn_enrich":      ("Enrichir", "Enrich"),
    "g_btn_applyhigher": ("Appliquer âges sup.", "Apply higher ages"),
    "g_chk_hidesame":  ("Masquer âges identiques", "Hide identical ages"),
    "g_btn_undoages":  ("Annuler âges", "Undo ages"),
    "btn_scan":        ("Rechercher", "Search"),
    "g_btn_export":    ("Exporter CSV", "Export CSV"),
    "g_t_lib_hint":    ("Cliquez sur Connecter pour charger les médiathèques.",
                        "Click Connect (top bar) to load your libraries."),
    "g_h_genres":      ("Genres à rechercher  (cocher = inclure dans la recherche)",
                        "Genres to search  (check = include in search)"),
    "g_btn_all":       ("Tout cocher", "Check all"),
    "g_btn_none":      ("Tout décocher", "Uncheck all"),
    "g_t_customgenre": ("Genre libre :", "Custom genre:"),
    "g_t_genre_hint":  ("Cliquez sur Connecter pour charger les genres de votre Emby.",
                        "Click Connect (top bar) to load your genres."),
    "g_t_filter":      ("Filtre :", "Filter:"),
}
_GENRES_HINTS = {
    "inp_filter":       ("Titre ou genre...", "Title or genre..."),
    "inp_custom_genre": ("ex: Anime, Kung Fu...", "e.g. Anime, Kung Fu..."),
}

def genres_apply_lang(code):
    """Traduit les libellés visibles de l'onglet Genres (FR/EN)."""
    G["lang"] = "en" if str(code).upper() == "EN" else "fr"
    i = 1 if str(code).upper() == "EN" else 0
    for tag, pair in _GENRES_TR.items():
        if not dpg.does_item_exist(tag):
            continue
        txt = pair[i]
        try:
            kind = dpg.get_item_type(tag)
            if "Button" in kind or "Collapsing" in kind or "Checkbox" in kind:
                dpg.configure_item(tag, label=txt)
            else:
                dpg.set_value(tag, txt)
        except Exception:
            pass
    for tag, pair in _GENRES_HINTS.items():
        if dpg.does_item_exist(tag):
            try:
                dpg.configure_item(tag, hint=pair[i])
            except Exception:
                pass
    # re-traduire les textes DYNAMIQUES déjà affichés
    try:
        if dpg.does_item_exist("g_genre_count") and G.get("genres_list"):
            n = len(G["genres_list"])
            dpg.set_value("g_genre_count",
                          gx(f"{n} genres chargés depuis votre Emby :",
                             f"{n} genres loaded from your Emby:"))
    except Exception:
        pass
    try:
        if G.get("results"):
            render_results()
    except Exception:
        pass
    ls = G.get("_last_status")
    if ls:
        try: dpg.set_value("lbl_status", gx(*ls))
        except Exception: pass
    # titres des popups de progression
    for tag, fr, en in (("scan_popup", "Recherche en cours", "Search in progress"),
                        ("apply_popup", "Application des âges", "Applying ages")):
        if dpg.does_item_exist(tag):
            try:
                dpg.configure_item(tag, label=gx(fr, en))
            except Exception:
                pass
    # infobulles statiques traduisibles
    for tag, fr, en in _GTIPS:
        if dpg.does_item_exist(tag):
            try:
                dpg.set_value(tag, gx(fr, en))
            except Exception:
                pass



def show_web_db_panel():
    """Petite fenetre : contenu de la base OMDB/TMDB + vidage."""
    tag = "g_webdb_win"
    if dpg.does_item_exist(tag):
        dpg.delete_item(tag)
    n1, n2 = web_db_stats()

    def _clear(s=None, a=None, u=None):
        try:
            web_db_clear()
            _set_status2("Base OMDB/TMDB vidée.", "OMDB/TMDB database cleared.")
        except Exception as e:
            modal_err(gx("Erreur base", "Database error"), str(e))
        if dpg.does_item_exist(tag):
            dpg.delete_item(tag)

    with dpg.window(label=gx("Base OMDB / TMDB", "OMDB / TMDB database"), tag=tag,
                    modal=True, width=460, autosize=True, pos=[260, 220], no_resize=True):
        dpg.add_text(str(WEB_DB_FILE), wrap=440, color=(136, 136, 170))
        dpg.add_text(gx(f"Âges / notes enregistrés : {n1}", f"Saved ages / scores: {n1}"))
        dpg.add_text(gx(f"Recherches TMDB enregistrées : {n2}", f"Saved TMDB searches: {n2}"))
        dpg.add_text(gx(f"Validité : {ENRICH_CACHE_DAYS} j (classé), {ENRICH_CACHE_DAYS_NC} j (non classé).\n"
                        "En cas d'erreur réseau, la valeur en base est utilisée même périmée.",
                        f"Validity: {ENRICH_CACHE_DAYS} d (rated), {ENRICH_CACHE_DAYS_NC} d (unrated).\n"
                        "On network errors, the stored value is used even if stale."),
                     wrap=440, color=(136, 136, 170))
        dpg.add_separator()
        with dpg.group(horizontal=True):
            dpg.add_button(label=gx("Vider la base", "Clear database"), width=150,
                           callback=_clear)
            dpg.add_button(label="OK", width=-1,
                           callback=lambda s, a, u: dpg.delete_item(tag))


def build_genres_popups():
    # Popup scan
    with dpg.window(label="Recherche en cours", tag="scan_popup", modal=True,
                    show=False, width=580, height=160, pos=[200,220],
                    no_close=True, no_resize=True):
        dpg.add_text("...", tag="scan_step", wrap=560)
        dpg.add_progress_bar(tag="scan_pb", default_value=0.0, width=-1, height=22)

    # Popup application des ages superieurs (progression)
    with dpg.window(label="Application des âges", tag="apply_popup", modal=True,
                    show=False, width=580, height=170, pos=[200,220],
                    no_close=True, no_resize=True):
        dpg.add_text("...", tag="apply_step", wrap=560)
        dpg.add_progress_bar(tag="apply_pb", default_value=0.0, width=-1, height=22)
        dpg.add_text("", tag="apply_detail", wrap=560, color=(136,136,170))

    # Fenêtre principale

def build_genres_body():

    # ── Titre ────────────────────────────────────────────
    with dpg.group(horizontal=True):
        dpg.add_text("Explorateur de genres Emby", tag="g_t_appname", color=(233,69,96))
        dpg.add_text("- trouver & déplacer par genre  (DirectX 11)",
                     tag="g_t_subtitle", color=(136,136,170))
        dpg.add_spacer(width=20)
    dpg.add_separator()

    # ── Ligne 1 : connexion (masquee : config commune en haut) ──
    with dpg.group(horizontal=True, tag="g_cfg_conn", show=False):
        dpg.add_text("URL")
        dpg.add_input_text(tag="inp_url",
                           default_value=CFG["emby"]["url"], width=210)
        dpg.add_spacer(width=6)
        dpg.add_text("Clé API")
        dpg.add_input_text(tag="inp_key",
                           default_value=CFG["emby"]["api_key"],
                           password=True, width=230)
        dpg.add_spacer(width=6)
        dpg.add_text("User ID")
        dpg.add_input_text(tag="inp_uid",
                           default_value=CFG["emby"].get("user_id",""),
                           width=110, hint="optionnel")
    dpg.add_spacer(height=3)

    # ── Ligne 2 : NAS (masquee) ──
    with dpg.group(horizontal=True, tag="g_cfg_nas", show=False):
        dpg.add_text("Prefixe Linux", color=(136,136,170))
        dpg.add_input_text(tag="inp_prefix",
                           default_value=CFG["emby"].get("nas_prefix","/volume1"),
                           width=170, hint="/volume1")
        gtip("Chemin Linux jusqu'au dossier partagé.\nEx: /volume1",
             "Linux path to the shared folder.\nEx: /volume1", wrap=300)
        dpg.add_text("->", color=(233,69,96))
        dpg.add_input_text(tag="inp_unc",
                           default_value=CFG["emby"].get("nas_unc",""),
                           width=280, hint=r"\\192.168.1.x\Films")
        gtip(r"Chemin UNC Windows. Ex: \\192.168.1.29",
             r"Windows UNC path. Ex: \\192.168.1.29", wrap=300)
    dpg.add_spacer(height=3)

    # ── Ligne 2b : Age web (provider + cle + enrichir) ───
    with dpg.group(horizontal=True):
        dpg.add_text("Age web :", tag="g_t_ageweb", color=(136,136,170))
        dpg.add_spacer(width=4)
        dpg.add_button(label="OMDB", tag="btn_prov_omdb", width=55,
            callback=lambda s,a,u: _on_provider_change(s,"omdb",u))
        dpg.add_button(label="TMDB", tag="btn_prov_tmdb", width=55,
            callback=lambda s,a,u: _on_provider_change(s,"tmdb",u))
        dpg.add_spacer(width=8)
        with dpg.group(horizontal=True, tag="grp_omdb_key", show=False):
            dpg.add_text("Cle OMDB :", color=(136,136,170))
            dpg.add_input_text(tag="inp_omdb_key", width=190,
                               default_value=API_CFG["omdb_key"],
                               hint="omdbapi.com (gratuit, 1000/j)")
        with dpg.group(horizontal=True, tag="grp_tmdb_key", show=False):
            dpg.add_text("Cle TMDB :", color=(136,136,170))
            dpg.add_input_text(tag="inp_tmdb_key", width=190,
                               default_value=API_CFG["tmdb_key"],
                               hint="themoviedb.org (illimite)")
        dpg.add_spacer(width=8)
        dpg.add_button(label="Enrichir", tag="btn_enrich",
                       callback=lambda s,a,u: do_enrich(), width=75)
        gtip("Récupère âge + note. Clés sauvegardées auto.\n"
             "Les résultats sont sauvegardés dans une base SQLite\n"
             "(emby_toolbox_web.db) : les films déjà connus ne\n"
             "consomment plus de quota et s'affichent dès le scan.",
             "Fetches age + score. Keys saved automatically.\n"
             "Results are saved in a SQLite database\n"
             "(emby_toolbox_web.db): already known movies no longer\n"
             "use API quota and show up right after the scan.", wrap=300)
        dpg.add_checkbox(label="Forcer", tag="g_chk_force_web", default_value=False,
                         callback=lambda s, v, u: G_FORCE_REFRESH.__setitem__("on", bool(v)))
        gtip("Ignore la base et réinterroge OMDB/TMDB pour tous les films\n"
             "(la base est mise à jour avec les nouvelles valeurs).",
             "Ignores the database and queries OMDB/TMDB again for every movie\n"
             "(the database is updated with the new values).", wrap=300)
        dpg.add_button(label="Base", tag="g_btn_webdb", width=50,
                       callback=lambda s, a, u: show_web_db_panel())
        gtip("Statistiques / vidage de la base de sauvegarde OMDB/TMDB.",
             "Stats / clearing of the OMDB/TMDB backup database.", wrap=300)
        dpg.add_spacer(width=6)
        dpg.add_button(label="Appliquer âges sup.", tag="g_btn_applyhigher",
                       callback=lambda s,a,u: do_apply_all_higher(), width=150)
        gtip("Applique automatiquement l'âge web sur Emby pour TOUS\nles films dont l'âge enrichi est SUPÉRIEUR à l'âge enregistré.",
             "Automatically applies the web age on Emby for ALL\nmovies whose enriched age is HIGHER than the recorded one.",
             wrap=300)
        dpg.add_spacer(width=8)
        dpg.add_button(label="Annuler âges", tag="g_btn_undoages",
                       callback=lambda s, a, u: do_undo_ages(),
                       width=105, enabled=False)
        gtip("Restaure les âges tels qu'ils étaient avant la dernière application en masse.",
             "Restores the ages as they were before the last bulk apply.", wrap=300)
        dpg.add_spacer(width=6)
        dpg.add_checkbox(label="Masquer âges identiques", tag="g_chk_hidesame",
                         default_value=False,
                         callback=lambda s,v,u: (_set_hide_same(v)))
        gtip("Masque les films dont l'âge web est égal à l'âge enregistré.",
             "Hides movies whose web age equals the recorded one.",
             wrap=300)
    dpg.add_spacer(height=3)

    # ── Ligne 2b : lecteur (masquee) ──
    with dpg.group(horizontal=True, tag="g_cfg_player", show=False):
        dpg.add_text("Lecteur video", color=(136,136,170))
        dpg.add_input_text(tag="inp_player",
                           default_value=CFG["emby"].get("player",""),
                           width=340, hint=r"C:\...\vlc.exe")
        gtip("Chemin vers VLC, MPC-HC, etc.\nVide = lecteur système.",
             "Path to VLC, MPC-HC, etc.\nEmpty = system player.", wrap=280)
        dpg.add_button(label="...", width=24,
            callback=lambda s,a,u: _browse_player())
    dpg.add_spacer(height=3)

    # ── Ligne 3 : boutons principaux ──────────────────────
    with dpg.group(horizontal=True):
        dpg.add_button(label="Connecter", tag="btn_connect", show=False,
                       callback=lambda s,a,u: do_connect(), width=100)
        gtip("Vérifie la connexion Emby et charge les médiathèques.",
             "Checks the Emby connection and loads the libraries.", wrap=300)
        dpg.add_button(label="Rechercher", tag="btn_scan",
                       callback=start_scan, width=110, enabled=False)
        gtip("Lance la recherche des films correspondant aux genres cochés.",
             "Searches for movies matching the checked genres.", wrap=300)
        dpg.add_spacer(width=10)
        dpg.add_button(label="Exporter CSV", tag="g_btn_export", callback=do_export_csv, width=110)
        gtip("Exporte la liste des résultats en CSV.",
             "Exports the results list to CSV.", wrap=280)
        dpg.add_spacer(width=20)
        dpg.add_text("", tag="lbl_status", color=(46,204,113))
    dpg.add_spacer(height=4)

    # ── Médiathèques ──────────────────────────────────────
    with dpg.child_window(tag="lib_panel", height=150,
                          border=True, autosize_x=True):
        dpg.add_text("Cliquez sur Connecter pour charger les médiathèques.",
                     tag="g_t_lib_hint", color=(136,136,170))
    dpg.add_spacer(height=4)

    # ── Genres (charges depuis Emby apres Connecter) ─────
    with dpg.collapsing_header(tag="g_h_genres",
            label="Genres à rechercher  (cocher = inclure dans la recherche)",
            default_open=True):
        dpg.add_spacer(height=4)
        with dpg.group(horizontal=True):
            dpg.add_button(label="Tout cocher", tag="g_btn_all", width=100,
                           callback=lambda s,a,u: _all_genres(True))
            dpg.add_button(label="Tout décocher", tag="g_btn_none", width=110,
                           callback=lambda s,a,u: _none_genres())
            dpg.add_spacer(width=14)
            dpg.add_text("Genre libre :", tag="g_t_customgenre", color=(136,136,170))
            dpg.add_input_text(tag="inp_custom_genre", width=160,
                               hint="ex: Anime, Kung Fu...")
            gtip("Genre absent de la liste (nom exact Emby, sensible à la casse).",
                 "Genre not in the list (exact Emby name, case-sensitive).", wrap=280)
        dpg.add_spacer(height=5)
        with dpg.child_window(tag="genre_panel", height=140,
                              border=False, autosize_x=True):
            dpg.add_text("Cliquez sur Connecter pour charger les genres de votre Emby.",
                         tag="g_t_genre_hint", color=(136,136,170))
        dpg.add_spacer(height=4)
    dpg.add_spacer(height=4)

    # ── Filtre résultats ──────────────────────────────────
    with dpg.group(horizontal=True):
        dpg.add_text("Filtre :", tag="g_t_filter", color=(136,136,170))
        dpg.add_input_text(tag="inp_filter", width=240,
                           hint="Titre ou genre...",
                           callback=on_filter, on_enter=False)
        dpg.add_spacer(width=10)
        # Filtre age - rempli dynamiquement après chaque scan
        with dpg.group(tag="age_filter_panel", horizontal=True):
            pass   # vide jusqu'au premier scan
    dpg.add_separator()

    # ── Zone résultats ────────────────────────────────────
    dpg.add_child_window(tag="results_area", border=False,
                         autosize_x=True, height=-1)


# ══════════════════════════════════════════════════════════════
#  POINT D'ENTRÉE
# ══════════════════════════════════════════════════════════════

# =====================================================================
#  OUTIL 2 : RefMatch (films & series)
# =====================================================================
APP_TITLE = "Emby RefMatch - vérification & re-référencement des films"

# Couleurs des boutons (R, G, B) - ajustables librement.
BTN_COLOR        = (45, 115, 190)    # bleu accent (boutons standard)
BTN_COLOR_HOVER  = (60, 145, 230)
BTN_COLOR_ACTIVE = (35, 95, 160)
BTN_OK_COLOR        = (40, 140, 70)  # vert (bouton « Appliquer »)
BTN_OK_COLOR_HOVER  = (55, 170, 90)
BTN_OK_COLOR_ACTIVE = (30, 110, 55)
BTN_TEXT_COLOR   = (240, 240, 240)
FONT_SIZE        = 17                 # police de l'interface

# ---------------------------------------------------------------------------
# Traductions de l'interface (FR / EN)
# ---------------------------------------------------------------------------
TR = {
    "FR": {
        "hdr_conn": "Connexion / Configuration",
        "subtitle": "- re-referencement des films & series  (DirectX 11)",
        "lbl_emby_url": "Emby URL",
        "lbl_emby_key": "Clé API Emby",
        "lbl_tmdb_key": "Clé TMDB",
        "lbl_lang": "Langue",
        "btn_connect": "Connecter",
        "btn_save": "Enregistrer config",
        "hdr_libs": "Médiathèques à scanner",
        "btn_all": "Tout",
        "btn_none": "Aucun",
        "hint_connect": "(clique « Connecter » pour lister)",
        "hdr_detect": "Détection des films/séries mal référencés",
        "lbl_kind": "Type :",
        "chk_noid": "Pas d'ID TMDB/IMDB",
        "chk_noart": "Pas de poster / résumé",
        "chk_all": "Tout re-scanner (audit)",
        "btn_scan": "Scanner la bibliothèque",
        "btn_auto": "Auto-corriger (score 1.0)",
        "auto_confirm_title": "Confirmation",
        "auto_confirm_msg": "L'auto-correction va traiter %d film(s) et appliquer les\ncorrespondances TMDB de similarité >= 95%% (dans la tolérance\nd'année). Cette opération écrit sur le serveur Emby. Continuer ?",
        "btn_yes": "Oui, corriger",
        "btn_no": "Annuler",
        "lbl_to_fix": "Films à corriger",
        "col_title": "Titre",
        "col_year": "Année",
        "col_problem": "Problème",
        "lbl_detail": "Détail du film sélectionné",
        "detail_placeholder": "(sélectionne un film à gauche)",
        "lbl_search_title": "Titre",
        "lbl_search_year": "Année",
        "lbl_tol": "± ans",
        "btn_search": "Rechercher candidats",
        "chk_replace_img": "Remplacer aussi les images (ReplaceAllImages)",
        "lbl_candidates": "Candidats (le 1er = le plus probable)",
        "lbl_journal": "Journal",
        # éléments dynamiques
        "btn_apply": "Appliquer ce film",
        "cand_best": "   <- meilleur",
        "cand_none": "Aucun candidat. Ajuste le titre/année et relance.",
        "cand_no_overview": "(pas de résumé)",
        "libs_nothing": "(rien à afficher)",
        "hint_no_libs": "Aucune médiathèque listée.",
        "hint_count": "%d médiathèque(s) - coche celles à scanner.",
        "connecting": "Connexion...",
        "detail_tpl": ("Emby    : %s (%s)\nFichier : %s\nDeviné  : %s  [%s]\n"
                       "IDs     : Tmdb=%s  Imdb=%s\nProblème: %s"),
        "detail_done": "Film corrigé. Sélectionne le suivant.",
        "status_scan": "Scan en cours...",
        "status_auto": "Auto-correction en cours...",
        "status_to_fix": "%d / %d films à corriger",
        "status_remaining": "%d film(s) restant(s)",
        "prog_counting": "Comptage des films...",
        "prog_scan": "Analyse : %d / %d",
        "prog_auto": "Auto : %d / %d",
        "prog_done": "Terminé",
        "log_crypto_dpapi": "Clés API : chiffrées (Windows DPAPI).",
        "log_crypto_fernet": "Clés API : chiffrées (Fernet, clé locale .key).",
        "log_crypto_plain": "Clés API NON chiffrées : installez 'cryptography' ou lancez sous Windows.",
        "log_crypto_migrated": "Clés API existantes chiffrées dans la config.",
        # journal
        "log_ready": "Prêt. Renseigne la config, connecte-toi, puis scanne.",
        "log_no_pil": "Pillow absent : posters non affichés (pip install pillow).",
        "log_saved": "Configuration enregistrée.",
        "log_save_err": "Erreur sauvegarde config : %s",
        "log_emby_ok": "Emby OK : %s v%s",
        "log_emby_fail": "Emby ÉCHEC : %s",
        "log_libs_found": "%d médiathèque(s) trouvée(s).",
        "log_libs_err": "Erreur listing médiathèques : %s",
        "log_tmdb_ok": "TMDB OK.",
        "log_tmdb_fail": "TMDB ÉCHEC : %s",
        "log_tmdb_nokey": "TMDB : pas de clé (Emby RemoteSearch utilisé seul).",
        "log_need_crit": "Coche au moins un critère de détection.",
        "log_need_lib": "Coche au moins une médiathèque (ou clique Connecter).",
        "log_scan_targets": "Scan : %s",
        "log_scan_lib": "-> médiathèque « %s »...",
        "log_scan_progress": "... %d films analysés (%d à corriger)",
        "log_scan_err": "Erreur scan : %s",
        "log_scan_done": "Scan terminé : %d films, %d à corriger.",
        "log_nothing": "Rien à corriger : lance d'abord un scan.",
        "log_auto_needkey": "Mode auto : une clé TMDB est requise.",
        "log_auto_start": "Auto-correction : %d film(s) à examiner (seuil score 1.0)...",
        "log_tmdb_err_name": "TMDB erreur (%s) : %s",
        "log_auto_ok": "Auto : %s (%s) - score 1.0",
        "log_auto_fail": "Auto échec (%s) : %s",
        "log_auto_done": "Auto-correction terminée : %d corrigé(s), %d restant(s).",
        "log_pick_first": "Sélectionne d'abord un film dans la liste.",
        "log_search_empty": "Titre de recherche vide.",
        "log_search_start": "Recherche de candidats pour : %s (%s)",
        "log_cand_count": "%d candidat(s) trouvé(s).",
        "log_tmdb_err": "TMDB erreur : %s",
        "log_emby_rs_err": "Emby RemoteSearch erreur : %s",
        "log_apply_start": "Application sur « %s » -> %s (%s) [%s]...",
        "log_apply_noid": "Candidat sans ID exploitable, abandon.",
        "log_apply_ok": "Appliqué + refresh lancé côté Emby pour « %s ».",
        "log_apply_fail": "Échec application : %s",
    },
    "EN": {
        "hdr_conn": "Connection / Configuration",
        "subtitle": "- movies & series re-matching  (DirectX 11)",
        "lbl_emby_url": "Emby URL",
        "lbl_emby_key": "Emby API key",
        "lbl_tmdb_key": "TMDB key",
        "lbl_lang": "Language",
        "btn_connect": "Connect",
        "btn_save": "Save config",
        "hdr_libs": "Libraries to scan",
        "btn_all": "All",
        "btn_none": "None",
        "hint_connect": "(click \"Connect\" to list)",
        "hdr_detect": "Detection of mis-referenced movies/series",
        "lbl_kind": "Type:",
        "chk_noid": "No TMDB/IMDB ID",
        "chk_noart": "No poster / overview",
        "chk_all": "Re-scan everything (audit)",
        "btn_scan": "Scan library",
        "btn_auto": "Auto-fix (score 1.0)",
        "auto_confirm_title": "Confirmation",
        "auto_confirm_msg": "Auto-fix will process %d movie(s) and apply TMDB matches\nwith similarity >= 95%% (within the year tolerance).\nThis writes to the Emby server. Continue?",
        "btn_yes": "Yes, fix",
        "btn_no": "Cancel",
        "lbl_to_fix": "Movies to fix",
        "col_title": "Title",
        "col_year": "Year",
        "col_problem": "Problem",
        "lbl_detail": "Selected movie details",
        "detail_placeholder": "(select a movie on the left)",
        "lbl_search_title": "Title",
        "lbl_search_year": "Year",
        "lbl_tol": "± yrs",
        "btn_search": "Search candidates",
        "chk_replace_img": "Also replace images (ReplaceAllImages)",
        "lbl_candidates": "Candidates (1st = most likely)",
        "lbl_journal": "Log",
        "btn_apply": "Apply this match",
        "cand_best": "   <- best",
        "cand_none": "No candidate. Adjust title/year and retry.",
        "cand_no_overview": "(no overview)",
        "libs_nothing": "(nothing to show)",
        "hint_no_libs": "No library listed.",
        "hint_count": "%d library(ies) - tick those to scan.",
        "connecting": "Connecting...",
        "detail_tpl": ("Emby   : %s (%s)\nFile   : %s\nGuessed: %s  [%s]\n"
                       "IDs    : Tmdb=%s  Imdb=%s\nProblem: %s"),
        "detail_done": "Movie fixed. Select the next one.",
        "status_scan": "Scanning...",
        "status_auto": "Auto-fix running...",
        "status_to_fix": "%d / %d movies to fix",
        "status_remaining": "%d movie(s) remaining",
        "prog_counting": "Counting movies...",
        "prog_scan": "Scanning: %d / %d",
        "prog_auto": "Auto: %d / %d",
        "prog_done": "Done",
        "log_crypto_dpapi": "API keys: encrypted (Windows DPAPI).",
        "log_crypto_fernet": "API keys: encrypted (Fernet, local .key file).",
        "log_crypto_plain": "API keys NOT encrypted: install 'cryptography' or run on Windows.",
        "log_crypto_migrated": "Existing API keys encrypted in config.",
        "log_ready": "Ready. Fill the config, connect, then scan.",
        "log_no_pil": "Pillow missing: posters not shown (pip install pillow).",
        "log_saved": "Configuration saved.",
        "log_save_err": "Config save error: %s",
        "log_emby_ok": "Emby OK: %s v%s",
        "log_emby_fail": "Emby FAILED: %s",
        "log_libs_found": "%d library(ies) found.",
        "log_libs_err": "Library listing error: %s",
        "log_tmdb_ok": "TMDB OK.",
        "log_tmdb_fail": "TMDB FAILED: %s",
        "log_tmdb_nokey": "TMDB: no key (Emby RemoteSearch used alone).",
        "log_need_crit": "Tick at least one detection criterion.",
        "log_need_lib": "Tick at least one library (or click Connect).",
        "log_scan_targets": "Scan: %s",
        "log_scan_lib": "-> library \"%s\"...",
        "log_scan_progress": "... %d movies analyzed (%d to fix)",
        "log_scan_err": "Scan error: %s",
        "log_scan_done": "Scan done: %d movies, %d to fix.",
        "log_nothing": "Nothing to fix: run a scan first.",
        "log_auto_needkey": "Auto mode: a TMDB key is required.",
        "log_auto_start": "Auto-fix: %d movie(s) to check (score 1.0 threshold)...",
        "log_tmdb_err_name": "TMDB error (%s): %s",
        "log_auto_ok": "Auto: %s (%s) - score 1.0",
        "log_auto_fail": "Auto fail (%s): %s",
        "log_auto_done": "Auto-fix done: %d fixed, %d remaining.",
        "log_pick_first": "Select a movie from the list first.",
        "log_search_empty": "Empty search title.",
        "log_search_start": "Searching candidates for: %s (%s)",
        "log_cand_count": "%d candidate(s) found.",
        "log_tmdb_err": "TMDB error: %s",
        "log_emby_rs_err": "Emby RemoteSearch error: %s",
        "log_apply_start": "Applying on \"%s\" -> %s (%s) [%s]...",
        "log_apply_noid": "Candidate has no usable ID, aborting.",
        "log_apply_ok": "Applied + refresh triggered on Emby for \"%s\".",
        "log_apply_fail": "Apply failed: %s",
    },
}

def _base_dir():
    return str(APP_DIR)

CONFIG_PATH = os.path.join(_base_dir(), "emby_refmatch.ini")
HTTP_TIMEOUT = 25
PAGE_SIZE = 500
TMDB_BASE = "https://api.themoviedb.org/3"
TMDB_IMG = "https://image.tmdb.org/t/p/w185"
YEAR_TOLERANCE = 2          # fenetre +/- annees par defaut pour la correspondance
AUTO_TITLE_MIN = 0.95       # similarite de titre minimale pour l'auto-correction


# ---------------------------------------------------------------------------
# Chiffrement des clés API sauvegardées
#   - Windows : DPAPI (CryptProtectData) - lié au compte Windows, rien à gérer
#   - autres  : Fernet (cryptography) si installé, clé locale dans un fichier
#   - sinon   : stockage en clair (avec avertissement au démarrage)
# Les valeurs chiffrées sont préfixées "enc:<methode>:" dans le .ini.
# ---------------------------------------------------------------------------


# Tags release à retirer du nom de fichier pour deviner le vrai titre.
_RELEASE_TOKENS = {
    # résolutions / sources
    "480p", "576p", "720p", "1080p", "1440p", "2160p", "4k", "uhd", "hd", "sd",
    "bluray", "blu-ray", "brrip", "bdrip", "bdremux", "remux", "web-dl", "webdl",
    "webrip", "web", "hdrip", "hdtv", "dvdrip", "dvd", "dvdr", "hddvd", "vodrip",
    "cam", "ts", "tc", "telesync", "hdcam", "r5",
    # codecs vidéo
    "x264", "x265", "h264", "h265", "h.264", "h.265", "hevc", "avc", "xvid",
    "divx", "vc-1", "10bit", "8bit", "hdr", "hdr10", "dovi", "dv", "sdr",
    # audio
    "aac", "ac3", "eac3", "dd", "dd5.1", "ddp", "ddp5.1", "dts", "dts-hd",
    "dtshd", "truehd", "atmos", "flac", "mp3", "5.1", "7.1", "2.0", "ma",
    # langues / pistes
    "multi", "multilang", "vff", "vfq", "vfi", "vof", "vfo", "truefrench",
    "french", "fr", "fra", "fre", "vostfr", "vost", "subfrench", "subbed",
    "eng", "en", "ita", "spa", "ger", "vf", "vo", "custom", "integrale",
    # divers
    "repack", "proper", "internal", "limited", "extended", "uncut", "unrated",
    "remastered", "directors", "cut", "imax", "complete", "readnfo",
}

_BRACKET_RX = re.compile(r"[\[\(\{][^\]\)\}]*[\]\)\}]")
_YEAR_RX = re.compile(r"(19\d{2}|20\d{2})")


# ---------------------------------------------------------------------------
# Parsing du nom de fichier -> (titre probable, année probable)
# ---------------------------------------------------------------------------
def guess_title_year(name, path=""):
    """Retourne (titre_nettoye, annee|None) à partir du Name Emby et/ou du Path."""
    candidates = []
    if name:
        candidates.append(name)
    if path:
        base = os.path.splitext(os.path.basename(path.replace("\\", "/").rstrip("/")))[0]
        candidates.append(base)
        # dossier parent (souvent "Titre (1999)")
        parent = os.path.basename(os.path.dirname(path.replace("\\", "/")))
        if parent:
            candidates.append(parent)

    best_title, best_year = (name or "").strip(), None
    for raw in candidates:
        title, year = _parse_one(raw)
        if year and (best_year is None):
            best_year = year
        # on garde le titre le plus "propre" (le plus court non vide après nettoyage)
        if title and (not best_title or (year and not best_year)):
            best_title = title
        if year and title:
            best_title, best_year = title, year
            break
    return best_title.strip() or (name or "").strip(), best_year


def _parse_one(raw):
    s = raw.replace(".", " ").replace("_", " ")
    # année avant suppression des crochets (souvent "(1999)")
    years = _YEAR_RX.findall(s)
    year = None
    if years:
        now = time.localtime().tm_year + 1
        for y in years:
            iy = int(y)
            if 1900 <= iy <= now:
                year = iy  # on prend la dernière occurrence plausible
    s = _BRACKET_RX.sub(" ", s)
    s = re.sub(r"[-]", " ", s)
    parts = [p for p in re.split(r"\s+", s) if p]
    out = []
    for p in parts:
        low = p.lower().strip(",;")
        if low in _RELEASE_TOKENS:
            break  # tout ce qui suit un tag release est du bruit
        if _YEAR_RX.fullmatch(low) and year and int(low) == year:
            break  # on coupe au niveau de l'année
        out.append(p)
    title = " ".join(out).strip(" --_")
    return title, year


def score_candidate(q_title, q_year, c_title, c_orig, c_year, popularity):
    """Score 0..1 : similarité de titre + bonus année + petit poids popularité."""
    q = (q_title or "").lower().strip()
    sims = []
    for t in (c_title, c_orig):
        if t:
            sims.append(SequenceMatcher(None, q, t.lower().strip()).ratio())
    title_sim = max(sims) if sims else 0.0

    year_bonus = 0.0
    if q_year and c_year:
        d = abs(int(q_year) - int(c_year))
        if d == 0:
            year_bonus = 0.18
        elif d == 1:
            year_bonus = 0.08
        elif d <= 2:
            year_bonus = 0.03
        else:
            year_bonus = -0.10
    pop = 0.0
    try:
        pop = min(float(popularity or 0) / 200.0, 1.0) * 0.06
    except Exception:
        pop = 0.0
    return max(0.0, min(1.0, title_sim * 0.85 + year_bonus + pop)), title_sim


def tmdb_year(r):
    """Année de sortie depuis un résultat TMDB (release_date) ou None."""
    rd = r.get("release_date") or ""
    if len(rd) >= 4 and rd[:4].isdigit():
        return int(rd[:4])
    return None


# ---------------------------------------------------------------------------
# Client Emby
# ---------------------------------------------------------------------------
class EmbyClient:
    def __init__(self, url, api_key, language="fr"):
        self.url = (url or "").rstrip("/")
        self.api_key = api_key or ""
        self.language = language or "fr"
        self.s = requests.Session()
        self.s.headers.update({"X-Emby-Token": self.api_key,
                               "Accept": "application/json"})

    def ping(self):
        r = self.s.get(self.url + "/System/Info", timeout=HTTP_TIMEOUT)
        r.raise_for_status()
        return r.json()

    def get_libraries(self):
        """Liste les médiathèques (dossiers virtuels) : name, type, id."""
        r = self.s.get(self.url + "/Library/VirtualFolders", timeout=HTTP_TIMEOUT)
        r.raise_for_status()
        libs = []
        for v in (r.json() or []):
            lid = v.get("ItemId") or v.get("Id")
            if not lid:
                continue
            libs.append({
                "name": v.get("Name", "?"),
                "type": (v.get("CollectionType") or "mixed").lower(),
                "id": lid,
            })
        return libs

    def count_items(self, parent_id=None, item_type="Movie"):
        """Nombre total d'items du type donné (pour la barre de progression)."""
        params = {
            "Recursive": "true",
            "IncludeItemTypes": item_type,
            "Limit": 1,
            "EnableTotalRecordCount": "true",
        }
        if parent_id:
            params["ParentId"] = parent_id
        r = self.s.get(self.url + "/Items", params=params, timeout=HTTP_TIMEOUT)
        r.raise_for_status()
        return int((r.json() or {}).get("TotalRecordCount", 0) or 0)

    # compat : ancien nom
    def count_movies(self, parent_id=None):
        return self.count_items(parent_id, "Movie")

    def iter_items(self, parent_id=None, item_type="Movie"):
        """Génère les items du type donné (paginés). Films ou Séries."""
        start = 0
        fields = "ProviderIds,Overview,Path,ProductionYear,OriginalTitle,PremiereDate"
        while True:
            params = {
                "Recursive": "true",
                "IncludeItemTypes": item_type,
                "Fields": fields,
                "EnableImageTypes": "Primary",
                "SortBy": "SortName",
                "SortOrder": "Ascending",
                "StartIndex": start,
                "Limit": PAGE_SIZE,
            }
            if parent_id:
                params["ParentId"] = parent_id
            r = self.s.get(self.url + "/Items", params=params, timeout=HTTP_TIMEOUT)
            r.raise_for_status()
            data = r.json()
            items = data.get("Items", []) or []
            for it in items:
                yield it
            total = data.get("TotalRecordCount", 0)
            start += len(items)
            if not items or start >= total:
                break

    # compat : ancien nom
    def iter_movies(self, parent_id=None):
        return self.iter_items(parent_id, "Movie")

    def iter_items_sources(self, parent_id=None, item_type="Movie"):
        """Comme iter_items, mais demande MediaSources : indispensable pour
        reperer les fiches auxquelles PLUSIEURS fichiers sont rattaches."""
        start = 0
        fields = "Path,MediaSources,ProductionYear,OriginalTitle,ProviderIds"
        while True:
            params = {"Recursive": "true", "IncludeItemTypes": item_type,
                      "Fields": fields, "SortBy": "SortName",
                      "SortOrder": "Ascending", "StartIndex": start,
                      "Limit": PAGE_SIZE}
            if parent_id:
                params["ParentId"] = parent_id
            r = self.s.get(self.url + "/Items", params=params,
                           timeout=HTTP_TIMEOUT)
            r.raise_for_status()
            data = r.json()
            items = data.get("Items", []) or []
            for it in items:
                yield it, int(data.get("TotalRecordCount", 0) or 0)
            start += len(items)
            if not items or start >= int(data.get("TotalRecordCount", 0) or 0):
                break

    def remote_search(self, item_id, name, year, kind="movie"):
        """Recherche distante Emby. kind 'movie' -> /RemoteSearch/Movie,
        'tv' -> /RemoteSearch/Series."""
        endpoint = ("/Items/RemoteSearch/Series" if kind == "tv"
                    else "/Items/RemoteSearch/Movie")
        body = {
            "SearchInfo": {
                "Name": name,
                "Year": int(year) if year else None,
                "ProviderIds": {},
                "MetadataLanguage": self.language,
                "MetadataCountryCode": (self.language or "fr").upper()[:2],
            },
            "ItemId": item_id,
            "SearchProviderName": None,
            "IncludeDisabledProviders": True,
        }
        r = self.s.post(self.url + endpoint, json=body, timeout=HTTP_TIMEOUT)
        r.raise_for_status()
        return r.json() or []

    # compat : ancien nom
    def remote_search_movie(self, item_id, name, year):
        return self.remote_search(item_id, name, year, "movie")

    def apply_remote_result(self, item_id, result, replace_images=True):
        """result = dict type RemoteSearchResult (au minimum ProviderIds)."""
        params = {"ReplaceAllImages": "true" if replace_images else "false"}
        r = self.s.post(self.url + "/Items/RemoteSearch/Apply/" + str(item_id),
                        params=params, json=result, timeout=HTTP_TIMEOUT)
        r.raise_for_status()
        return True

    def primary_image_url(self, item_id, tag=None, max_h=160):
        u = self.url + "/Items/%s/Images/Primary?maxHeight=%d&api_key=%s" % (
            item_id, max_h, self.api_key)
        if tag:
            u += "&tag=" + tag
        return u


# ---------------------------------------------------------------------------
# Client TMDB
# ---------------------------------------------------------------------------
class TmdbClient:
    def __init__(self, api_key, language="fr-FR"):
        self.api_key = api_key or ""
        self.language = language or "fr-FR"
        self.s = requests.Session()

    def search(self, query, year=None, kind="movie"):
        """Recherche TMDB. kind = 'movie' ou 'tv'. Les résultats TV sont
        normalisés en forme 'film' (title/original_title/release_date) pour
        que le reste du code reste identique pour films et séries."""
        if not self.api_key:
            return []
        endpoint = "/search/tv" if kind == "tv" else "/search/movie"
        params = {"api_key": self.api_key, "query": query,
                  "language": self.language, "include_adult": "false"}
        if year:
            if kind == "tv":
                params["first_air_date_year"] = int(year)
            else:
                params["year"] = int(year)
        # base SQLite : evite de refaire la meme recherche (et marche hors ligne)
        qkey = "search|%s|%s|%s|%s" % (kind, self.language,
                                       (query or "").strip().lower(), year or "")
        cached = tmdb_query_get(qkey)
        if cached is not None:
            return cached
        try:
            r = self.s.get(TMDB_BASE + endpoint, params=params, timeout=HTTP_TIMEOUT)
            r.raise_for_status()
        except Exception:
            # TMDB injoignable : on tente une version perimee de la base
            old = _tmdb_query_stale(qkey)
            if old is not None:
                return old
            raise
        results = (r.json() or {}).get("results", []) or []
        if kind == "tv":
            for r2 in results:
                r2.setdefault("title", r2.get("name"))
                r2.setdefault("original_title", r2.get("original_name"))
                r2.setdefault("release_date", r2.get("first_air_date"))
        if results:
            tmdb_query_put(qkey, results)
        return results

    def external_ids(self, movie_id, kind="movie"):
        if not self.api_key:
            return {}
        base = "/tv/%s/external_ids" if kind == "tv" else "/movie/%s/external_ids"
        qkey = "extids|%s|%s" % (kind, movie_id)
        cached = tmdb_query_get(qkey)
        if cached is not None:
            return cached
        try:
            r = self.s.get(TMDB_BASE + base % movie_id,
                           params={"api_key": self.api_key}, timeout=HTTP_TIMEOUT)
            r.raise_for_status()
            data = r.json() or {}
            if data:
                tmdb_query_put(qkey, data)
            return data
        except Exception:
            return _tmdb_query_stale(qkey) or {}


# ---------------------------------------------------------------------------
# File d'attente UI (thread-safe)
# ---------------------------------------------------------------------------
_ui_queue = queue.Queue()

def ui_post(fn):
    _ui_queue.put(fn)

def drain_ui_queue():
    while True:
        try:
            fn = _ui_queue.get_nowait()
        except queue.Empty:
            break
        try:
            try:
                fn()
            except Exception:
                pass
        except Exception:
            traceback.print_exc()


# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
class App:
    def __init__(self):
        self.cfg = configparser.ConfigParser()
        self.emby = None
        self.tmdb = None
        self.flagged = []          # films détectés (dicts Emby + 'reason')
        self.libraries = []        # médiathèques listées après "Connecter"
        self.current = None        # film sélectionné
        self.candidates = []       # candidats affichés
        self._tex_tags = []        # textures dynamiques à nettoyer
        self._gen = 0              # génération (anti-collision threads)
        self._scanning = False
        self.emby_type = "Movie"   # type d'items courant : "Movie" ou "Series"
        self.tmdb_kind = "movie"   # côté TMDB : "movie" ou "tv"
        self._i18n = []            # widgets traduisibles : (tag, kind, key)
        self._font_loaded = False
        self._font_path = None
        self._secrets_migrated = False
        self.load_config()
        self.lang = (self.get("options", "lang", "FR") or "FR").upper()
        if self.lang not in TR:
            self.lang = "FR"
        self._migrate_secrets()

    def _migrate_secrets(self):
        """Chiffre les clés encore en clair dans le .ini (au 1er lancement)."""
        changed = False
        for sec, key in (("emby", "api_key"), ("tmdb", "api_key")):
            val = self.get(sec, key)
            if val and not val.startswith("enc:"):
                enc = encrypt_secret(val)
                if enc != val:            # un chiffrement a bien été appliqué
                    self.cfg.set(sec, key, enc)
                    changed = True
        if changed:
            try:
                with open(CONFIG_PATH, "w", encoding="utf-8") as fh:
                    self.cfg.write(fh)
                self._secrets_migrated = True
            except Exception:
                pass

    def t(self, key):
        return TR.get(self.lang, TR["FR"]).get(key, TR["FR"].get(key, key))

    # ----- config -----
    def load_config(self):
        if os.path.exists(CONFIG_PATH):
            try:
                self.cfg.read(CONFIG_PATH, encoding="utf-8")
            except Exception:
                pass
        for sec in ("emby", "tmdb", "options"):
            if not self.cfg.has_section(sec):
                self.cfg.add_section(sec)

    def get(self, sec, key, default=""):
        try:
            return self.cfg.get(sec, key)
        except Exception:
            return default

    def get_secret(self, sec, key):
        """Lit et déchiffre une clé API stockée dans le .ini."""
        return decrypt_secret(self.get(sec, key))

    def save_config(self):
        self.cfg.set("emby", "url", dpg.get_value("emby_url"))
        self.cfg.set("emby", "api_key", encrypt_secret(dpg.get_value("emby_key")))
        self.cfg.set("tmdb", "api_key", encrypt_secret(dpg.get_value("tmdb_key")))
        self.cfg.set("options", "lang", self.lang)
        if dpg.does_item_exist("rm_kind"):
            _v = (dpg.get_value("rm_kind") or "").strip().lower()
            _ser = _v in ("séries","series","tv shows","séries/tv","series/tv")
            self.cfg.set("options", "kind", "Séries" if _ser else "Films")
        if dpg.does_item_exist("search_tol"):
            self.cfg.set("options", "year_tol", str(self._read_tol()))
        self.cfg.set("options", "no_id", str(dpg.get_value("crit_noid")))
        self.cfg.set("options", "no_art", str(dpg.get_value("crit_noart")))
        self.cfg.set("options", "all", str(dpg.get_value("crit_all")))
        sel = []
        for i in range(len(self.libraries)):
            tag = "lib_chk_%d" % i
            if dpg.does_item_exist(tag) and dpg.get_value(tag):
                sel.append(self.libraries[i]["name"])
        self.cfg.set("options", "selected_libs", "|".join(sel))
        try:
            save_shared_creds(url=dpg.get_value("emby_url"),
                              api_key=dpg.get_value("emby_key"),
                              tmdb_key=dpg.get_value("tmdb_key"))
            push_shared_to_all_tabs()
        except Exception:
            pass
        try:
            with open(CONFIG_PATH, "w", encoding="utf-8") as fh:
                self.cfg.write(fh)
            self.log(self.t("log_saved"))
        except Exception as exc:
            self.log(self.t("log_save_err") % exc)

    def _build_clients(self):
        if self.lang == "EN":
            emby_lang, tmdb_lang = "en", "en-US"
        else:
            emby_lang, tmdb_lang = "fr", "fr-FR"
        self.emby = EmbyClient(dpg.get_value("emby_url"),
                               dpg.get_value("emby_key"), emby_lang)
        self.tmdb = TmdbClient(dpg.get_value("tmdb_key"), tmdb_lang)
        # type d'items : Films ou Séries (radio rm_kind)
        sel = "Films"
        if dpg.does_item_exist("rm_kind"):
            sel = dpg.get_value("rm_kind") or "Films"
        if str(sel).strip().lower() in ("séries","series","tv shows","séries/tv","series/tv"):
            self.emby_type, self.tmdb_kind = "Series", "tv"
        else:
            self.emby_type, self.tmdb_kind = "Movie", "movie"

    # ----- logging -----
    def log(self, msg):
        line = time.strftime("[%H:%M:%S] ") + str(msg)
        def _do():
            prev = dpg.get_value("log_box")
            dpg.set_value("log_box", (prev + "\n" + line) if prev else line)
        ui_post(_do)

    def _set_progress(self, frac, text=""):
        def _do():
            if dpg.does_item_exist("scan_progress"):
                dpg.set_value("scan_progress", max(0.0, min(1.0, frac)))
                dpg.configure_item("scan_progress", overlay=text)
        ui_post(_do)

    # ----- connexion + listing des médiathèques -----
    def _hint(self, msg, color=(150, 150, 150)):
        """Message visible a cote des mediatheques : le journal ne suffit pas,
        on ne le regarde pas quand on croit que le bouton ne marche pas."""
        def apply():
            if dpg.does_item_exist("lib_hint"):
                dpg.set_value("lib_hint", msg)
                dpg.configure_item("lib_hint", color=color)
        ui_post(apply)

    def on_connect(self, sender, app_data, user_data):
        try:
            self._build_clients()
        except Exception as exc:
            self._hint("Echec avant connexion : %s" % exc, (215, 75, 90))
            self.log("Echec _build_clients : %s" % exc)
            return
        url = (dpg.get_value("emby_url") or "").strip()
        key = (dpg.get_value("emby_key") or "").strip()
        if not url or not key:
            manque = " et ".join([x for x, v in (("l'URL", url),
                                                 ("la cle API", key)) if not v])
            self._hint("Connexion impossible : %s manque. Renseigne la "
                       "configuration commune en haut, puis Enregistrer."
                       % manque, (215, 75, 90))
            self.log("Connexion abandonnee : %s vide." % manque)
            return
        self._hint(self.t("connecting"), (235, 140, 20))
        def worker():
            ok = False
            try:
                info = self.emby.ping()
                ok = True
                self.log(self.t("log_emby_ok") % (info.get("ServerName", "?"),
                                                   info.get("Version", "?")))
            except Exception as exc:
                self.log(self.t("log_emby_fail") % exc)
                self._hint("Serveur injoignable ou cle refusee : %s"
                           % str(exc)[:90], (215, 75, 90))
            if ok:
                try:
                    self.libraries = self.emby.get_libraries()
                    self.log(self.t("log_libs_found") % len(self.libraries))
                    ui_post(self._render_libraries)
                    self._hint("Connecte  -  %d mediatheque(s)"
                               % len(self.libraries),
                               (46, 204, 113) if self.libraries
                               else (235, 140, 20))
                except Exception as exc:
                    self.log(self.t("log_libs_err") % exc)
                    self._hint("Mediatheques illisibles : %s" % str(exc)[:90],
                               (215, 75, 90))
            if self.tmdb.api_key:
                try:
                    self.tmdb.search("matrix", 1999)
                    self.log(self.t("log_tmdb_ok"))
                except Exception as exc:
                    self.log(self.t("log_tmdb_fail") % exc)
            else:
                self.log(self.t("log_tmdb_nokey"))
        threading.Thread(target=worker, daemon=True).start()

    # ══════════════════════════════════════════════════════════════
    #  DETECTION DES FICHES A PLUSIEURS FICHIERS RATTACHES
    # ══════════════════════════════════════════════════════════════
    # Emby regroupe volontiers deux fichiers sous une meme fiche, comme
    # deux "versions" du meme film. C'est legitime pour un 1080p et un 4K
    # du meme titre ; ca ne l'est pas du tout quand les deux fichiers sont
    # deux films differents (mauvais identifiant, .nfo errone). La fiche
    # affiche alors l'affiche de l'un et peut lire l'autre.

    _MERGE_TAGS = re.compile(
        r"\b(2160p|1080[pi]|720p|576p|480p|4k|uhd|bluray|blu[ .-]?ray|bdrip|"
        r"brrip|remux|webrip|web[ .-]?dl|web|hdtv|dvdrip|dvd|x ?26[45]|"
        r"h ?26[45]|hevc|avc|av1|xvid|divx|aac|ac3|eac3|dts[ .-]?hd|dts|"
        r"truehd|atmos|flac|ddp?5 ?1|10bits?|8bits?|hdr10|hdr|dovi|sdr|imax|"
        r"multi|vff|vfq|vfi|vf|vostfr|vost|french|truefrench|english|"
        r"repack|proper|extended|unrated|uncut|remastered|integrale|"
        r"final ?cut|directors? ?cut|amzn|netflix|dsnp|hulu|atvp|3d)\b", re.I)

    @staticmethod
    def _merge_key(fname):
        """Reduit un nom de fichier a son titre nu, pour comparer deux
        versions entre elles sans se laisser tromper par les tags de release."""
        x = Path(fname or "").stem
        x = unicodedata.normalize("NFKD", x)
        x = "".join(c for c in x if not unicodedata.combining(c)).lower()
        x = re.sub(r"[._\-\[\]\(\)\{\}:,;!?'\"`~+/\\|#@*]", " ", x)
        x = App._MERGE_TAGS.sub(" ", x)
        x = re.sub(r"\b(19|20)\d{2}\b", " ", x)      # l'annee ne discrimine pas
        x = re.sub(r"\s+", " ", x).strip()
        return x

    def _scan_merged(self, parent_ids, progress):
        """Parcourt la bibliotheque et renvoie les fiches a plusieurs
        fichiers, classees suspectes ou normales."""
        vus, resultats, total, faits = set(), [], 0, 0
        scopes = parent_ids or [None]
        for pid in scopes:
            for it, tot in self.emby.iter_items_sources(pid, self.emby_type):
                total = max(total, tot)
                faits += 1
                if faits % 50 == 0:
                    progress(faits, total)
                iid = it.get("Id")
                if not iid or iid in vus:
                    continue
                vus.add(iid)
                srcs = it.get("MediaSources") or []
                if len(srcs) < 2:
                    continue
                fichiers = []
                for sr in srcs:
                    pth = sr.get("Path") or ""
                    fichiers.append({
                        "name": Path(pth).name or (sr.get("Name") or "?"),
                        "path": pth,
                        "size": int(sr.get("Size", 0) or 0),
                        "key": self._merge_key(Path(pth).name)})
                cles = [f["key"] for f in fichiers if f["key"]]
                pire = 1.0
                for i in range(len(cles)):
                    for j in range(i + 1, len(cles)):
                        pire = min(pire, SequenceMatcher(None, cles[i],
                                                         cles[j]).ratio())
                if len(cles) < 2:
                    pire = 0.0          # noms inexploitables : a verifier
                resultats.append({
                    "id": iid,
                    "title": it.get("Name", "?"),
                    "year": it.get("ProductionYear", 0) or 0,
                    "files": fichiers,
                    "score": pire,
                    "suspect": pire < self.merge_threshold})
        progress(faits, total or faits)
        resultats.sort(key=lambda r: (not r["suspect"], r["score"]))
        return resultats

    def on_scan_merged(self, sender=None, app_data=None, user_data=None):
        try:
            self._build_clients()
        except Exception as exc:
            self._merge_hint("Echec avant scan : %s" % exc, (215, 75, 90))
            return
        if not (dpg.get_value("emby_url") or "").strip() or \
           not (dpg.get_value("emby_key") or "").strip():
            self._merge_hint("Connecte-toi d'abord : URL ou cle API manquante.",
                             (215, 75, 90))
            return
        try:
            self.merge_threshold = float(dpg.get_value("mrg_seuil") or 55) / 100.0
        except Exception:
            self.merge_threshold = 0.55
        # memes cases a cocher que le reste de l'onglet (lib_chk_N)
        pids = []
        for i, l in enumerate(self.libraries or []):
            tag = "lib_chk_%d" % i
            if dpg.does_item_exist(tag) and dpg.get_value(tag):
                pids.append(l["id"])
        pids = pids or None
        self._merge_hint("Analyse en cours...", (235, 140, 20))
        dpg.configure_item("mrg_btn", enabled=False)

        def worker():
            try:
                def progress(fait, total):
                    self._merge_hint("Analyse : %d / %d fiches"
                                     % (fait, total or fait), (235, 140, 20))
                res = self._scan_merged(pids, progress)
                self.merged = res
                ui_post(self._render_merged)
            except Exception as exc:
                self.log("Scan versions multiples : %s" % exc)
                self._merge_hint("Echec : %s" % str(exc)[:90], (215, 75, 90))
            finally:
                ui_post(lambda: dpg.configure_item("mrg_btn", enabled=True))

        threading.Thread(target=worker, daemon=True).start()

    def _merge_hint(self, msg, color=(150, 150, 150)):
        def apply():
            if dpg.does_item_exist("mrg_hint"):
                dpg.set_value("mrg_hint", msg)
                dpg.configure_item("mrg_hint", color=color)
        ui_post(apply)

    def _render_merged(self):
        dpg.delete_item("mrg_group", children_only=True)
        res = getattr(self, "merged", []) or []
        susp = [r for r in res if r["suspect"]]
        if not res:
            self._merge_hint("Aucune fiche ne regroupe plusieurs fichiers.",
                             (46, 204, 113))
            dpg.add_text("Rien a signaler.", parent="mrg_group",
                         color=(150, 150, 175))
            return
        self._merge_hint("%d fiche(s) a plusieurs fichiers  -  %d suspecte(s)"
                         % (len(res), len(susp)),
                         (215, 75, 90) if susp else (46, 204, 113))
        for r in res:
            col = (215, 75, 90) if r["suspect"] else (46, 204, 113)
            with dpg.group(parent="mrg_group"):
                dpg.add_text("%s  %s (%s)  -  %d fichiers  -  similarite %d%%"
                             % ("SUSPECT" if r["suspect"] else "normal  ",
                                r["title"][:60], r["year"] or "?",
                                len(r["files"]), round(r["score"] * 100)),
                             color=col)
                for f in r["files"]:
                    dpg.add_text("      %-72s %10s"
                                 % (f["name"][:72], fmt_size(f["size"])),
                                 color=(200, 200, 215))
                    with dpg.tooltip(dpg.last_item()):
                        dpg.add_text(f["path"], wrap=620)
                with dpg.group(horizontal=True):
                    dpg.add_text("      ")
                    dpg.add_button(label="Copier l'ID", width=110,
                                   user_data=r["id"],
                                   callback=lambda s, a, u: dpg.set_clipboard_text(u))
                    dpg.add_button(label="Ouvrir dans Emby", width=150,
                                   user_data=r["id"],
                                   callback=lambda s, a, u: webbrowser.open(
                                       "%s/web/index.html#!/item?id=%s"
                                       % ((dpg.get_value("emby_url") or "").rstrip("/"), u)))
                dpg.add_separator()

    def on_export_merged(self, sender=None, app_data=None, user_data=None):
        res = getattr(self, "merged", []) or []
        if not res:
            self._merge_hint("Rien a exporter : lance d'abord l'analyse.",
                             (235, 140, 20))
            return
        fp = Path(__file__).with_name("emby_versions_multiples.csv")
        try:
            with open(fp, "w", newline="", encoding="utf-8-sig") as fh:
                w = csv.writer(fh, delimiter=";")
                w.writerow(["Verdict", "Titre", "Annee", "ItemId",
                            "Nb fichiers", "Similarite %", "Fichier",
                            "Taille", "Chemin"])
                for r in res:
                    for f in r["files"]:
                        w.writerow(["SUSPECT" if r["suspect"] else "normal",
                                    r["title"], r["year"], r["id"],
                                    len(r["files"]), round(r["score"] * 100),
                                    f["name"], f["size"], f["path"]])
            self._merge_hint("Exporte : %s" % fp, (46, 204, 113))
        except Exception as exc:
            self._merge_hint("Export impossible : %s" % exc, (215, 75, 90))

    def _render_libraries(self):
        dpg.delete_item("lib_group", children_only=True)
        if not self.libraries:
            dpg.set_value("lib_hint", self.t("hint_no_libs"))
            dpg.add_text(self.t("libs_nothing"), parent="lib_group", color=(150, 150, 150))
            return
        saved = [s for s in self.get("options", "selected_libs", "").split("|") if s]
        for i, lib in enumerate(self.libraries):
            pre = (lib["name"] in saved)   # seules les médiathèques déjà choisies sont cochées
            dpg.add_checkbox(label="%s   (%s)" % (lib["name"], lib["type"]),
                             tag="lib_chk_%d" % i, default_value=pre,
                             parent="lib_group")
        dpg.set_value("lib_hint", self.t("hint_count") % len(self.libraries))

    def on_libs_all(self, sender, app_data, user_data):
        for i in range(len(self.libraries)):
            tag = "lib_chk_%d" % i
            if dpg.does_item_exist(tag):
                dpg.set_value(tag, True)

    def on_libs_none(self, sender, app_data, user_data):
        for i in range(len(self.libraries)):
            tag = "lib_chk_%d" % i
            if dpg.does_item_exist(tag):
                dpg.set_value(tag, False)

    def on_save(self, sender, app_data, user_data):
        self.save_config()

    # ----- scan -----
    def _classify(self, item, no_id, no_art, scan_all):
        pid = {k.lower(): v for k, v in (item.get("ProviderIds") or {}).items()}
        has_id = bool(pid.get("tmdb") or pid.get("imdb"))
        has_poster = bool((item.get("ImageTags") or {}).get("Primary"))
        has_overview = bool((item.get("Overview") or "").strip())
        reasons = []
        if no_id and not has_id:
            reasons.append("sans ID")
        if no_art and (not has_poster or not has_overview):
            miss = []
            if not has_poster:
                miss.append("poster")
            if not has_overview:
                miss.append("résumé")
            reasons.append("sans " + "/".join(miss))
        if scan_all:
            reasons.append("audit")
        return reasons

    def on_scan(self, sender, app_data, user_data):
        if self._scanning:
            return
        no_id = dpg.get_value("crit_noid")
        no_art = dpg.get_value("crit_noart")
        scan_all = dpg.get_value("crit_all")
        if not (no_id or no_art or scan_all):
            self.log(self.t("log_need_crit"))
            return

        # médiathèques sélectionnées
        targets = []
        if self.libraries:
            for i, lib in enumerate(self.libraries):
                tag = "lib_chk_%d" % i
                if dpg.does_item_exist(tag) and dpg.get_value(tag):
                    targets.append(lib)
            if not targets:
                self.log(self.t("log_need_lib"))
                dpg.set_value("status_txt", self.t("log_need_lib"))
                return
        else:
            targets = [{"name": "(toutes)" if self.lang == "FR" else "(all)", "id": None}]

        self._build_clients()
        self._scanning = True
        dpg.set_value("status_txt", self.t("status_scan"))
        self._set_progress(0.0, self.t("prog_counting"))
        self.log(self.t("log_scan_targets") % ", ".join(t["name"] for t in targets))

        def worker():
            flagged, total = [], 0
            # pré-comptage pour une progression précise
            grand_total = 0
            try:
                for lib in targets:
                    grand_total += self.emby.count_items(parent_id=lib["id"],
                                                          item_type=self.emby_type)
            except Exception:
                grand_total = 0
            try:
                for lib in targets:
                    if lib["id"]:
                        self.log(self.t("log_scan_lib") % lib["name"])
                    for it in self.emby.iter_items(parent_id=lib["id"],
                                                   item_type=self.emby_type):
                        total += 1
                        reasons = self._classify(it, no_id, no_art, scan_all)
                        if reasons:
                            it["_reason"] = ", ".join(reasons)
                            it["_library"] = lib["name"]
                            flagged.append(it)
                        if total % 25 == 0 or total == grand_total:
                            frac = (total / grand_total) if grand_total else 0.0
                            self._set_progress(frac, self.t("prog_scan") % (total, grand_total or total))
                        if total % 200 == 0:
                            self.log(self.t("log_scan_progress") % (total, len(flagged)))
            except Exception as exc:
                self.log(self.t("log_scan_err") % exc)
            self.flagged = flagged
            self._set_progress(1.0, self.t("prog_done"))
            self.log(self.t("log_scan_done") % (total, len(flagged)))
            ui_post(lambda: dpg.set_value("status_txt",
                    self.t("status_to_fix") % (len(flagged), total)))
            self._scan_total = total
            ui_post(self._refresh_flagged_table)
            self._scanning = False

        threading.Thread(target=worker, daemon=True).start()

    def _refresh_flagged_table(self):
        if not dpg.does_item_exist("flagged_table"):
            return
        # ne supprimer QUE les lignes (slot 1) : recréer les colonnes d'une
        # table déjà rendue laisse parfois ses lignes invisibles (quirk DPG)
        for r in dpg.get_item_children("flagged_table", 1) or []:
            try:
                dpg.delete_item(r)
            except Exception:
                pass
        labels = (self.t("col_title"), self.t("col_year"), self.t("col_problem"))
        cols = dpg.get_item_children("flagged_table", 0) or []
        if not cols:
            dpg.add_table_column(label=labels[0], parent="flagged_table", width_stretch=True)
            dpg.add_table_column(label=labels[1], parent="flagged_table", width_fixed=True, init_width_or_weight=60)
            dpg.add_table_column(label=labels[2], parent="flagged_table", width_fixed=True, init_width_or_weight=160)
        else:
            for c, lbl in zip(cols, labels):
                try:
                    dpg.configure_item(c, label=lbl)
                except Exception:
                    pass
        for idx, it in enumerate(self.flagged):
            with dpg.table_row(parent="flagged_table"):
                dpg.add_selectable(label=it.get("Name", "?"), span_columns=True,
                                   callback=self.on_pick_movie, user_data=idx)
                dpg.add_text(str(it.get("ProductionYear") or "-"))
                dpg.add_text(it.get("_reason", ""))

    # ----- auto-correction (TMDB score 1.0) -----
    # ----- tolerance d'annee -----
    def _read_tol(self):
        try:
            return max(0, int(dpg.get_value("search_tol")))
        except Exception:
            return YEAR_TOLERANCE

    def _filter_year(self, results, year, tol):
        """Garde les candidats dont l'annee est dans [year-tol, year+tol].
        Si aucune annee devinee, ne filtre pas. Si la fenetre est vide, garde tout."""
        if not year:
            return results
        win = []
        for r in results:
            ty = tmdb_year(r)
            if ty is not None and abs(ty - year) <= tol:
                win.append(r)
        return win if win else results

    def _tmdb_best(self, title, year, tol=YEAR_TOLERANCE):
        """Meilleur candidat TMDB {r, year, score, title_sim} ou None.
        Recherche LARGE (sans contrainte d'annee cote TMDB) puis filtrage
        local a +/- tol annees : indispensable quand l'annee Emby est fausse."""
        results = self._filter_year(self.tmdb.search(title, None, self.tmdb_kind),
                                    year, tol)
        best = None
        for r in results[:12]:
            cyear = tmdb_year(r)
            sc, sim = score_candidate(title, year, r.get("title"),
                                      r.get("original_title"), cyear,
                                      r.get("popularity"))
            if best is None or sc > best["score"]:
                best = {"r": r, "year": cyear, "score": sc, "title_sim": sim}
        return best

    def on_auto(self, sender, app_data, user_data):
        if self._scanning:
            return
        if not self.flagged:
            self.log(self.t("log_nothing"))
            return
        self._build_clients()
        if not self.tmdb.api_key:
            self.log(self.t("log_auto_needkey"))
            return
        items = list(self.flagged)
        tol = self._read_tol()
        # confirmation avant écriture en masse sur Emby
        tagc = "auto_confirm_win"
        if dpg.does_item_exist(tagc):
            dpg.delete_item(tagc)
        def _start(sender=None, app_data=None, user_data=None):
            if dpg.does_item_exist(tagc):
                dpg.delete_item(tagc)
            self._start_auto(items, tol)
        with dpg.window(label=self.t("auto_confirm_title"), tag=tagc, modal=True,
                        width=540, autosize=True, pos=[220, 220], no_resize=True):
            dpg.add_text(self.t("auto_confirm_msg") % len(items), wrap=505)
            dpg.add_spacer(height=4)
            with dpg.group(horizontal=True):
                dpg.add_button(label=self.t("btn_yes"), width=150, callback=_start)
                dpg.add_button(label=self.t("btn_no"), width=110,
                               callback=lambda s, a, u: dpg.delete_item(tagc))
        return

    def _start_auto(self, items, tol):
        self._scanning = True
        dpg.set_value("status_txt", self.t("status_auto"))
        self._set_progress(0.0, self.t("prog_auto") % (0, len(items)))
        self.log(self.t("log_auto_start") % len(items))

        def worker():
            applied, done_ids = 0, set()
            total = len(items)
            for idx, it in enumerate(items, 1):
                title, year = guess_title_year(it.get("Name", ""), it.get("Path", ""))
                try:
                    best = self._tmdb_best(title, year, tol)
                except Exception as exc:
                    self.log(self.t("log_tmdb_err_name") % (title, exc))
                    self._set_progress(idx / total, self.t("prog_auto") % (idx, total))
                    continue
                # l'annee du candidat est deja dans la fenetre +/- tol (via _filter_year) ;
                # on applique des que le titre est quasi-exact.
                if not best or best.get("title_sim", 0.0) < AUTO_TITLE_MIN:
                    self._set_progress(idx / total, self.t("prog_auto") % (idx, total))
                    continue
                # garde-fou : en auto, on borne strictement l'annee a +/- tol
                by = best.get("year")
                if year and by is not None and abs(by - year) > tol:
                    self._set_progress(idx / total, self.t("prog_auto") % (idx, total))
                    continue
                r, cyear = best["r"], best["year"]
                pids = {"Tmdb": str(r.get("id"))}
                try:
                    ext = self.tmdb.external_ids(r.get("id"), self.tmdb_kind)
                    if ext.get("imdb_id"):
                        pids["Imdb"] = ext["imdb_id"]
                except Exception:
                    pass
                result = {
                    "Name": r.get("title") or r.get("original_title"),
                    "ProductionYear": cyear,
                    "ProviderIds": pids,
                    "SearchProviderName": "TheMovieDb",
                    "ImageUrl": (TMDB_IMG + r["poster_path"]) if r.get("poster_path") else None,
                }
                try:
                    # replace_images=True : remplace résumé ET images
                    self.emby.apply_remote_result(it.get("Id"), result, replace_images=True)
                    applied += 1
                    done_ids.add(it.get("Id"))
                    self.log(self.t("log_auto_ok") % (result["Name"], cyear or "-"))
                except Exception as exc:
                    self.log(self.t("log_auto_fail") % (title, exc))
                self._set_progress(idx / total, self.t("prog_auto") % (idx, total))
                time.sleep(0.2)  # throttle TMDB/Emby

            self.flagged = [f for f in self.flagged if f.get("Id") not in done_ids]
            self._set_progress(1.0, self.t("prog_done"))
            self.log(self.t("log_auto_done") % (applied, len(self.flagged)))
            ui_post(lambda: dpg.set_value("status_txt",
                    self.t("status_remaining") % len(self.flagged)))
            ui_post(self._refresh_flagged_table)
            self._scanning = False

        threading.Thread(target=worker, daemon=True).start()

    # ----- sélection d'un film -----
    def on_pick_movie(self, sender, app_data, user_data):
        idx = user_data
        if idx is None or idx >= len(self.flagged):
            return
        self.current = self.flagged[idx]
        it = self.current
        title, year = guess_title_year(it.get("Name", ""), it.get("Path", ""))
        pid = {k.lower(): v for k, v in (it.get("ProviderIds") or {}).items()}
        info = self.t("detail_tpl") % (
            it.get("Name", "?"), it.get("ProductionYear") or "-",
            it.get("Path", "-"),
            title, year or "-",
            pid.get("tmdb", "-"), pid.get("imdb", "-"),
            it.get("_reason", ""),
        )
        dpg.set_value("detail_txt", info)
        dpg.set_value("search_title", title)
        dpg.set_value("search_year", str(year) if year else "")
        self._clear_candidates()

    # ----- recherche de candidats -----
    def on_search(self, sender, app_data, user_data):
        if not self.current:
            self.log(self.t("log_pick_first"))
            return
        title = dpg.get_value("search_title").strip()
        year_s = dpg.get_value("search_year").strip()
        year = int(year_s) if year_s.isdigit() else None
        if not title:
            self.log(self.t("log_search_empty"))
            return
        self._gen += 1
        gen = self._gen
        item_id = self.current.get("Id")
        self._build_clients()
        tol = self._read_tol()
        self.log(self.t("log_search_start") % (title, year or "-"))
        self._clear_candidates()

        def worker():
            cands = []
            # 1) TMDB prioritaire : recherche large puis filtrage +/- tol annees
            try:
                results = (self._filter_year(
                            self.tmdb.search(title, None, self.tmdb_kind), year, tol)
                           if self.tmdb.api_key else [])
                for r in results[:12]:
                    cyear = tmdb_year(r)
                    sc, sim = score_candidate(title, year, r.get("title"),
                                              r.get("original_title"), cyear,
                                              r.get("popularity"))
                    cands.append({
                        "source": "TMDB",
                        "tmdb_id": r.get("id"),
                        "imdb_id": None,
                        "title": r.get("title") or r.get("original_title") or "?",
                        "orig": r.get("original_title") or "",
                        "year": cyear,
                        "overview": (r.get("overview") or "").strip(),
                        "poster": (TMDB_IMG + r["poster_path"]) if r.get("poster_path") else None,
                        "score": sc,
                    })
            except Exception as exc:
                self.log(self.t("log_tmdb_err") % exc)

            # 2) Emby RemoteSearch en secours (si TMDB n'a rien donné de solide)
            best = max((c["score"] for c in cands), default=0.0)
            if best < 0.5:
                try:
                    rs = self.emby.remote_search(item_id, title, year, self.tmdb_kind)
                    for r in rs[:12]:
                        rpid = {k.lower(): v for k, v in (r.get("ProviderIds") or {}).items()}
                        cyear = r.get("ProductionYear")
                        sc, sim = score_candidate(title, year, r.get("Name"),
                                                  None, cyear, 0)
                        cands.append({
                            "source": "Emby",
                            "tmdb_id": rpid.get("tmdb"),
                            "imdb_id": rpid.get("imdb"),
                            "title": r.get("Name") or "?",
                            "orig": "",
                            "year": cyear,
                            "overview": (r.get("Overview") or "").strip(),
                            "poster": r.get("ImageUrl"),
                            "score": sc,
                            "_raw": r,   # RemoteSearchResult complet pour Apply
                        })
                except Exception as exc:
                    self.log(self.t("log_emby_rs_err") % exc)

            cands.sort(key=lambda c: c["score"], reverse=True)
            if gen != self._gen:
                return  # une recherche plus récente a démarré
            self.candidates = cands
            self.log(self.t("log_cand_count") % len(cands))
            ui_post(lambda: self._render_candidates(gen))

        threading.Thread(target=worker, daemon=True).start()

    def _clear_candidates(self):
        self.candidates = []
        if dpg.does_item_exist("cand_group"):
            dpg.delete_item("cand_group", children_only=True)
        for t in self._tex_tags:
            if dpg.does_item_exist(t):
                dpg.delete_item(t)
        self._tex_tags = []

    def _render_candidates(self, gen):
        if gen != self._gen:
            return
        dpg.delete_item("cand_group", children_only=True)
        if not self.candidates:
            dpg.add_text(self.t("cand_none"),
                         parent="cand_group", color=(200, 160, 80))
            return
        for i, c in enumerate(self.candidates):
            best = (i == 0)
            col = (120, 220, 140) if best else (210, 210, 210)
            with dpg.child_window(parent="cand_group", height=140, border=True):
                with dpg.group(horizontal=True):
                    img_tag = "cand_img_%d_%d" % (gen, i)
                    dpg.add_text("", tag=img_tag)  # placeholder remplacé par le poster
                    with dpg.group():
                        # Le bouton est EN TETE de carte, sur la ligne du titre :
                        # place en dernier sous un resume de plusieurs lignes,
                        # il tombait sous la zone visible et obligeait a
                        # derouler l'ascenseur pour chaque candidat.
                        with dpg.group(horizontal=True):
                            apply_btn = dpg.add_button(
                                label=self.t("btn_apply"), width=155,
                                callback=self.on_apply, user_data=i)
                            dpg.bind_item_theme(apply_btn, "th_btn_ok")
                            head = "%s (%s)  ·  %s  ·  score %.2f%s" % (
                                c["title"], c["year"] or "-", c["source"],
                                c["score"], self.t("cand_best") if best else "")
                            dpg.add_text(head, color=col)
                        ids = "TMDB=%s  IMDB=%s" % (c.get("tmdb_id") or "-",
                                                    c.get("imdb_id") or "-")
                        dpg.add_text(ids, color=(150, 150, 150))
                        ov = c["overview"]
                        if len(ov) > 200:
                            ov = ov[:197] + "..."
                        dpg.add_text(ov or self.t("cand_no_overview"), wrap=520,
                                     color=(180, 180, 180))
            if PIL_OK and c.get("poster"):
                self._load_poster_async(c["poster"], img_tag, gen)

    def _load_poster_async(self, url, anchor_tag, gen):
        def worker():
            try:
                r = requests.get(url, timeout=HTTP_TIMEOUT)
                r.raise_for_status()
                img = Image.open(BytesIO(r.content)).convert("RGBA")
                img.thumbnail((80, 120))
                w, h = img.size
                data = [b / 255.0 for b in img.tobytes()]
            except Exception:
                return
            def _do():
                if gen != self._gen or not dpg.does_item_exist(anchor_tag):
                    return
                tex_tag = "tex_%s" % anchor_tag
                if dpg.does_item_exist(tex_tag):
                    return
                with dpg.texture_registry():
                    dpg.add_static_texture(w, h, data, tag=tex_tag)
                self._tex_tags.append(tex_tag)
                parent = dpg.get_item_parent(anchor_tag)
                dpg.add_image(tex_tag, parent=parent, before=anchor_tag,
                              width=w, height=h)
                dpg.delete_item(anchor_tag)
            ui_post(_do)
        threading.Thread(target=worker, daemon=True).start()

    # ----- application -----
    def on_apply(self, sender, app_data, user_data):
        i = user_data
        if i is None or i >= len(self.candidates) or not self.current:
            return
        c = self.candidates[i]
        item = self.current
        item_id = item.get("Id")
        replace = dpg.get_value("opt_replace_img")
        self._build_clients()
        self.log(self.t("log_apply_start") % (
            item.get("Name", "?"), c["title"], c["year"] or "-", c["source"]))

        def worker():
            try:
                if c["source"] == "Emby" and c.get("_raw"):
                    result = c["_raw"]
                else:
                    # candidat TMDB : on construit un RemoteSearchResult minimal.
                    pids = {}
                    if c.get("tmdb_id"):
                        pids["Tmdb"] = str(c["tmdb_id"])
                    # récupère l'IMDb id pour enrichir le match
                    if c.get("tmdb_id"):
                        ext = self.tmdb.external_ids(c["tmdb_id"], self.tmdb_kind)
                        if ext.get("imdb_id"):
                            pids["Imdb"] = ext["imdb_id"]
                    result = {
                        "Name": c["title"],
                        "ProductionYear": c["year"],
                        "ProviderIds": pids,
                        "SearchProviderName": "TheMovieDb",
                        "ImageUrl": c.get("poster"),
                    }
                    if not pids:
                        self.log(self.t("log_apply_noid"))
                        return
                self.emby.apply_remote_result(item_id, result, replace_images=replace)
                self.log(self.t("log_apply_ok") % item.get("Name", "?"))
                ui_post(lambda: self._mark_done(item_id))
            except Exception as exc:
                self.log(self.t("log_apply_fail") % exc)

        threading.Thread(target=worker, daemon=True).start()

    def _mark_done(self, item_id):
        # retire le film traité de la liste et rafraîchit la table
        self.flagged = [f for f in self.flagged if f.get("Id") != item_id]
        self.current = None
        self._clear_candidates()
        dpg.set_value("detail_txt", self.t("detail_done"))
        dpg.set_value("status_txt", self.t("status_remaining") % len(self.flagged))
        self._refresh_flagged_table()

    # ----- construction UI -----
    def _build_themes(self):
        # thème global appliqué à tous les boutons
        with dpg.theme(tag="th_btn"):
            with dpg.theme_component(dpg.mvButton):
                dpg.add_theme_color(dpg.mvThemeCol_Button, BTN_COLOR)
                dpg.add_theme_color(dpg.mvThemeCol_ButtonHovered, BTN_COLOR_HOVER)
                dpg.add_theme_color(dpg.mvThemeCol_ButtonActive, BTN_COLOR_ACTIVE)
                dpg.add_theme_color(dpg.mvThemeCol_Text, BTN_TEXT_COLOR)
                dpg.add_theme_style(dpg.mvStyleVar_FrameRounding, 4)
                dpg.add_theme_style(dpg.mvStyleVar_FramePadding, 8, 4)
        # thème vert pour le bouton « Appliquer » (action de validation)
        with dpg.theme(tag="th_btn_ok"):
            with dpg.theme_component(dpg.mvButton):
                dpg.add_theme_color(dpg.mvThemeCol_Button, BTN_OK_COLOR)
                dpg.add_theme_color(dpg.mvThemeCol_ButtonHovered, BTN_OK_COLOR_HOVER)
                dpg.add_theme_color(dpg.mvThemeCol_ButtonActive, BTN_OK_COLOR_ACTIVE)
                dpg.add_theme_color(dpg.mvThemeCol_Text, BTN_TEXT_COLOR)
                dpg.add_theme_style(dpg.mvStyleVar_FrameRounding, 4)
                dpg.add_theme_style(dpg.mvStyleVar_FramePadding, 8, 4)

    def _build_font(self):
        """Charge une police système couvrant le Latin-1 (accents FR, « »)."""
        if getattr(self, "_font_loaded", False):
            return
        candidates = [
            r"C:\Windows\Fonts\segoeui.ttf",
            r"C:\Windows\Fonts\arial.ttf",
            r"C:\Windows\Fonts\tahoma.ttf",
            r"C:\Windows\Fonts\verdana.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/Library/Fonts/Arial.ttf",
        ]
        path = next((p for p in candidates if os.path.exists(p)), None)
        if not path:
            self._font_loaded = False
            return  # police par défaut DPG (accents possiblement absents)
        try:
            with dpg.font_registry():
                with dpg.font(path, FONT_SIZE) as f:
                    dpg.add_font_range_hint(dpg.mvFontRangeHint_Default)
                    # chargement EXPLICITE des plages utilisées dans l'UI.
                    # Sans cela (sur les versions de DPG où ces plages ne sont pas
                    # automatiques), les glyphes hors Latin s'affichent en « ? ».
                    dpg.add_font_range(0x0020, 0x017F)   # Latin de base + Latin-1 + Extended-A
                    dpg.add_font_range(0x2000, 0x206F)   # ponctuation : tiret cadratin, …, guillemets, •
                    dpg.add_font_range(0x2100, 0x214F)   # symboles type lettre : ℹ, №, ™
                    dpg.add_font_range(0x2190, 0x21FF)   # flèches : →, ←, ↔
                    dpg.add_font_range(0x2600, 0x27BF)   # symboles divers + dingbats : ✓, ✗, ⚠, ★
            dpg.bind_font(f)
            self._font_loaded = True
            self._font_path = path
        except Exception:
            self._font_loaded = False

    def _reg(self, tag, kind, key, prefix=""):
        """Enregistre un widget traduisible et renvoie son tag."""
        self._i18n.append((tag, kind, key, prefix))
        return tag

    def apply_language(self):
        # statut re-émis dans la nouvelle langue
        if self.flagged and getattr(self, "_scan_total", 0):
            try:
                dpg.set_value("status_txt",
                              self.t("status_to_fix") % (len(self.flagged),
                                                         self._scan_total))
            except Exception:
                pass
        # hints/placeholders : remis dans la langue courante s'ils affichent
        # encore le placeholder de l'autre langue
        for tag, key in (("lib_hint", "hint_connect"),
                         ("detail_txt", "detail_placeholder")):
            if dpg.does_item_exist(tag):
                cur = str(dpg.get_value(tag) or "")
                variants = {TR[l].get(key, "") for l in TR}
                if cur in variants:
                    dpg.set_value(tag, self.t(key))
        # radio Films/Séries : libellés traduits, sélection préservée par index
        if dpg.does_item_exist("rm_kind"):
            _kl = ("Movies", "TV Shows") if self.lang == "EN" else ("Films", "Séries")
            _cur = (dpg.get_value("rm_kind") or "").strip().lower()
            _ix = 1 if _cur in ("séries","series","tv shows","séries/tv","series/tv") else 0
            dpg.configure_item("rm_kind", items=list(_kl))
            dpg.set_value("rm_kind", _kl[_ix])
        for tag, kind, key, prefix in self._i18n:
            if not dpg.does_item_exist(tag):
                continue
            text = prefix + self.t(key)
            if kind == "label":
                dpg.configure_item(tag, label=text)
            else:
                dpg.set_value(tag, text)
        # zones dynamiques régénérées dans la nouvelle langue
        self._refresh_flagged_table()
        if self.libraries:
            # préserver l'état ACTUEL des cases (la reconstruction repartirait
            # de la sélection sauvegardée et décocherait silencieusement)
            checked = {i: bool(dpg.get_value("lib_chk_%d" % i))
                       for i in range(len(self.libraries))
                       if dpg.does_item_exist("lib_chk_%d" % i)}
            self._render_libraries()
            for i, v in checked.items():
                if dpg.does_item_exist("lib_chk_%d" % i):
                    dpg.set_value("lib_chk_%d" % i, v)
        if self.candidates:
            self._render_candidates(self._gen)

    def on_lang_change(self, sender, app_data, user_data):
        self.lang = (app_data or "FR").upper()
        self.apply_language()

    def build_ui(self):
        self._build_themes()
        self._build_font()
        self._i18n = []
        # ── Bandeau de titre IDFinder ──
        with dpg.group(horizontal=True):
            dpg.add_text("IDFinder", color=(233, 69, 96))
            dpg.add_text(self.t("subtitle"),
                         tag=self._reg("t_subtitle", "value", "subtitle"),
                         color=(136, 136, 170))
        dpg.add_separator()
        with dpg.collapsing_header(tag=self._reg("h_conn", "label", "hdr_conn"),
                                   label=self.t("hdr_conn"), default_open=True,
                                   show=False):  # config commune en haut
            with dpg.group(horizontal=True):
                dpg.add_text(self.t("lbl_emby_url"),
                             tag=self._reg("t_emby_url", "value", "lbl_emby_url"))
                dpg.add_input_text(tag="emby_url", width=340,
                                   default_value=self.get("emby", "url", "http://192.168.1.10:8096"))
                dpg.add_text(self.t("lbl_emby_key"),
                             tag=self._reg("t_emby_key", "value", "lbl_emby_key"))
                dpg.add_input_text(tag="emby_key", width=320, password=True,
                                   default_value=self.get_secret("emby", "api_key"))
            with dpg.group(horizontal=True):
                dpg.add_text(self.t("lbl_tmdb_key"),
                             tag=self._reg("t_tmdb_key", "value", "lbl_tmdb_key"))
                dpg.add_input_text(tag="tmdb_key", width=340, password=True,
                                   default_value=self.get_secret("tmdb", "api_key"))
                dpg.add_text(self.t("lbl_lang"),
                             tag=self._reg("t_lang", "value", "lbl_lang"))
                dpg.add_radio_button(("FR", "EN"), tag="ui_lang", horizontal=True,
                                     default_value=self.lang,
                                     callback=self.on_lang_change)
                dpg.add_button(tag=self._reg("b_connect", "label", "btn_connect"),
                               label=self.t("btn_connect"), callback=self.on_connect)
                dpg.add_button(tag=self._reg("b_save", "label", "btn_save"),
                               label=self.t("btn_save"), callback=self.on_save)

        with dpg.collapsing_header(tag=self._reg("h_libs", "label", "hdr_libs"),
                                   label=self.t("hdr_libs"), default_open=True):
            with dpg.group(horizontal=True):
                # Bouton de connexion propre a l'onglet : le bouton commun en
                # haut appelle les trois modules dans un try/except muet, donc
                # un echec de l'IDFinder y passe inapercu.
                bc = dpg.add_button(tag=self._reg("b_connect_tab", "label",
                                                  "btn_connect"),
                                    label=self.t("btn_connect"),
                                    width=120, callback=self.on_connect)
                try:
                    dpg.bind_item_theme(bc, "th_btn_ok")
                except Exception:
                    pass
                dpg.add_button(tag=self._reg("b_all", "label", "btn_all"),
                               label=self.t("btn_all"), callback=self.on_libs_all)
                dpg.add_button(tag=self._reg("b_none", "label", "btn_none"),
                               label=self.t("btn_none"), callback=self.on_libs_none)
                dpg.add_text(self.t("hint_connect"), tag="lib_hint",
                             color=(150, 150, 150))
            with dpg.child_window(height=150, width=-1):
                dpg.add_group(tag="lib_group")

        with dpg.collapsing_header(label="Fiches a plusieurs fichiers "
                                   "(fusions de versions)", default_open=False):
            dpg.add_text("Emby regroupe parfois deux fichiers sous une meme "
                         "fiche. Legitime pour un 1080p et un 4K du meme film, "
                         "aberrant quand ce sont deux films differents : la "
                         "fiche affiche alors l'affiche de l'un et peut lire "
                         "l'autre.", color=(150, 150, 175), wrap=1000)
            with dpg.group(horizontal=True):
                b = dpg.add_button(label="Analyser les fusions", tag="mrg_btn",
                                   width=190, callback=self.on_scan_merged)
                try:
                    dpg.bind_item_theme(b, "th_btn_scan")
                except Exception:
                    pass
                dpg.add_text("Seuil de suspicion")
                dpg.add_slider_int(tag="mrg_seuil", width=150, min_value=10,
                                   max_value=95, default_value=55,
                                   format="%d%%")
                with dpg.tooltip(dpg.last_item()):
                    dpg.add_text("Deux fichiers d'une meme fiche sont compares "
                                 "apres retrait des tags de release. En dessous "
                                 "de ce taux de ressemblance, la fusion est "
                                 "signalee comme suspecte.\n55 % convient dans "
                                 "la plupart des cas.", wrap=360)
                dpg.add_button(label="Exporter en CSV", width=150,
                               callback=self.on_export_merged)
                dpg.add_text("", tag="mrg_hint", color=(150, 150, 150))
            with dpg.child_window(height=260, width=-1):
                dpg.add_group(tag="mrg_group")

        with dpg.collapsing_header(tag=self._reg("h_detect", "label", "hdr_detect"),
                                   label=self.t("hdr_detect"), default_open=True):
            with dpg.group(horizontal=True):
                dpg.add_text(self.t("lbl_kind"),
                             tag=self._reg("t_kind", "value", "lbl_kind"))
                _kraw = (self.get("options", "kind", "Films") or "").strip().lower()
                _kser = _kraw in ("séries","series","tv shows","séries/tv","series/tv")
                _klabels = ("Movies", "TV Shows") if self.lang == "EN" else ("Films", "Séries")
                dpg.add_radio_button(_klabels, tag="rm_kind",
                                     horizontal=True,
                                     default_value=_klabels[1 if _kser else 0])
                dpg.add_spacer(width=12)
            with dpg.group(horizontal=True):
                dpg.add_checkbox(tag=self._reg("crit_noid", "label", "chk_noid"),
                                 label=self.t("chk_noid"),
                                 default_value=self.get("options", "no_id", "True") == "True")
                dpg.add_checkbox(tag=self._reg("crit_noart", "label", "chk_noart"),
                                 label=self.t("chk_noart"),
                                 default_value=self.get("options", "no_art", "False") == "True")
                dpg.add_checkbox(tag=self._reg("crit_all", "label", "chk_all"),
                                 label=self.t("chk_all"),
                                 default_value=self.get("options", "all", "False") == "True")
                dpg.add_button(tag=self._reg("b_scan", "label", "btn_scan"),
                               label=self.t("btn_scan"), callback=self.on_scan)
                auto_btn = dpg.add_button(tag=self._reg("b_auto", "label", "btn_auto", ""),
                                          label="" + self.t("btn_auto"),
                                          callback=self.on_auto)
                dpg.bind_item_theme(auto_btn, "th_btn_ok")
                dpg.add_text("", tag="status_txt", color=(150, 200, 255))
            dpg.add_progress_bar(tag="scan_progress", default_value=0.0,
                                 width=-1, overlay="")

        with dpg.group(horizontal=True):
            # colonne gauche : films détectés
            with dpg.child_window(width=440, height=-160):
                dpg.add_text(self.t("lbl_to_fix"),
                             tag=self._reg("t_to_fix", "value", "lbl_to_fix"))
                with dpg.table(tag="flagged_table", header_row=True,
                               borders_innerH=True, borders_outerH=True,
                               borders_innerV=True, borders_outerV=True,
                               policy=dpg.mvTable_SizingStretchProp,
                               scrollY=True, height=-1):
                    dpg.add_table_column(label=self.t("col_title"))
                    dpg.add_table_column(label=self.t("col_year"))
                    dpg.add_table_column(label=self.t("col_problem"))

            # colonne droite : détails + recherche + candidats
            with dpg.child_window(width=-1, height=-160):
                dpg.add_text(self.t("lbl_detail"),
                             tag=self._reg("t_detail", "value", "lbl_detail"))
                dpg.add_text(self.t("detail_placeholder"), tag="detail_txt",
                             wrap=560, color=(200, 200, 200))
                dpg.add_separator()
                with dpg.group(horizontal=True):
                    dpg.add_text(self.t("lbl_search_title"),
                                 tag=self._reg("t_s_title", "value", "lbl_search_title"))
                    dpg.add_input_text(tag="search_title", width=260)
                    dpg.add_text(self.t("lbl_search_year"),
                                 tag=self._reg("t_s_year", "value", "lbl_search_year"))
                    dpg.add_input_text(tag="search_year", width=70)
                    dpg.add_text(self.t("lbl_tol"),
                                 tag=self._reg("t_tol", "value", "lbl_tol"))
                    dpg.add_input_int(tag="search_tol", width=90, step=1,
                                      min_value=0, min_clamped=True,
                                      default_value=int(self.get("options", "year_tol", "2") or 2))
                    dpg.add_button(tag=self._reg("b_search", "label", "btn_search"),
                                   label=self.t("btn_search"), callback=self.on_search)
                dpg.add_checkbox(tag=self._reg("opt_replace_img", "label", "chk_replace_img"),
                                 label=self.t("chk_replace_img"), default_value=True)
                dpg.add_separator()
                dpg.add_text(self.t("lbl_candidates"),
                             tag=self._reg("t_candidates", "value", "lbl_candidates"))
                with dpg.child_window(tag="cand_scroll", width=-1, height=-1):
                    dpg.add_group(tag="cand_group")

        dpg.add_separator()
        dpg.add_text(self.t("lbl_journal"),
                     tag=self._reg("t_journal", "value", "lbl_journal"))
        dpg.add_input_text(tag="log_box", multiline=True, readonly=True,
                           width=-1, height=140)


# =====================================================================
#  OUTIL 3 : Doublons (espace de noms isole)
# =====================================================================
def _init_doublons():
    LANGS = {
        "fr": {
            "connect":"Connecter","scan":"Scanner","save":"Sauvegarder",
            "load":"Charger scan","export":"Exporter","reset_ign":"Reinitialiser",
            "check_all":"Tout cocher","uncheck_all":"Tout decocher",
            "prev_btn":"<< Prec","next_btn":"Suiv >>",
            "flt_samedur":"Duree identique","flt_samesize":"Taille identique",
            "tip_samedur":"Ne montre que les groupes ou au moins deux versions ont la meme duree (a 1s pres).\nLes durees identiques sont surlignees en cyan.",
            "tip_samesize":"Ne montre que les groupes ou au moins deux versions ont exactement la meme taille.\nLes tailles identiques sont surlignees en jaune.",
            "ign_count":"%d ignore(s)",
            "app_title":"Detecteur de doublons Emby",
            "need_lib_title":"Selection requise",
            "need_lib_msg":"Necessite la selection d'au moins une mediatheque.",
            "all_check":"Tout cocher","all_uncheck":"Tout decocher",
            "open_all":"Ouvrir tout","compare":"Comparer","ignore":"Ignorer",
            "play":"Lire","folder_btn":"Dossier","close":"Fermer",
            "url_lbl":"URL","apikey_lbl":"Cle API","userid_lbl":"User ID",
            "userid_hint":"optionnel","prefix_lbl":"Prefixe",
            "player_lbl":"Lecteur video","player_hint":"C:\\...\\vlc.exe",
            "filter_lbl":"Filtre :","filter_hint":"Titre...","sort_lbl":"Trier :",
            "threshold_lbl":"Seuil min :","ignored_lbl":"Ignores :","reset_lbl":"Reinitialiser",
            "sub":"- lecture seule (DirectX 11)",
            "criteria_hdr":"Criteres - versions intentionnelles (coche = ignorer ce type de doublon)",
            "cr_resolution":"Resolution differente (4K/HD/SD)",
            "cr_hdr":"HDR vs SDR","cr_av1":"Codec AV1",
            "cr_3d":"Film 3D / SBS / MVC","cr_remaster":"Remastered",
            "cr_cut":"Version longue / Extended / Director's Cut",
            "cr_bonus":"Bonus / Extras / Featurette",
            "col_file":"Fichier","col_qual":"Qualite","col_folder":"Dossier NAS",
            "col_dur":"Duree","col_size":"Taille","col_actions":"Actions",
            "stat_groups":"groupes","stat_files":"fichiers",
            "stat_recover":"recuperable","stat_intent":"intentionnels","stat_ign":"ignores",
            "sec_dupes":"VRAIS DOUBLONS","sec_intent":"VERSIONS INTENTIONNELLES",
            "no_dupes":"Aucun doublon detecte.","no_real":"Aucun vrai doublon.",
            "sort_az":"Titre A>Z","sort_za":"Titre Z>A","sort_sz":"Taille",
            "sort_cd":"Confiance v","sort_ca":"Confiance ^",
            "libs_hint":"Cliquez sur Connecter pour charger les mediatheques.",
            "scan_win":"Scan en cours","lang_btn":"EN",
            "tip_player":"Chemin vers votre lecteur video (VLC, MPC-HC...).\nModification prise en compte IMMEDIATEMENT sans relancer le script.",
            "tip_browse":"Parcourir pour choisir le lecteur video.",
            "tip_open_all":"Ouvre tous les fichiers du groupe simultanement.\n\nATTENTION : votre lecteur video doit supporter plusieurs instances simultanees.\nVLC : Preferences > Interface > decocher une seule instance.\nMPC-HC : Options > Lecteur > Permettre plusieurs instances.",
            "tip_compare":"Affiche les metadonnees des fichiers cote a cote\n(resolution, codec, bitrate, pistes audio...)\nLes differences sont surlignees en jaune.",
            "tip_ignore":"Marque ce groupe comme faux positif.\nIl n apparaitra plus dans les resultats.\nRecuperez-le via Reinitialiser les ignores.",
            "tip_play":"Ouvre ce fichier avec le lecteur video configure.",
            "tip_folder":"Ouvre l explorateur Windows sur le dossier de ce fichier.",
            "tip_score":"Masque les groupes dont le score est inferieur a ce seuil.\n\nIMDB=100% TMDB=85% Titre=60% Similarite=40%",
            "tip_connect":"Verifie la connexion au serveur Emby et charge les mediatheques.\n\nIMPORTANT : si vous avez supprime des doublons manuellement,\nrafraichissez d'abord les mediatheques dans Emby avant de rescanner\n(Tableau de bord > Mediatheques > Analyser les mediatheques),\nsinon les fichiers supprimes apparaitront encore dans les resultats.",
            "tip_cr_resolution":"Coche = 4K et 1080p du meme film ne sont PAS des doublons.",
            "tip_cr_hdr":"Coche = version HDR et SDR ne sont PAS des doublons.",
            "tip_cr_av1":"Coche = fichier AV1 et H264/HEVC ne sont PAS des doublons.",
            "tip_cr_3d":"Coche = version 3D/SBS et version 2D ne sont PAS des doublons.",
            "tip_cr_remaster":"Coche = Remastered et original ne sont PAS des doublons.",
            "tip_cr_cut":"Coche = Extended, Director's Cut, Version Longue etc. ne sont PAS des doublons.",
            "tip_cr_bonus":"Coche = fichier Bonus/Extras/Featurette ne sont PAS des doublons.",
            "tip_scan":"Lance le scan des mediatheques selectionnees.\nUtilisez Connecter d'abord pour choisir les mediatheques,\nou Charger scan pour reafficher un scan precedent sans reconnecter.",
            "tip_save":"Sauvegarde les resultats du scan dans un fichier JSON local.",
            "tip_load":"Recharge les resultats du dernier scan sans reconnecter Emby.\nFonctionne hors ligne.",
            "tip_export":"Exporte le rapport des doublons en HTML ou CSV.",
            "cancel":"Annuler","delete":"Supprimer","delete_confirm":"Confirmer la suppression",
            "copy_path":"Copier","open_both":"Ouvrir les deux",
            "manage_ign":"Gerer","manage_ign_tip":"Voir et retirer des groupes ignorés individuellement.","filter_res":"Res. :","filter_codec":"Codec :",
            "all_res":"Toutes","all_codec":"Tous",
            "fuzzy_warn":"{n} film(s) sans identifiant ignores par le moteur fuzzy (plafond 200).",
            "scan_cancelled":"Scan annule.","page_of":"/{n} pages",
            "del_emby_only":"Retirer de la librairie (fichier conserve)",
            "del_emby_file":"Supprimer definitivement le fichier",
            "del_warning":"! Cette action est irreversible.",
            "ignored_panel":"Groupes ignores","remove_ignore":"Retirer",
            "hidden_score":"{n} groupe(s) masque(s) par le seuil de score.",
            "refresh_emby":"Analyser Emby",
            "tc_lbl":"Depart Ouvrir tout :",
            "tip_tc":"Timecode de depart utilise par 'Ouvrir tout' (et 'Ouvrir les deux').\nToutes les fenetres MPC-HC / MPC-BE / VLC / mpv / PotPlayer\ndemarrent a cet endroit. Vide ou 0 = depuis le debut.\n\nFormats : 1:02:30  45:00  1h05m  90s  45 (= minutes).\nMemorise automatiquement.",
            "tip_refresh_emby":"Lance l'analyse des mediatheques SELECTIONNEES sur le serveur Emby\n(equivalent de Tableau de bord > Mediatheques > Analyser).\nA utiliser apres avoir supprime des fichiers manuellement :\npatientez quelques instants puis relancez un scan pour rafraichir les resultats.",
        },
        "en": {
            "connect":"Connect","scan":"Scan","save":"Save",
            "load":"Load scan","export":"Export","reset_ign":"Reset",
            "check_all":"Check all","uncheck_all":"Uncheck all",
            "prev_btn":"<< Prev","next_btn":"Next >>",
            "flt_samedur":"Same duration","flt_samesize":"Same size",
            "tip_samedur":"Only shows groups where at least two versions share the same duration (within 1s).\nIdentical durations are highlighted in cyan.",
            "tip_samesize":"Only shows groups where at least two versions have exactly the same size.\nIdentical sizes are highlighted in yellow.",
            "ign_count":"%d ignored",
            "app_title":"Emby Duplicate Finder",
            "need_lib_title":"Selection required",
            "need_lib_msg":"Requires selecting at least one library.",
            "all_check":"Check all","all_uncheck":"Uncheck all",
            "open_all":"Open all","compare":"Compare","ignore":"Ignore",
            "play":"Play","folder_btn":"Folder","close":"Close",
            "url_lbl":"URL","apikey_lbl":"API Key","userid_lbl":"User ID",
            "userid_hint":"optional","prefix_lbl":"Prefix",
            "player_lbl":"Video player","player_hint":"C:\\...\\vlc.exe",
            "filter_lbl":"Filter:","filter_hint":"Title...","sort_lbl":"Sort:",
            "threshold_lbl":"Min score:","ignored_lbl":"Ignored:","reset_lbl":"Reset",
            "sub":"- read only (DirectX 11)",
            "criteria_hdr":"Criteria - intentional versions (check = ignore this duplicate type)",
            "cr_resolution":"Different resolution (4K/HD/SD)",
            "cr_hdr":"HDR vs SDR","cr_av1":"AV1 codec",
            "cr_3d":"3D / SBS / MVC film","cr_remaster":"Remastered",
            "cr_cut":"Long version / Extended / Director's Cut",
            "cr_bonus":"Bonus / Extras / Featurette",
            "col_file":"File","col_qual":"Quality","col_folder":"NAS Folder",
            "col_dur":"Duration","col_size":"Size","col_actions":"Actions",
            "stat_groups":"groups","stat_files":"files",
            "stat_recover":"recoverable","stat_intent":"intentional","stat_ign":"ignored",
            "sec_dupes":"TRUE DUPLICATES","sec_intent":"INTENTIONAL VERSIONS",
            "no_dupes":"No duplicates found.","no_real":"No true duplicates.",
            "sort_az":"Title A>Z","sort_za":"Title Z>A","sort_sz":"Size",
            "sort_cd":"Confidence v","sort_ca":"Confidence ^",
            "libs_hint":"Click Connect to load libraries.",
            "scan_win":"Scanning...","lang_btn":"FR",
            "tip_player":"Path to your video player (VLC, MPC-HC...).\nChanges take effect IMMEDIATELY without restarting.",
            "tip_browse":"Browse to choose your video player.",
            "tip_open_all":"Opens all files simultaneously.\n\nWARNING: your video player must support multiple instances.\nVLC: Preferences > Interface > uncheck Single instance.\nMPC-HC: Options > Player > Allow multiple instances.",
            "tip_compare":"Shows file metadata side by side\n(resolution, codec, bitrate, audio tracks...)\nDifferences highlighted in yellow.",
            "tip_ignore":"Mark this group as a false positive.\nIt will no longer appear in results.\nRestore via Reset ignored groups.",
            "tip_play":"Open this file with the configured video player.",
            "tip_folder":"Open Windows Explorer on this file folder.",
            "tip_score":"Hides groups whose score is below this threshold.\n\nIMDB=100% TMDB=85% Title=60% Similarity=40%",
            "tip_connect":"Checks Emby server connection and loads libraries.\n\nIMPORTANT: if you manually deleted duplicates,\nrefresh your libraries in Emby before rescanning\n(Dashboard > Libraries > Scan libraries),\notherwise deleted files will still appear in results.",
            "tip_cr_resolution":"Checked = 4K and 1080p of the same film are NOT duplicates.",
            "tip_cr_hdr":"Checked = HDR and SDR versions are NOT duplicates.",
            "tip_cr_av1":"Checked = AV1 and H264/HEVC files are NOT duplicates.",
            "tip_cr_3d":"Checked = 3D/SBS and 2D versions are NOT duplicates.",
            "tip_cr_remaster":"Checked = Remastered and original are NOT duplicates.",
            "tip_cr_cut":"Checked = Extended, Director's Cut, Long version etc. are NOT duplicates.",
            "tip_cr_bonus":"Checked = Bonus/Extras/Featurette files are NOT duplicates.",
            "tip_scan":"Starts scanning the selected libraries.\nUse Connect first to choose libraries,\nor Load scan to display a previous scan without reconnecting.",
            "tip_save":"Saves scan results to a local JSON file.",
            "tip_load":"Reloads the last scan without reconnecting to Emby.\nWorks offline.",
            "tip_export":"Exports the duplicate report as HTML or CSV.",
            "cancel":"Cancel","delete":"Delete","delete_confirm":"Confirm deletion",
            "copy_path":"Copy","open_both":"Open both",
            "manage_ign":"Manage","manage_ign_tip":"View and remove ignored groups individually.","filter_res":"Res.:","filter_codec":"Codec:",
            "all_res":"All","all_codec":"All",
            "fuzzy_warn":"{n} film(s) without identifier skipped by fuzzy engine (cap 200).",
            "scan_cancelled":"Scan cancelled.","page_of":"/{n} pages",
            "del_emby_only":"Remove from library (keep file)",
            "del_emby_file":"Delete file permanently",
            "del_warning":"! This action is irreversible.",
            "ignored_panel":"Ignored groups","remove_ignore":"Remove",
            "hidden_score":"{n} group(s) hidden by score threshold.",
            "refresh_emby":"Scan Emby",
            "tc_lbl":"Open all start:",
            "tip_tc":"Start timecode used by 'Open all' (and 'Open both').\nEvery MPC-HC / MPC-BE / VLC / mpv / PotPlayer window\nstarts there. Empty or 0 = from the start.\n\nFormats: 1:02:30  45:00  1h05m  90s  45 (= minutes).\nSaved automatically.",
            "tip_refresh_emby":"Triggers a scan of the SELECTED libraries on the Emby server\n(same as Dashboard > Libraries > Scan).\nUse after manually deleting files:\nwait a moment then re-run a scan to refresh results.",
        }
    }

    def t(key):
        """Retourne la traduction courante pour la cle donnee."""
        return LANGS[G.get("lang","fr")].get(key, key)

    # ══════════════════════════════════════════════════════════════
    #  FICHIERS PERSISTANTS
    # ══════════════════════════════════════════════════════════════
    CONFIG_FILE  = APP_DIR / (APP_STEM + ".ini")
    SCAN_FILE    = APP_DIR / (APP_STEM + "_resultats.json")
    IGNORED_FILE = APP_DIR / (APP_STEM + "_ignores.json")

    def load_config():
        cfg = configparser.ConfigParser()
        cfg["emby"] = {"url":"http://localhost:8096","api_key":"","user_id":"",
                       "nas_prefix":"/volume1","nas_unc":r"\\192.168.1.x","player":""}
        if CONFIG_FILE.exists():
            cfg.read(CONFIG_FILE, encoding="utf-8")
        if cfg.has_section("emby"):
            cfg["emby"]["api_key"] = decrypt_secret(cfg["emby"].get("api_key", ""))
        return cfg

    def save_config(d):
        d = dict(d)
        try:
            save_shared_creds(url=d.get("url", ""), api_key=d.get("api_key", ""),
                              user_id=d.get("user_id", ""),
                              nas_prefix=d.get("nas_prefix", ""),
                              nas_unc=d.get("nas_unc", ""), player=d.get("player", ""))
            push_shared_to_all_tabs()
        except Exception:
            pass
        d["api_key"] = encrypt_secret(d.get("api_key", ""))
        cfg = configparser.ConfigParser(); cfg["emby"] = d
        with open(CONFIG_FILE,"w",encoding="utf-8") as f: cfg.write(f)

    def load_ignored():
        if IGNORED_FILE.exists():
            try: return set(json.loads(IGNORED_FILE.read_text("utf-8")))
            except Exception: pass
        return set()

    def save_ignored(s):
        IGNORED_FILE.write_text(json.dumps(sorted(s),ensure_ascii=False,indent=2),"utf-8")

    def save_scan(dupes, multiqual, url, prefix, unc):
        mq = {k:{"items":v[0],"reason":v[1]} for k,v in multiqual.items()}
        p  = {"saved_at":time.strftime("%Y-%m-%d %H:%M:%S"),"server_url":url,
              "nas_prefix":prefix,"nas_unc":unc,"dupes":dupes,"multiqual":mq}
        tmp = SCAN_FILE.with_suffix(".tmp")
        tmp.write_text(json.dumps(p,ensure_ascii=False,indent=2),"utf-8")
        os.replace(tmp, SCAN_FILE)   # atomique : pas de fichier corrompu si kill
        return SCAN_FILE

    def load_scan():
        if not SCAN_FILE.exists(): raise FileNotFoundError(str(SCAN_FILE))
        p = json.loads(SCAN_FILE.read_text("utf-8"))
        if not {"saved_at","dupes","multiqual"}.issubset(p): raise ValueError("Fichier invalide")
        mq = {k:(v["items"],v["reason"]) for k,v in p["multiqual"].items()}
        return p["dupes"],mq,{"saved_at":p.get("saved_at","?"),"server_url":p.get("server_url","?"),
                              "nas_prefix":p.get("nas_prefix",""),"nas_unc":p.get("nas_unc","")}

    # ══════════════════════════════════════════════════════════════
    #  CONVERSION CHEMIN
    # ══════════════════════════════════════════════════════════════
    def to_win(path, prefix, unc):
        if not path or not unc: return path
        base = re.sub(r'\d+$','',prefix.rstrip('/')) or "/volume"
        m = re.match(r'^'+re.escape(base)+r'\d*',path,re.I)
        if m: return unc.rstrip("\\")+path[m.end():].replace("/","\\")
        return path.replace("/","\\")

    # ══════════════════════════════════════════════════════════════
    #  API EMBY (GET uniquement)
    # ══════════════════════════════════════════════════════════════
    def emby_get(base, key, path, params=None, _retry=3):
        p = dict(params or {}); p["api_key"] = key
        url = f"{base.rstrip('/')}{path}?{urllib.parse.urlencode(p)}"
        req = urllib.request.Request(url, headers={"Accept":"application/json"})
        for attempt in range(_retry):
            try:
                with urllib.request.urlopen(req, timeout=30) as r:
                    return json.loads(r.read().decode())
            except urllib.error.HTTPError:
                raise   # erreurs HTTP (401, 404...) → pas de retry
            except Exception:
                if attempt == _retry - 1:
                    raise
                time.sleep(1.5 ** attempt)  # backoff : 1s, 1.5s...

    # La suppression d'un gros fichier sur un NAS (disques en veille, SMB,
    # corbeille Synology...) peut largement depasser 15 s alors qu'Emby
    # termine bien l'operation : l'ancien timeout de 15 s affichait une
    # erreur pour une suppression pourtant reussie.
    DELETE_TIMEOUT = 180       # attente max de la reponse HTTP
    DELETE_VERIFY_SECS = 90    # apres un timeout : verification de disparition

    def emby_item_exists(base, key, item_id):
        """True si l'item est encore connu d'Emby, False s'il a disparu."""
        try:
            d = emby_get(base, key, "/Items",
                         {"Ids": item_id, "Recursive": "true", "Fields": "Path"},
                         _retry=1)
            return bool((d or {}).get("Items"))
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return False
            raise

    def _is_timeout(exc):
        if isinstance(exc, (TimeoutError, socket.timeout)):
            return True
        if isinstance(exc, urllib.error.URLError):
            r = getattr(exc, "reason", None)
            return isinstance(r, (TimeoutError, socket.timeout)) or "timed out" in str(r)
        return "timed out" in str(exc).lower()

    def emby_delete(base, key, item_id, delete_file=False, progress=None):
        """Appelle DELETE /Items/{id} sur le serveur Emby.
        Double auth (query param + header) pour compatibilité maximale.
        Timeout long ; si la reponse n'arrive quand meme pas, on verifie
        aupres d'Emby que l'item a bien disparu avant de conclure a un echec.
        Retourne le code HTTP, ou 0 si le succes a ete constate apres timeout."""
        params = {"api_key": key}
        if delete_file:
            params["deleteFiles"] = "true"
        url = f"{base.rstrip('/')}/Items/{item_id}?{urllib.parse.urlencode(params)}"
        req = urllib.request.Request(url, method="DELETE",
                                     headers={"X-Emby-Token": key,
                                              "Accept": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=DELETE_TIMEOUT) as r:
                return r.status
        except Exception as e:
            if not _is_timeout(e):
                raise
        # Timeout : Emby continue souvent la suppression en arriere-plan.
        t_end = time.time() + DELETE_VERIFY_SECS
        while time.time() < t_end:
            if progress:
                progress(int(t_end - time.time()))
            try:
                if not emby_item_exists(base, key, item_id):
                    return 0
            except Exception:
                pass
            time.sleep(3)
        raise TimeoutError(
            f"Emby n'a pas répondu en {DELETE_TIMEOUT} s et l'élément est "
            f"toujours présent après {DELETE_VERIFY_SECS} s de vérification.\n"
            "La suppression continue peut-être côté serveur : relancez un scan "
            "dans quelques minutes avant de réessayer.")

    def emby_refresh_item(base, key, item_id):
        """Déclenche l'analyse d'UNE médiathèque (POST /Items/{id}/Refresh).
        Portée limitée aux médiathèques sélectionnées par l'utilisateur."""
        params = {"api_key": key, "Recursive": "true",
                  "MetadataRefreshMode": "Default", "ImageRefreshMode": "Default"}
        url = (f"{base.rstrip('/')}/Items/{item_id}/Refresh?"
               f"{urllib.parse.urlencode(params)}")
        req = urllib.request.Request(url, data=b"", method="POST",
                                     headers={"X-Emby-Token": key,
                                              "Accept": "application/json"})
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status

    def fetch_movies(base, key, uid, cb, parent_ids=None):
        """
        parent_ids : liste d'IDs de médiathèques à scanner.
        Si None ou vide → scan global (comportement original).
        Respecte le flag _cancel_scan pour annulation propre.
        """
        base_params = {"Recursive":"true","IncludeItemTypes":"Movie",
                       "Fields":"ProviderIds,Path,MediaSources,ProductionYear,DateCreated,MediaStreams",
                       "Limit":500}
        if uid: base_params["UserId"] = uid

        # Liste des scopes à parcourir
        scopes = parent_ids if parent_ids else [None]
        all_items, seen_ids = [], set()

        for pid in scopes:
            if _cancel_scan.is_set(): break
            params = dict(base_params); params["StartIndex"] = 0
            if pid: params["ParentId"] = pid
            page = 0
            while True:
                if _cancel_scan.is_set(): break
                data  = emby_get(base,key,"/Items",dict(params))
                items = data.get("Items",[])
                for it in items:
                    if it.get("Id") not in seen_ids:
                        seen_ids.add(it.get("Id")); all_items.append(it)
                total_scope = data.get("TotalRecordCount",0)
                fetched_in_scope = params["StartIndex"] + len(items)
                remaining = max(0, total_scope - fetched_in_scope)
                page += 1; cb(len(all_items), len(all_items) + remaining, page)
                if len(items)<params["Limit"] or not items: break
                params["StartIndex"] += len(items)

        return all_items

    # ══════════════════════════════════════════════════════════════
    #  ANALYSE MÉTADONNÉES
    # ══════════════════════════════════════════════════════════════
    _RES_RE   = re.compile(r'\b(4K|UHD|2160p?|1080p?|720p?|480p?)\b',re.I)
    _CODEC_RE = re.compile(r'\b(AV1|HEVC|[Hx]\.?265|[Hx]\.?264|VP9|AVC)\b',re.I)
    _HDR_RE   = re.compile(r'\b(HDR10?\+?|Dolby[\. ]?Vision|DV|HLG)\b',re.I)
    # Critères séparés - chacun contrôlable par checkbox
    _3D_RE      = re.compile(r'\b(3D|SBS|OU|half[.\s_-]?SBS|full[.\s_-]?SBS|MVC)\b', re.I)
    _REMASTER_RE= re.compile(r'\b(remastered|remasterise)\b', re.I)
    _BONUS_RE   = re.compile(r'\b(bonus|extras?|featurette|behind[.\s_-]?the[.\s_-]?scenes|making[.\s_-]?of|deleted[.\s_-]?scenes|interviews?)\b', re.I)
    _CUT_RE     = re.compile(
        r'\b(extended|version[.\s_-]?longue|long[.\s_-]?cut|'
        r"director(?:['’]?s)?[.\s_-]?cut|theatrical[.\s_-]?cut|theatrical|"
        r'unrated|uncensored|final[.\s_-]?cut|redux|special[.\s_-]?edition)\b', re.I)
    _VER_KW     = re.compile(   # union pour la rétrocompatibilité
        r'\b(extended|version[.\s_-]?longue|long[.\s_-]?cut|'
        r"director(?:['’]?s)?[.\s_-]?cut|theatrical[.\s_-]?cut|theatrical|"
        r'unrated|uncensored|final[.\s_-]?cut|redux|remastered|'
        r'special[.\s_-]?edition|3D|SBS|OU|MVC|bonus|extras?|featurette)\b', re.I)

    def _nc(c):
        c=(c or "").upper()
        if c in ("HEVC","H265","H.265","X265"): return "HEVC"
        if c in ("H264","H.264","X264","AVC"):  return "H264"
        if c in ("AV1",): return "AV1"
        if c in ("VP9",): return "VP9"
        return c or "?"

    def get_all_sources(movie):
        srcs = movie.get("MediaSources",[])
        if srcs: return [s.get("Path","") for s in srcs if s.get("Path")]
        p = movie.get("Path",""); return [p] if p else []

    def get_api_size(item):
        return (item.get("MediaSources") or [{}])[0].get("Size",0) or 0

    def get_rich_metadata(movie):
        mid = movie.get("Id", "")
        if mid and mid in _meta_cache:
            return _meta_cache[mid]
        md = {"width":0,"height":0,"res_label":"?","res_tier":0,
              "vcodec":"?","acodec":"?","channels":"?","hdr":False,
              "bitrate_kbps":0,"duration_s":0,"size_bytes":0,
              "date_added":movie.get("DateCreated","")[:10],
              "audio_tracks":[]}   # liste de dicts {codec, channels, lang, title}
        srcs = movie.get("MediaSources") or []  # None → [] (Emby peut envoyer null)
        src = srcs[0] if srcs else {}
        md["size_bytes"]   = src.get("Size",0) or 0
        md["bitrate_kbps"] = (src.get("Bitrate",0) or 0)//1000
        ticks = src.get("RunTimeTicks",0) or movie.get("RunTimeTicks",0) or 0
        md["duration_s"]   = ticks//10_000_000
        streams = src.get("MediaStreams") or movie.get("MediaStreams") or []
        video = next((s for s in streams if s.get("Type")=="Video"),None)

        # Toutes les pistes audio
        audio_streams = [s for s in streams if s.get("Type")=="Audio"]
        for a in audio_streams:
            lang  = (a.get("Language") or "").strip().upper() or "?"
            title = (a.get("DisplayTitle") or a.get("Title") or "").strip()
            codec = (a.get("Codec") or "?").upper()
            ch    = str(a.get("Channels","?"))
            md["audio_tracks"].append({"lang":lang,"codec":codec,
                                       "channels":ch,"title":title})

        # Résumé piste principale (compatibilité)
        if audio_streams:
            a = audio_streams[0]
            md["acodec"]   = (a.get("Codec") or "?").upper()
            md["channels"] = str(a.get("Channels","?"))

        if video:
            w,h = video.get("Width",0) or 0, video.get("Height",0) or 0
            mx = max(w,h)
            tier = 4 if mx>=2160 else 3 if mx>=1080 else 2 if mx>=720 else 1 if mx>0 else 0
            md["width"]=w; md["height"]=h; md["res_tier"]=tier
            md["vcodec"] = _nc(video.get("Codec",""))
            md["hdr"]    = video.get("VideoRange","").upper() not in ("SDR","")
            lbl = {4:"4K/UHD",3:"1080p",2:"720p",1:"SD",0:"?"}
            md["res_label"] = f"{lbl[tier]} ({w}x{h})" if w else lbl[tier]
        if md["vcodec"]=="?":
            paths=get_all_sources(movie); fname=Path(paths[0]).name if paths else ""
            mc=_CODEC_RE.search(fname)
            if mc: md["vcodec"]=_nc(mc.group(0))
        if mid:
            _meta_cache[mid] = md   # mise en cache pour éviter re-parse
        return md

    def get_quality_signature(movie):
        md = get_rich_metadata(movie)
        return {"res_tier":md["res_tier"],"codec":md["vcodec"],
                "hdr":md["hdr"],"res_label":md["res_label"],"codec_raw":md["vcodec"]}

    def fmt_size(b):
        if not b: return "?"
        for u in ["o","Ko","Mo","Go","To"]:
            if b<1024: return f"{b:.1f}{u}"
            b/=1024
        return f"{b:.1f}Po"

    def _pair_same(vals, tol=0):
        """Vrai si au moins deux valeurs non nulles sont identiques (à tol près)."""
        vs = [v for v in vals if v]
        for i in range(len(vs)):
            for j in range(i + 1, len(vs)):
                if abs(vs[i] - vs[j]) <= tol:
                    return True
        return False


    def fmt_duration(s):
        if not s: return "?"
        h,r=divmod(int(s),3600); m,sc=divmod(r,60)
        return f"{h}h{m:02d}m{sc:02d}s" if h else f"{m}m{sc:02d}s"

    def confidence_score(key):
        if key.startswith("imdb:"):  return 100,"IMDB",(46,204,113)
        if key.startswith("tmdb:"):  return  85,"TMDB",(240,160,0)
        if key.startswith("title:"): return  60,"Titre",(220,120,0)
        if key.startswith("fuzzy:"): return  40,"Fuzzy",(200,60,60)
        return 50,"?",(136,136,136)

    # ══════════════════════════════════════════════════════════════
    #  DÉTECTION DOUBLONS
    # ══════════════════════════════════════════════════════════════
    def normalize_title(t):
        t=t.lower(); t=re.sub(r"[^\w\s]","",t); t=re.sub(r"\s+"," ",t).strip()
        return re.sub(r"^(le|la|les|the|a|an|l|un|une)\s+","",t)

    def _ver_tag(movie):
        paths=get_all_sources(movie)
        fname=Path(paths[0]).name if paths else movie.get("Name","")
        m=_VER_KW.search(fname); return m.group(0).lower() if m else ""

    def _fname(movie):
        paths=get_all_sources(movie)
        return Path(paths[0]).name if paths else movie.get("Name","")

    def is_intentional(items, criteria=None):
        """
        criteria : dict des critères actifs (None = utilise G["criteria"]).
        Clés : resolution, hdr, av1, 3d, remaster, cut
        """
        c = criteria if criteria is not None else G.get("criteria",{})
        sigs=[get_quality_signature(m) for m in items]
        tiers={s["res_tier"] for s in sigs if s["res_tier"]>0}
        codecs={s["codec"] for s in sigs if s["codec"] not in ("?","")}
        hdrs={s["hdr"] for s in sigs}
        reasons=[]

        if c.get("resolution",True) and len(tiers)>1:
            reasons.append("resolutions diff. ("+"/".join(sorted({s["res_label"] for s in sigs}))+")")

        if c.get("hdr",True) and len(hdrs)>1:
            reasons.append("HDR vs SDR")

        if c.get("av1",True) and "AV1" in codecs and len(codecs)>1:
            reasons.append("AV1 vs autre codec")

        if c.get("3d",True):
            for m in items:
                if _3D_RE.search(_fname(m)):
                    reasons.append("3D/SBS detecte"); break

        if c.get("remaster",True):
            for m in items:
                if _REMASTER_RE.search(_fname(m)):
                    reasons.append("Remastered detecte"); break

        if c.get("cut",True):
            tags=set()
            for m in items:
                mr=_CUT_RE.search(_fname(m))
                if mr: tags.add(mr.group(0).lower())
            if tags: reasons.append("cuts ("+"/".join(sorted(tags))+")")

        if c.get("bonus",True):
            for m in items:
                if _BONUS_RE.search(_fname(m)):
                    reasons.append("bonus/extras detecte"); break

        return bool(reasons)," + ".join(reasons)

    def find_fuzzy_dupes(movies):
        cands=[m for m in movies
               if not m.get("ProviderIds",{}).get("Imdb","").strip()
               and not m.get("ProviderIds",{}).get("Tmdb","").strip()]
        capped = max(0, len(cands) - 200)
        G["fuzzy_capped"] = capped
        if capped: cands = cands[:200]   # cap O(n²)
        groups,used={},set()
        for i,a in enumerate(cands):
            if i in used: continue
            ta=normalize_title(a.get("Name","")); ya=a.get("ProductionYear",0) or 0
            grp=[a]
            for j,b in enumerate(cands):
                if j<=i or j in used: continue
                tb=normalize_title(b.get("Name","")); yb=b.get("ProductionYear",0) or 0
                if abs(ya-yb)>1: continue
                if difflib.SequenceMatcher(None,ta,tb).ratio()>=0.82 and ta!=tb:
                    grp.append(b); used.add(j)
            if len(grp)>1: used.add(i); groups[f"fuzzy:{ta}:{ya}"]=grp
        return groups

    def find_duplicates(movies, step_cb):
        groups={}; total=len(movies)
        for idx,m in enumerate(movies):
            pids=m.get("ProviderIds",{})
            imdb=pids.get("Imdb","").strip(); tmdb=pids.get("Tmdb","").strip()
            key=(f"imdb:{imdb}" if imdb else f"tmdb:{tmdb}" if tmdb
                 else f"title:{normalize_title(m.get('Name',''))}:{m.get('ProductionYear','')}")
            groups.setdefault(key,[]).append(m)
            if idx%20==0 or idx==total-1: step_cb(idx+1,total,m.get("Name",""))
        # Exclure les films déjà dans un groupe existant pour éviter les doublons titre+fuzzy
        already_grouped = {id(it) for g in groups.values() for it in g if len(g) > 1}
        for k,v in find_fuzzy_dupes(movies).items():
            filtered = [it for it in v if id(it) not in already_grouped]
            if len(filtered) > 1 and k not in groups:
                groups[k] = filtered
        real,mq={},{}
        for k,v in groups.items():
            if len(v)<=1: continue
            intl,reason=is_intentional(v)
            if intl: mq[k]=(v,reason)
            else:    real[k]=v
        return real,mq

    def compute_stats(dupes):
        """Retourne (nb_groupes, nb_fichiers, gain_min, gain_max).
        gain_min : si on supprime seulement le plus petit fichier de chaque groupe.
        gain_max : si on garde seulement le plus grand (meilleure qualité) de chaque groupe.
        """
        ng, nf, gain_min, gain_max = 0, 0, 0, 0
        for items in dupes.values():
            ng += 1
            # Utiliser size_bytes des métadonnées, fallback sur Size API
            sizes = sorted([get_rich_metadata(it)["size_bytes"] or get_api_size(it)
                            for it in items], reverse=True)
            nf += len(items)
            if len(sizes) > 1:
                gain_min += sizes[-1]          # on supprime seulement le plus petit
                gain_max += sum(sizes[1:])     # on garde le plus grand (meilleure qualité)
        return ng, nf, gain_min, gain_max

    # ══════════════════════════════════════════════════════════════
    #  EXPORT
    # ══════════════════════════════════════════════════════════════
    def export_csv(dupes, multiqual, fp, prefix, unc):
        with open(fp,"w",newline="",encoding="utf-8-sig") as f:
            w=csv.writer(f,delimiter=";")
            w.writerow(["Type","#","Titre","Année","Score","Fichier","Dossier","Taille","Qualité"])
            for g_idx,(key,items) in enumerate(dupes.items(),1):
                sc,sl,_=confidence_score(key); first=items[0]
                for item in items:
                    for p in get_all_sources(item):
                        wp=to_win(p,prefix,unc) or p; sig=get_quality_signature(item)
                        w.writerow(["Doublon",g_idx,first.get("Name","?"),
                                    first.get("ProductionYear",""),sc,
                                    Path(p).name,str(Path(wp).parent),
                                    fmt_size(get_api_size(item)),
                                    f"{sig['res_label']} {sig['codec_raw']}"])

    def export_html(dupes, multiqual, fp, prefix, unc):
        ng, nf, gain_min, gain_max = compute_stats(dupes)
        nq  = len(multiqual)
        ts  = time.strftime("%d/%m/%Y à %H:%M")

        # ── Données pour graphiques ──────────────────────────────────────────────

        # Répartition par score de confiance
        score_counts = {"IMDB (100%)":0, "TMDB (85%)":0, "Titre (60%)":0, "Fuzzy (40%)":0}
        for key in dupes:
            sc,sl,_ = confidence_score(key)
            if sc==100:   score_counts["IMDB (100%)"]+=1
            elif sc==85:  score_counts["TMDB (85%)"]+=1
            elif sc==60:  score_counts["Titre (60%)"]+=1
            else:         score_counts["Fuzzy (40%)"]+=1

        # Top 10 doublons par espace gaspillé
        top10 = []
        for key, items in dupes.items():
            # Utiliser size_bytes des métadonnées, fallback sur Size API
            sizes = sorted([get_rich_metadata(it)["size_bytes"] or get_api_size(it)
                            for it in items], reverse=True)
            wasted = sum(sizes[1:]) if len(sizes)>1 else 0
            if wasted > 0:
                first = items[0]
                top10.append({
                    "title": f"{first.get('Name','?')} ({first.get('ProductionYear','')})",
                    "wasted": wasted,
                    "files": len(items),
                    "sc": confidence_score(key)[1]
                })
        top10 = sorted(top10, key=lambda x: -x["wasted"])[:10]

        # Répartition par résolution
        res_counts = {"4K/UHD":0, "1080p":0, "720p":0, "SD":0, "?":0}
        for items in dupes.values():
            for item in items:
                sig = get_quality_signature(item)
                tier = sig["res_tier"]
                k = {4:"4K/UHD",3:"1080p",2:"720p",1:"SD"}.get(tier,"?")
                res_counts[k] += 1

        # Répartition codec
        codec_counts = {}
        for items in dupes.values():
            for item in items:
                sig = get_quality_signature(item)
                c = sig["codec_raw"] or "?"
                codec_counts[c] = codec_counts.get(c,0)+1

        # Tableau détaillé
        rows_html = ""
        for g_idx,(key,items) in enumerate(dupes.items(),1):
            sc,sl,_ = confidence_score(key)
            first   = items[0]
            title   = f"{first.get('Name','?')} ({first.get('ProductionYear','')})"
            sizes   = sorted([get_api_size(it) for it in items],reverse=True)
            wasted  = sum(sizes[1:]) if len(sizes)>1 else 0
            badge_col = {"IMDB":"#2ecc71","TMDB":"#f39c12","Titre":"#e67e22","Fuzzy":"#e74c3c"}.get(sl,"#888")
            rows_html += (f'<tr class="grp"><td>{g_idx}</td>'
                          f'<td colspan="4"><b>{title}</b>'
                          f'  <span class="badge" style="background:{badge_col}">{sl} {sc}%</span>'
                          f'  <span class="wasted">Gaspille : {fmt_size(wasted)}</span></td></tr>\n')
            for item in items:
                for p in get_all_sources(item):
                    wp  = to_win(p,prefix,unc) or p
                    sig = get_quality_signature(item)
                    sz  = fmt_size(get_api_size(item))
                    rows_html += (f'<tr><td></td><td>{Path(p).name}</td>'
                                  f'<td>{sig["res_label"]} {sig["codec_raw"]}</td>'
                                  f'<td style="color:#aaa;font-size:.8em">{str(Path(wp).parent)}</td>'
                                  f'<td style="text-align:right">{sz}</td></tr>\n')

        rows_mq = ""
        for g_idx,(key,payload) in enumerate(multiqual.items(),1):
            items,reason = payload
            first = items[0]
            title = f"{first.get('Name','?')} ({first.get('ProductionYear','')})"
            rows_mq += (f'<tr class="grp-mq"><td>{g_idx}</td>'
                        f'<td colspan="4"><b>{title}</b>'
                        f'  <span style="color:#aaa;font-size:.85em">{reason}</span></td></tr>\n')

        # ── JSON pour Chart.js ──────────────────────────────────────────────────
        sc_labels = json.dumps(list(score_counts.keys()))
        sc_data   = json.dumps(list(score_counts.values()))
        sc_colors = json.dumps(["#2ecc71","#f39c12","#e67e22","#e74c3c"])

        res_labels = json.dumps([k for k,v in res_counts.items() if v>0])
        res_data   = json.dumps([v for v in res_counts.values() if v>0])
        res_colors = json.dumps(["#e94560","#0f3460","#f39c12","#2ecc71","#888"])

        cod_labels = json.dumps(list(codec_counts.keys()))
        cod_data   = json.dumps(list(codec_counts.values()))

        top_labels = json.dumps([item["title"][:35]+"..." if len(item["title"])>35
                                   else item["title"] for item in top10])
        top_data   = json.dumps([round(item["wasted"]/1e9,2) for item in top10])

        html = f"""<!DOCTYPE html>
    <html lang="fr">
    <head>
    <meta charset="utf-8">
    <title>Emby Duplicate Finder — Rapport {ts}</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
    <style>
    *{{box-sizing:border-box;margin:0;padding:0}}
    body{{font-family:'Segoe UI',sans-serif;background:#12121e;color:#e0e0e0;padding:0}}
    .header{{background:linear-gradient(135deg,#1a1a3e,#0f3460);padding:28px 40px;
             border-bottom:3px solid #e94560}}
    .header h1{{font-size:2em;color:#e94560;margin-bottom:4px}}
    .header .sub{{color:#8888aa;font-size:.95em}}
    .content{{padding:28px 40px}}
    .kpi-grid{{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));
               gap:16px;margin-bottom:32px}}
    .kpi{{background:#16213e;border-radius:12px;padding:20px;text-align:center;
          border:1px solid #0f3460;transition:transform .2s}}
    .kpi:hover{{transform:translateY(-3px)}}
    .kpi .val{{font-size:2.2em;font-weight:700;line-height:1.1}}
    .kpi .lbl{{color:#8888aa;font-size:.85em;margin-top:4px}}
    .kpi.red .val{{color:#e94560}}
    .kpi.orange .val{{color:#f39c12}}
    .kpi.green .val{{color:#2ecc71}}
    .kpi.blue .val{{color:#88ccff}}
    .kpi.gray .val{{color:#aaa}}
    .charts{{display:grid;grid-template-columns:1fr 1fr;gap:24px;margin-bottom:32px}}
    .chart-box{{background:#16213e;border-radius:12px;padding:20px;border:1px solid #0f3460}}
    .chart-box h3{{color:#88ccff;margin-bottom:16px;font-size:1em;text-transform:uppercase;
                   letter-spacing:.05em}}
    .chart-box.wide{{grid-column:1/-1}}
    canvas{{max-height:280px}}
    h2{{color:#e94560;margin:32px 0 12px;font-size:1.1em;text-transform:uppercase;
        letter-spacing:.08em;border-bottom:1px solid #0f3460;padding-bottom:8px}}
    table{{width:100%;border-collapse:collapse;font-size:.875em;margin-bottom:32px}}
    th{{background:#0f3460;padding:10px 12px;text-align:left;color:#88ccff;
        font-weight:600;position:sticky;top:0}}
    td{{padding:7px 12px;border-bottom:1px solid #1e2a3a}}
    tr:hover td{{background:#1e2a40}}
    tr.grp td{{background:#2a1a1a;font-weight:600;padding:10px 12px}}
    tr.grp-mq td{{background:#0f1f30;padding:10px 12px}}
    .badge{{display:inline-block;padding:2px 8px;border-radius:20px;font-size:.78em;
            font-weight:700;color:#000;margin-left:8px;vertical-align:middle}}
    .wasted{{color:#e94560;font-size:.82em;margin-left:12px;font-weight:400}}
    .footer{{background:#0a0a18;text-align:center;padding:16px;color:#445;font-size:.8em;
             border-top:1px solid #1e2a3a}}
    </style>
    </head>
    <body>

    <div class="header">
      <h1>🎬 Emby Duplicate Finder</h1>
      <div class="sub">Rapport généré le {ts} &nbsp;·&nbsp; By Popov2026 &copy; 2026</div>
    </div>

    <div class="content">

    <!-- KPIs -->
    <div class="kpi-grid">
      <div class="kpi red">
        <div class="val">{ng}</div>
        <div class="lbl">Groupes de doublons</div>
      </div>
      <div class="kpi orange">
        <div class="val">{nf}</div>
        <div class="lbl">Fichiers concernés</div>
      </div>
      <div class="kpi green">
        <div class="val">{fmt_size(gain_min)}</div>
        <div class="lbl">Gain minimum<br><small>(supprimer les plus petits)</small></div>
      </div>
      <div class="kpi green">
        <div class="val">{fmt_size(gain_max)}</div>
        <div class="lbl">Gain maximum<br><small>(ne garder que le meilleur)</small></div>
      </div>
      <div class="kpi blue">
        <div class="val">{nq}</div>
        <div class="lbl">Versions intentionnelles</div>
      </div>
    </div>

    <!-- Graphiques -->
    <div class="charts">
      <div class="chart-box">
        <h3>Répartition par score de confiance</h3>
        <canvas id="chartScore"></canvas>
      </div>
      <div class="chart-box">
        <h3>Répartition par résolution</h3>
        <canvas id="chartRes"></canvas>
      </div>
      <div class="chart-box">
        <h3>Répartition par codec vidéo</h3>
        <canvas id="chartCodec"></canvas>
      </div>
      <div class="chart-box">
        <h3>Top 10 — Espace gaspillé par groupe (Go)</h3>
        <canvas id="chartTop"></canvas>
      </div>
    </div>

    <!-- Tableau doublons -->
    <h2>⚠ Vrais doublons — {ng} groupe(s)</h2>
    <table>
      <tr><th>#</th><th>Fichier</th><th>Qualité</th><th>Dossier</th><th>Taille</th></tr>
      {rows_html}
    </table>

    <!-- Tableau intentionnels -->
    <h2>ℹ Versions intentionnelles — {nq} groupe(s)</h2>
    <table>
      <tr><th>#</th><th colspan="4">Titre — Raison</th></tr>
      {rows_mq}
    </table>

    </div>

    <div class="footer">
      Emby Duplicate Finder &nbsp;·&nbsp; By Popov2026 &copy; 2026 &nbsp;·&nbsp;
      Rapport du {ts}
    </div>

    <script>
    const DARK = '#16213e', GRID = '#1e2a3a', TEXT = '#8888aa';
    const defaults = {{
      plugins:{{legend:{{labels:{{color:'#e0e0e0',font:{{size:12}}}}}}}},
      scales:{{
        x:{{ticks:{{color:TEXT}},grid:{{color:GRID}}}},
        y:{{ticks:{{color:TEXT}},grid:{{color:GRID}}}}
      }}
    }};

    // Score confiance
    new Chart(document.getElementById('chartScore'),{{
      type:'doughnut',
      data:{{labels:{sc_labels},datasets:[{{data:{sc_data},backgroundColor:{sc_colors},
        borderColor:'#12121e',borderWidth:2}}]}},
      options:{{plugins:{{legend:{{labels:{{color:'#e0e0e0'}}}}}}}}
    }});

    // Résolution
    new Chart(document.getElementById('chartRes'),{{
      type:'pie',
      data:{{labels:{res_labels},datasets:[{{data:{res_data},backgroundColor:{res_colors},
        borderColor:'#12121e',borderWidth:2}}]}},
      options:{{plugins:{{legend:{{labels:{{color:'#e0e0e0'}}}}}}}}
    }});

    // Codec
    new Chart(document.getElementById('chartCodec'),{{
      type:'bar',
      data:{{labels:{cod_labels},datasets:[{{label:'Fichiers',data:{cod_data},
        backgroundColor:'#0f3460',borderColor:'#e94560',borderWidth:1}}]}},
      options:{{...defaults,plugins:{{legend:{{display:false}}}}}}
    }});

    // Top 10
    new Chart(document.getElementById('chartTop'),{{
      type:'bar',
      data:{{labels:{top_labels},datasets:[{{label:'Go gaspillés',data:{top_data},
        backgroundColor:'#e94560cc',borderColor:'#e94560',borderWidth:1}}]}},
      options:{{...defaults,indexAxis:'y',plugins:{{legend:{{display:false}}}}}}
    }});
    </script>

    </body>
    </html>"""
        Path(fp).write_text(html, encoding="utf-8")

    # ══════════════════════════════════════════════════════════════
    #  OUVERTURE FICHIERS
    # ══════════════════════════════════════════════════════════════
    def _clean_player(p):
        return (p or "").strip().strip('"').strip("'").strip()

    def get_player():
        """Lit toujours le lecteur depuis l'UI - jamais de valeur figée.
        Priorite au champ VISIBLE du bandeau commun (sh_player) : le champ
        de l'onglet (dbl_inp_player, masque) n'est resynchronise qu'au clic
        sur Enregistrer, il pouvait donc etre vide ou perime."""
        cands = []
        for tag in ("sh_player", "dbl_inp_player"):
            try:
                if dpg.does_item_exist(tag):
                    cands.append(_clean_player(dpg.get_value(tag)))
            except Exception:
                pass
        try:
            cands.append(_clean_player(get_shared_creds().get("player", "")))
        except Exception:
            pass
        cands.append(_clean_player(G.get("player", "")))
        cands = [c for c in cands if c]
        for c in cands:
            if Path(c).is_file():
                return c
        return cands[0] if cands else ""

    _KNOWN_PLAYERS = (
        r"%ProgramFiles%\MPC-HC\mpc-hc64.exe",
        r"%ProgramFiles(x86)%\MPC-HC\mpc-hc.exe",
        r"%ProgramFiles(x86)%\K-Lite Codec Pack\MPC-HC64\mpc-hc64.exe",
        r"%ProgramFiles(x86)%\K-Lite Codec Pack\MPC-HC\mpc-hc.exe",
        r"%ProgramFiles%\MPC-BE x64\mpc-be64.exe",
        r"%ProgramFiles(x86)%\MPC-BE\mpc-be.exe",
        r"%ProgramFiles%\VideoLAN\VLC\vlc.exe",
        r"%ProgramFiles(x86)%\VideoLAN\VLC\vlc.exe",
        r"%ProgramFiles%\DAUM\PotPlayer\PotPlayerMini64.exe",
    )

    def _assoc_player(ext):
        """Programme associe a l'extension par Windows (ex. MPC-HC pour .mkv)."""
        if sys.platform != "win32":
            return ""
        try:
            import ctypes
            from ctypes import wintypes
            fn = ctypes.windll.shlwapi.AssocQueryStringW
            fn.argtypes = [ctypes.c_uint, ctypes.c_uint, wintypes.LPCWSTR,
                           wintypes.LPCWSTR, wintypes.LPWSTR,
                           ctypes.POINTER(wintypes.DWORD)]
            size = wintypes.DWORD(1024)
            buf = ctypes.create_unicode_buffer(1024)
            # ASSOCF_NONE=0, ASSOCSTR_EXECUTABLE=2
            if fn(0, 2, ext or ".mkv", "open", buf, ctypes.byref(size)) == 0:
                return buf.value
        except Exception:
            pass
        return ""

    def resolve_player(player, sample_path=""):
        """Chemin d'un lecteur utilisable : celui configure s'il existe, sinon
        le lecteur associe par Windows a l'extension, sinon une installation
        standard de MPC-HC / MPC-BE / VLC / PotPlayer. '' si rien."""
        player = _clean_player(player)
        if player and Path(player).is_file():
            return player
        ext = Path(sample_path).suffix if sample_path else ".mkv"
        a = _assoc_player(ext)
        if a and Path(a).is_file() and player_kind(a):
            return a
        if sys.platform == "win32":
            for c in _KNOWN_PLAYERS:
                c = os.path.expandvars(c)
                if Path(c).is_file():
                    return c
        return ""


    # ── Timecode de depart (bouton "Ouvrir tout") ──────────────────
    TIMECODE_FILE = APP_DIR / "emby_toolbox_timecode.json"

    def parse_timecode(txt):
        """'1:23:45' / '23:45' / '1h23m' / '45m' / '90s' / '42' (minutes)
        -> secondes (int). '' ou '0' -> 0. Invalide -> None."""
        t = (txt or "").strip().lower().replace(" ", "")
        if not t:
            return 0
        if ":" in t:
            parts = t.split(":")
            if len(parts) > 3 or not all(p.isdigit() for p in parts):
                return None
            sec = 0
            for p in parts:
                sec = sec * 60 + int(p)
            return sec
        m = re.fullmatch(r"(?:(\d+)h)?(?:(\d+)m(?:in)?)?(?:(\d+)s?)?", t)
        if not m or not any(m.groups()):
            return None
        h, mi, se = (int(g) if g else 0 for g in m.groups())
        if not t.endswith("s") and m.group(3) and not m.group(2):
            mi, se = se, 0                   # '42' ou '1h30' : minutes
        return h * 3600 + mi * 60 + se

    def fmt_timecode(sec):
        sec = int(sec or 0)
        return f"{sec // 3600}:{sec // 60 % 60:02d}:{sec % 60:02d}"

    def load_last_timecode():
        try:
            return json.loads(TIMECODE_FILE.read_text("utf-8")).get("timecode", "")
        except Exception:
            return ""

    def save_last_timecode(tc):
        try:
            TIMECODE_FILE.write_text(json.dumps({"timecode": tc}), "utf-8")
        except Exception:
            pass

    def player_kind(player):
        n = re.split(r"[\\/]", (player or "").strip())[-1].lower()
        if n.startswith("mpc"):        return "mpc"      # mpc-hc, mpc-hc64, mpc-be64
        if n.startswith("vlc"):        return "vlc"
        if n.startswith("mpv"):        return "mpv"
        if "potplayer" in n:           return "pot"
        return ""

    def player_cmd(player, path, start_s=0):
        """Ligne de commande du lecteur, avec position de depart si demandee."""
        start_s = int(start_s or 0)
        k = player_kind(player)
        if start_s <= 0 or not k:
            return [player, path]
        if k == "mpc":   # MPC-HC / MPC-BE : /start en millisecondes
            return [player, path, "/start", str(start_s * 1000)]
        if k == "vlc":
            return [player, f"--start-time={start_s}", path]
        if k == "mpv":
            return [player, f"--start={start_s}", path]
        if k == "pot":
            return [player, path, f"/seek={fmt_timecode(start_s)}"]
        return [player, path]

    def open_file(path, player="", start_s=0):
        if not path: return
        player = resolve_player(player, path) or player
        try:
            if player and Path(player).exists():
                subprocess.Popen(player_cmd(player, path, start_s))
            elif sys.platform == "win32":
                # os.startfile gère mieux les chemins UNC que cmd /c start
                os.startfile(path)
            else:
                subprocess.Popen(["xdg-open", path])
        except Exception as e:
            ui(lambda e=e, p=path: modal_err(
                "Erreur ouverture",
                f"Chemin tente :\n{p}\n\n"
                f"Erreur : {e}\n\n"
                f"Lecteur configure : {player or '(defaut systeme)'}"))


    def open_folder(path):
        if not path: return
        try:
            if sys.platform == "win32":
                # Chaîne (pas liste) : list2cmdline entourerait l'argument entier de guillemets
                # ce qui ferait échouer /select, sur les chemins avec espaces ou accents.
                # Ici on contrôle le quoting : /select, reste nu, seul le chemin est guillemété.
                subprocess.Popen(f'explorer /select,"{path}"')
            else:
                subprocess.Popen(["xdg-open", str(Path(path).parent)])
        except Exception as e:
            ui(lambda e=e, p=path: modal_err(
                "Erreur dossier",
                f"Chemin tente :\n{p}\n\nErreur : {e}"))


    def open_files_tiled(paths, player, start_s=0):
        """Ouvre plusieurs fichiers et dispose les fenêtres en mosaïque (Windows).
        2 fichiers -> côte à côte plein écran ; 3 -> en ligne ; 4+ -> grille carrée.
        Hors Windows ou sans lecteur défini -> ouverture simple (empilée)."""
        paths = [p for p in paths if p]
        if not paths:
            return
        player = resolve_player(player, paths[0]) or player
        if sys.platform != "win32" or not (player and Path(player).exists()):
            for p in paths:
                open_file(p, player, start_s)
            return

        def worker():
            import ctypes, math
            from ctypes import wintypes
            user32   = ctypes.windll.user32
            kernel32 = ctypes.windll.kernel32

            WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
            user32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
            user32.IsWindowVisible.argtypes = [wintypes.HWND]
            user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
            user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
            user32.GetWindowThreadProcessId.restype = wintypes.DWORD
            user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
            user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND,
                                            ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                            ctypes.c_int, ctypes.c_uint]
            user32.SystemParametersInfoW.argtypes = [ctypes.c_uint, ctypes.c_uint,
                                                     ctypes.c_void_p, ctypes.c_uint]
            kernel32.OpenProcess.restype = wintypes.HANDLE
            kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                                            wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
            kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

            target = Path(player).name.lower()

            def list_player_windows():
                found = []
                def cb(hwnd, lparam):
                    if not user32.IsWindowVisible(hwnd):              return True
                    if user32.GetWindowTextLengthW(hwnd) == 0:        return True
                    pid = wintypes.DWORD()
                    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                    h = kernel32.OpenProcess(0x1000, False, pid.value)  # QUERY_LIMITED_INFORMATION
                    if h:
                        try:
                            buf = ctypes.create_unicode_buffer(512)
                            size = wintypes.DWORD(512)
                            if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
                                if Path(buf.value).name.lower() == target:
                                    found.append(hwnd)
                        finally:
                            kernel32.CloseHandle(h)
                    return True
                user32.EnumWindows(WNDENUMPROC(cb), 0)
                return set(found)

            # 1. Fenêtres du lecteur déjà ouvertes (pour ne pas y toucher)
            try: before = list_player_windows()
            except Exception: before = set()

            # 2. Lancer chaque fichier (léger décalage = fenêtres distinctes)
            for p in paths:
                try:
                    subprocess.Popen(player_cmd(player, p, start_s))
                except Exception as e:
                    ui(lambda e=e, pp=p: modal_err("Erreur ouverture", f"{pp}\n\n{e}"))
                time.sleep(0.25)

            # 3. Disposer les nouvelles fenêtres en mosaïque (best effort)
            try:
                n = len(paths)
                new = []
                for _ in range(60):  # ~6 s max d'attente
                    new = list(list_player_windows() - before)
                    if len(new) >= n:
                        break
                    time.sleep(0.1)
                if not new:
                    return
                new.sort(key=lambda h: ctypes.cast(h, ctypes.c_void_p).value or 0)
                new = new[:n]

                class RECT(ctypes.Structure):
                    _fields_ = [("left",ctypes.c_long),("top",ctypes.c_long),
                                ("right",ctypes.c_long),("bottom",ctypes.c_long)]
                wa = RECT()
                user32.SystemParametersInfoW(0x0030, 0, ctypes.byref(wa), 0)  # SPI_GETWORKAREA
                ax, ay = wa.left, wa.top
                aw, ah = wa.right - wa.left, wa.bottom - wa.top

                cnt = len(new)
                if cnt <= 3:
                    cols, rows = cnt, 1
                else:
                    cols = math.ceil(math.sqrt(cnt))
                    rows = math.ceil(cnt / cols)
                cw = aw // cols
                ch = ah // rows
                SWP_SHOWWINDOW, SWP_NOZORDER, SW_RESTORE = 0x0040, 0x0004, 9
                for i, hwnd in enumerate(new):
                    col, row = i % cols, i // cols
                    x, y = ax + col*cw, ay + row*ch
                    user32.ShowWindow(hwnd, SW_RESTORE)           # dé-maximiser si besoin
                    user32.SetWindowPos(hwnd, 0, x, y, cw, ch,
                                        SWP_SHOWWINDOW | SWP_NOZORDER)
            except Exception:
                pass  # les fenêtres restent ouvertes (empilées) si la mosaïque échoue

        threading.Thread(target=worker, daemon=True).start()


    # Registre des tooltips traduisibles : [(text_tag, lang_key, wrap), ...]
    _TIPS_REGISTRY = []
    _tip_counter = 0

    def tip(text, wrap=320, en=None):
        """Tooltip statique. Si 'en' est fourni, choisit FR/EN selon la langue courante."""
        with dpg.tooltip(dpg.last_item()):
            msg = en if (en is not None and G.get("lang", "fr") == "en") else text
            dpg.add_text(msg, wrap=wrap)

    def tip_t(lang_key, wrap=320):
        """Tooltip traduisible - tague le texte et l'enregistre pour apply_lang()."""
        nonlocal _tip_counter
        _tip_counter += 1
        txt_tag = f"dbl_tip_txt_{_tip_counter}"
        with dpg.tooltip(dpg.last_item()):
            dpg.add_text(t(lang_key), tag=txt_tag, wrap=wrap)
        _TIPS_REGISTRY.append((txt_tag, lang_key, wrap))


    # ══════════════════════════════════════════════════════════════
    #  ÉTAT GLOBAL
    # ══════════════════════════════════════════════════════════════
    CFG = load_config()
    G = {
        "dupes":{}, "multiqual":{}, "ignored":load_ignored(),
        "nas_prefix": CFG["emby"].get("nas_prefix","/volume1"),
        "nas_unc":    CFG["emby"].get("nas_unc",""),
        "player":     CFG["emby"].get("player",""),
        "filter":"",  "sort":"title_asc",  "min_score": 0,  "lang":"fr",
        "filter_res":"", "filter_codec":"",  # filtres résolution / codec
        "same_dur":False, "same_size":False,  # filtres durée/taille identiques
        "emby_url":"", "api_key":"",         # stockés au connect pour delete
        "fuzzy_capped": 0,                   # nb films ignorés par le plafond fuzzy
        # Critères d'exclusion (True = actif = ce critère rend le groupe intentionnel)
        "criteria": {"resolution":True,"hdr":True,"av1":True,
                     "3d":True,"remaster":True,"cut":True,"bonus":True},
        # Médiathèques disponibles et sélectionnées
        "libraries": [],        # [{"id":..,"name":..,"type":..}]
        "lib_selected": set(),  # ids sélectionnés pour le scan
    }
    _mid = 0
    _render_timer = 0.0   # debounce pour slider/filtre
    _RENDER_DELAY = 0.30  # secondes d'attente apres dernier changement
    _meta_cache: dict = {}          # cache get_rich_metadata keyed sur movie Id
    _cancel_scan = threading.Event()# flag annulation scan en cours
    _current_page = 0               # page courante résultats
    _PAGE_SIZE = 50                 # groupes affichés par page

    # ══════════════════════════════════════════════════════════════
    #  MODALES  (toujours appelées depuis le thread principal)
    # ══════════════════════════════════════════════════════════════
    def _set_status(msg, color=(136,136,170)):
        """Met à jour le bandeau de statut (lbl_scan_info) avec une couleur."""
        try: dpg.configure_item("dbl_lbl_scan_info", default_value=msg, color=color)
        except Exception: pass

    def modal_err(title, msg):
        nonlocal _mid; _mid+=1; tag=f"dbl_err{_mid}"
        with dpg.window(label=title,tag=tag,modal=True,width=520,autosize=True,
                        pos=[160,200],no_resize=True):
            dpg.add_text(msg,wrap=500)
            dpg.add_separator()
            dpg.add_button(label="OK",width=-1,user_data=tag,callback=lambda s,a,u:dpg.delete_item(u))

    def modal_info(title, msg):
        nonlocal _mid; _mid+=1; tag=f"dbl_inf{_mid}"
        with dpg.window(label=title,tag=tag,modal=True,width=520,autosize=True,
                        pos=[160,200],no_resize=True):
            dpg.add_text(msg,wrap=500)
            dpg.add_separator()
            dpg.add_button(label="OK",width=-1,user_data=tag,callback=lambda s,a,u:dpg.delete_item(u))

    # ids en cours de suppression -> (titre, debut) : anti double-clic +
    # sablier anime / chrono affiches par _tick() tant que la liste n'est pas vide
    _deleting = {}

    def modal_confirm_delete(item_id, item_name, group_key):
        """Modale de confirmation avant suppression via l'API Emby."""
        nonlocal _mid; _mid+=1; tag=f"dbl_del{_mid}"
        url = G.get("emby_url",""); key = G.get("api_key","")
        if not url or not key:
            modal_err("Suppression impossible",
                      "Connectez-vous au serveur Emby avant de supprimer.\n"
                      "(Charger un scan sans se connecter ne suffit pas.)")
            return
        with dpg.window(label=t("delete_confirm"),tag=tag,modal=True,
                        width=540,autosize=True,pos=[180,200],no_resize=True):
            dpg.add_text(f"{item_name}", wrap=520, color=(255,180,60))
            dpg.add_text(t("del_warning"), color=(233,69,96))
            dpg.add_separator()
            dpg.add_text("Choisissez l'action :", color=(136,136,170))
            dpg.add_spacer(height=6)
            with dpg.group(horizontal=True):
                def _do_delete(delete_file, _t=tag, _id=item_id, _key=group_key, _nm=item_name):
                    dpg.delete_item(_t)
                    if _id in _deleting:
                        _set_status(f"Suppression déjà en cours : {_nm}", (240,160,0))
                        return
                    _deleting[_id] = (_nm, time.time())
                    _set_status(f"Suppression en cours (jusqu'à {DELETE_TIMEOUT} s) : {_nm}",
                                (240,160,0))
                    def _prog(left, _nm=_nm):
                        ui(lambda l=left: _set_status(
                            f"Emby lent - vérification de la suppression ({l} s) : {_nm}",
                            (240,160,0)))
                    def thread():
                        try:
                            emby_delete(url, key, _id, delete_file=delete_file,
                                        progress=_prog)
                            def on_ok():
                                # Retirer l'item du groupe dans G["dupes"]
                                grp = G["dupes"].get(_key, [])
                                G["dupes"][_key] = [it for it in grp
                                                    if it.get("Id","") != _id]
                                if len(G["dupes"][_key]) <= 1:
                                    G["dupes"].pop(_key, None)
                                _meta_cache.pop(_id, None)
                                # Feedback via le bandeau de statut (pas de modale empilée)
                                msg = "Fichier supprimé" if delete_file else "Retiré de la librairie"
                                _set_status(f"OK - {msg} : {_nm}", (46,204,113))
                                render_results()
                            ui(on_ok)
                        except urllib.error.HTTPError as e:
                            if e.code == 401:
                                m = "ERREUR - Non autorisé (401), la clé API n'a pas les droits de suppression."
                            elif e.code == 404:
                                m = "ERREUR - Introuvable (404), l'élément a peut-être déjà été supprimé."
                            else:
                                m = f"ERREUR - HTTP {e.code}: {e.reason}"
                            ui(lambda m=m: _set_status(m, (233,69,96)))
                        except TimeoutError as e:
                            ui(lambda m=str(e): (
                                _set_status("ERREUR suppression - délai dépassé", (233,69,96)),
                                modal_err("Suppression : délai dépassé", m)))
                        except Exception as e:
                            ui(lambda m=str(e): _set_status(f"ERREUR suppression - {m}", (233,69,96)))
                        finally:
                            _deleting.pop(_id, None)
                    threading.Thread(target=thread, daemon=True).start()
                dpg.add_button(label=t("del_emby_only"), width=230,
                    callback=lambda s,a,u: _do_delete(False))
                dpg.add_spacer(width=10)
                dpg.add_button(label=t("del_emby_file"), width=230,
                    callback=lambda s,a,u: _do_delete(True))
            dpg.add_separator()
            dpg.add_button(label=t("cancel"), width=-1,
                user_data=tag,
                callback=lambda s,a,u: dpg.delete_item(u))

    def get_start_timecode():
        """Timecode du champ 'Départ' (barre de l'onglet). (secondes, texte)."""
        try:
            txt = (dpg.get_value("dbl_inp_tc") or "").strip()
        except Exception:
            txt = G.get("last_timecode", "")
        return parse_timecode(txt), txt

    def on_timecode_change(s=None, v=None, u=None):
        txt = (v or "").strip()
        sec = parse_timecode(txt)
        try:
            if sec is None:
                dpg.set_value("dbl_lbl_tc_state", "?")
                dpg.configure_item("dbl_lbl_tc_state", color=(233,69,96))
                return
            dpg.set_value("dbl_lbl_tc_state", fmt_timecode(sec) if sec else
                          ("début" if G.get("lang","fr") != "en" else "start"))
            dpg.configure_item("dbl_lbl_tc_state", color=(136,136,170))
        except Exception:
            pass
        G["last_timecode"] = txt
        save_last_timecode(txt)

    def open_all_now(paths):
        """'Ouvrir tout' : ouvre directement, au timecode réglé en amont."""
        paths = [p for p in paths if p]
        if not paths:
            return
        en = G.get("lang", "fr") == "en"
        L = (lambda fr, e: e if en else fr)
        sec, txt = get_start_timecode()
        if sec is None:
            _set_status(L(f"Timecode de départ invalide : {txt!r} (ex : 1:02:30, 45:00, 1h05m, 45)",
                          f"Invalid start timecode: {txt!r} (e.g. 1:02:30, 45:00, 1h05m, 45)"),
                        (233,69,96))
            return
        player = resolve_player(get_player(), paths[0])
        open_files_tiled(paths, player or get_player(), sec)
        name = Path(player).name if player else L("lecteur système", "system player")
        if sec and not player_kind(player):
            _set_status(L(f"Lecteur non reconnu ({name}) : ouverture SANS timecode. "
                          "Indiquez le chemin de mpc-hc64.exe / vlc.exe dans le bandeau du haut.",
                          f"Unknown player ({name}): opening WITHOUT timecode. "
                          "Set the path of mpc-hc64.exe / vlc.exe in the top bar."),
                        (240,160,0))
        else:
            _set_status(L(f"Ouverture de {len(paths)} fichier(s) avec {name}"
                          + (f" à {fmt_timecode(sec)}" if sec else " depuis le début"),
                          f"Opening {len(paths)} file(s) with {name}"
                          + (f" at {fmt_timecode(sec)}" if sec else " from the start")),
                        (46,204,113))

    def show_ignored_panel():
        """Panneau listant les groupes ignorés avec bouton Retirer individuel."""
        nonlocal _mid; _mid+=1; tag=f"dbl_ign{_mid}"
        ign = G["ignored"]
        h = min(80 + len(ign)*32, 600)
        with dpg.window(label=t("ignored_panel"),tag=tag,modal=True,
                        width=560,height=h,pos=[180,120]):
            if not ign:
                dpg.add_text("No ignored group." if G["lang"]=="en" else "Aucun groupe ignoré.", color=(136,136,170))
            else:
                for key in sorted(ign):
                    with dpg.group(horizontal=True):
                        disp = key[:60]+"..." if len(key)>60 else key
                        dpg.add_text(disp, color=(200,200,200))
                        dpg.add_spacer(width=8)
                        dpg.add_button(label=t("remove_ignore"), width=65,
                            user_data=(key, tag),
                            callback=lambda s,a,u: _remove_one_ignore(u[0], u[1]))
            dpg.add_separator()
            dpg.add_button(label=t("close"), width=-1,
                user_data=tag,
                callback=lambda s,a,u: dpg.delete_item(u))

    def _remove_one_ignore(key, panel_tag):
        G["ignored"].discard(key)
        save_ignored(G["ignored"])
        dpg.delete_item(panel_tag)
        render_results()

    def compare_audio_popup(items):
        """Popup comparant les pistes audio de toutes les versions côte à côte."""
        nonlocal _mid; _mid+=1; tag=f"dbl_auc{_mid}"
        first=items[0]
        metas=[get_rich_metadata(it) for it in items]
        tracks_per_file=[md.get("audio_tracks",[]) for md in metas]
        max_tracks=max((len(tr) for tr in tracks_per_file), default=0)
        w=min(300+220*len(items),1500)
        title_str=f"{first.get('Name','?')} ({first.get('ProductionYear','')})"

        def fmt_track(tr):
            s=f"{tr['lang']} {tr['codec']} {tr['channels']}ch"
            if tr['title']: s+=f"  ({tr['title'][:28]})"
            return s

        with dpg.window(label=f"Comparaison audio - {title_str}", tag=tag,
                        modal=True, width=w, height=600, pos=[50,70]):
            dpg.add_text(("Audio tracks of each version, side by side. "
                          "Differing rows are highlighted."
                          if G["lang"]=="en" else
                          "Pistes audio de chaque version, côte à côte. "
                          "Les lignes qui diffèrent sont surlignées."),
                         color=(136,136,170), wrap=w-40)
            dpg.add_separator()
            with dpg.table(header_row=True, borders_outerH=True, borders_outerV=True,
                           borders_innerV=True, borders_innerH=True,
                           policy=dpg.mvTable_SizingStretchProp):
                dpg.add_table_column(label="", width_fixed=True, init_width_or_weight=80)
                for ci in range(len(items)):
                    dpg.add_table_column(label=f"Fichier #{ci+1}", width_stretch=True)

                # Ligne nom de fichier
                with dpg.table_row():
                    dpg.add_text("Fichier", color=(136,136,170))
                    for it in items:
                        fn=_fname(it)
                        dpg.add_text(fn[:38]+"..." if len(fn)>38 else fn, color=(170,221,255))

                # Ligne résolution/codec vidéo (contexte)
                with dpg.table_row():
                    dpg.add_text("Video" if G["lang"]=="en" else "Vidéo", color=(136,136,170))
                    vvals=[f"{md['res_label']} {md['vcodec']}"+(" HDR" if md['hdr'] else "")
                           for md in metas]
                    vdiff=len(set(vvals))>1
                    for v in vvals:
                        dpg.add_text(v, color=(255,220,100) if vdiff else (224,224,224))

                # Ligne nombre de pistes
                with dpg.table_row():
                    dpg.add_text("Nb pistes", color=(136,136,170))
                    counts=[len(tr) for tr in tracks_per_file]
                    cdiff=len(set(counts))>1
                    for c in counts:
                        dpg.add_text(str(c), color=(255,220,100) if cdiff else (224,224,224))

                # Une ligne par index de piste
                for ti in range(max_tracks):
                    vals=[fmt_track(tr[ti]) if ti < len(tr) else "-"
                          for tr in tracks_per_file]
                    tdiff=len({v for v in vals if v != "-"})>1 or "-" in vals
                    with dpg.table_row():
                        dpg.add_text(f"Piste {ti+1}", color=(136,136,170))
                        for v in vals:
                            col=(255,220,100) if tdiff else (224,224,224)
                            if v=="-": col=(110,110,110)
                            dpg.add_text(v, color=col)

            dpg.add_separator()
            dpg.add_button(label=t("close"), width=-1,
                user_data=tag,
                callback=lambda s,a,u: dpg.delete_item(u))

    # ══════════════════════════════════════════════════════════════
    #  FILTRE / TRI
    # ══════════════════════════════════════════════════════════════
    def apply_filter_sort():
        ft=G["filter"].lower().strip(); sk=G["sort"]; mn=G["min_score"]
        ign=G["ignored"]; d=G["dupes"]; m=G["multiqual"]
        fr=G.get("filter_res",""); fc=G.get("filter_codec","")
        sd=G.get("same_dur",False); ss=G.get("same_size",False)

        _TIER_LBL = {4:"4K",3:"1080p",2:"720p",1:"SD"}

        def match(key,payload):
            items=payload[0] if isinstance(payload,tuple) else payload
            title=(items[0].get("Name","") if items else "").lower()
            sc,_,_=confidence_score(key)
            if sc < mn: return False
            if not (not ft or ft in title or ft in key.lower()): return False
            if sd or ss:
                durs=[]; szs=[]
                for it in items:
                    md=get_rich_metadata(it)
                    durs.append(md["duration_s"])
                    szs.append(md["size_bytes"] or get_api_size(it))
                if sd and not _pair_same(durs, tol=1): return False
                if ss and not _pair_same(szs, tol=0): return False
            if fr or fc:
                for item in items:
                    sig=get_quality_signature(item)
                    tier_lbl=_TIER_LBL.get(sig["res_tier"],"")
                    if fr and tier_lbl != fr: continue
                    if fc and sig["codec"] != fc: continue
                    return True
                return False
            return True

        d2={k:v for k,v in d.items() if match(k,v) and k not in ign}
        m2={k:v for k,v in m.items() if match(k,v)}

        def skey(kv,is_mq=False):
            key,payload=kv
            items=payload[0] if is_mq else payload
            title=(items[0].get("Name","") if items else "").lower()
            sc,_,_=confidence_score(key)
            sz=sum(get_api_size(it) for it in items)
            if sk=="title_asc":  return title
            if sk=="title_desc": return tuple(-ord(c) for c in title[:60])
            if sk=="size":       return -sz
            if sk=="conf_desc":  return -sc
            if sk=="conf_asc":   return sc
            return title

        return (dict(sorted(d2.items(),key=lambda kv:skey(kv,False))),
                dict(sorted(m2.items(),key=lambda kv:skey(kv,True))))

    # ══════════════════════════════════════════════════════════════
    #  RENDU RÉSULTATS  (thread principal uniquement)
    # ══════════════════════════════════════════════════════════════
    def render_results():
        nonlocal _current_page
        dpg.delete_item("dbl_results_area",children_only=True)
        d,m = apply_filter_sort()
        ng=len(d); nf=sum(len(v) for v in d.values()); nq=len(m); ign=len(G["ignored"])

        try: dpg.configure_item("dbl_lbl_ignored",default_value=t("ign_count")%ign)
        except Exception: pass

        # Avertissement fuzzy cappé
        capped = G.get("fuzzy_capped", 0)
        if capped:
            warn = t("fuzzy_warn").format(n=capped)
            dpg.add_text(warn, parent="dbl_results_area", color=(230,126,34), wrap=900)

        if not d and not m:
            dpg.add_text("No duplicate found." if G["lang"]=="en" else "Aucun doublon détecté.",parent="dbl_results_area",color=(46,204,113))
            return

        # Groupes masqués par le seuil de score
        hidden_by_score = sum(1 for k in G["dupes"]
                              if confidence_score(k)[0] < G["min_score"]
                              and k not in G["ignored"])
        if hidden_by_score:
            dpg.add_text(t("hidden_score").format(n=hidden_by_score),
                         parent="dbl_results_area", color=(136,136,170))

        # Stats globales
        _,_,gain_min,gain_max=compute_stats(d)
        with dpg.group(parent="dbl_results_area",horizontal=True):
            for val,lbl,col in [(str(ng),t("stat_groups"),(233,69,96)),
                                 (str(nf),t("stat_files"),(230,126,34)),
                                 (f"{fmt_size(gain_min)} ~ {fmt_size(gain_max)}",t("stat_recover"),(46,204,113)),
                                 (str(nq),t("stat_intent"),(136,136,170)),
                                 (str(ign),t("stat_ign"),(100,100,120))]:
                with dpg.group():
                    dpg.add_text(val,color=col)
                    dpg.add_text(lbl,color=(136,136,170))
                dpg.add_spacer(width=18)
        dpg.add_separator(parent="dbl_results_area")
        dpg.add_spacer(height=6,parent="dbl_results_area")

        # ── Pagination ─────────────────────────────────────────────
        d_keys = list(d.keys())
        total_pages = max(1, (len(d_keys) + _PAGE_SIZE - 1) // _PAGE_SIZE)
        _current_page = max(0, min(_current_page, total_pages - 1))
        page_start = _current_page * _PAGE_SIZE
        page_keys  = d_keys[page_start : page_start + _PAGE_SIZE]
        d_page = {k: d[k] for k in page_keys}

        if total_pages > 1:
            with dpg.group(parent="dbl_results_area", horizontal=True):
                dpg.add_button(label=t("prev_btn"), width=70,
                    enabled=(_current_page > 0),
                    callback=lambda s,a,u: _go_page(_current_page - 1))
                dpg.add_text(f"  Page {_current_page+1}/{total_pages}  ",
                             color=(136,136,170))
                dpg.add_button(label=t("next_btn"), width=70,
                    enabled=(_current_page < total_pages - 1),
                    callback=lambda s,a,u: _go_page(_current_page + 1))
            dpg.add_spacer(height=4, parent="dbl_results_area")

        if d:
            dpg.add_text(f"  {t('sec_dupes')}  -  {ng} {t('stat_groups')}  -  {nf} {t('stat_files')}",
                         parent="dbl_results_area",color=(233,69,96))
            dpg.add_spacer(height=4,parent="dbl_results_area")
            _render_table(d_page,False)
        else:
            dpg.add_text(t("no_real"),parent="dbl_results_area",color=(46,204,113))

        if m:
            dpg.add_spacer(height=14,parent="dbl_results_area")
            dpg.add_text(f"  {t('sec_intent')}  -  {nq} {t('stat_groups')}",
                         parent="dbl_results_area",color=(136,136,170))
            dpg.add_spacer(height=4,parent="dbl_results_area")
            _render_table(m,True)

    def _go_page(page):
        nonlocal _current_page
        _current_page = page
        render_results()


    def _render_table(groups, is_mq):
        prefix=G["nas_prefix"]; unc=G["nas_unc"]
        ttl_col = (255,160,60) if not is_mq else (140,180,255)  # couleur titre

        for g_idx,(key,payload) in enumerate(groups.items()):
            items=payload[0] if is_mq else payload
            reason=payload[1] if is_mq else ""
            if not items: continue
            first=items[0]
            title=first.get("Name","?"); year=first.get("ProductionYear","")
            imdb=first.get("ProviderIds",{}).get("Imdb","")
            sc,sl,sc_col=confidence_score(key)
            all_wp=[to_win(p,prefix,unc) or p for it in items for p in get_all_sources(it)]

            # ── Séparateur visuel entre groupes ──────────────────
            dpg.add_spacer(height=10, parent="dbl_results_area")

            # ── Bandeau titre du groupe ───────────────────────────
            with dpg.group(parent="dbl_results_area"):

                # Ligne titre principale
                with dpg.group(horizontal=True):
                    # Badge score coloré
                    dpg.add_text(f"[{sc}%]", color=sc_col)

                    # Titre en grand, coloré
                    dpg.add_text(f"  >> {title}",color=ttl_col)
                    if year:
                        dpg.add_text(f"({year})", color=(180,180,180))
                    if imdb:
                        dpg.add_text(f"[IMDB:{imdb}]", color=(100,200,100))
                    dpg.add_text(
                        f"-  {len(items)} fichiers" + (f"  .  {reason}" if reason else ""),
                        color=(180,180,180))

                    # Boutons alignés à droite
                    dpg.add_spacer(width=20)
                    dpg.add_text(f"[{sl} {sc}%]", color=sc_col)
                    dpg.add_spacer(width=10)
                    dpg.add_button(label=t("open_all"), width=80,
                        user_data=all_wp,
                        callback=lambda s,a,u: open_all_now(u))
                    tip("Ouvre tous les fichiers du groupe et dispose les fenetres\n"
                        "cote a cote (mosaique) automatiquement.\n"
                        "Toutes les fenetres demarrent au timecode du champ 'Depart'\n"
                        "(MPC-HC, MPC-BE, VLC, mpv, PotPlayer).\n\n"
                        "ATTENTION : votre lecteur video doit supporter\n"
                        "plusieurs instances simultanées (sessions multiples).\n"
                        "VLC : Preferences > Interface > decocher 'Une seule instance'.\n"
                        "MPC-BE : Options > Lecteur > 'Permettre plusieurs instances'.", wrap=360,
                        en="Opens all files in the group and arranges the windows\n"
                           "side by side (mosaic) automatically.\n"
                           "Every window starts at the timecode of the 'Start' field\n"
                           "(MPC-HC, MPC-BE, VLC, mpv, PotPlayer).\n\n"
                           "WARNING: your video player must support\n"
                           "multiple simultaneous instances (multi-session).\n"
                           "VLC: Preferences > Interface > uncheck 'Allow only one instance'.\n"
                           "MPC-BE: Options > Player > 'Allow multiple instances'.")
                    dpg.add_button(label=t("compare"), width=70,
                        user_data=items,
                        callback=lambda s,a,u: compare_popup(u))
                    tip("Affiche les metadonnees des fichiers cote a cote\n(resolution, codec, bitrate, pistes audio...)\nLes differences sont surlignees en jaune.",
                        en="Shows the files' metadata side by side\n(resolution, codec, bitrate, audio tracks...)\nDifferences are highlighted in yellow.")
                    dpg.add_button(label="Audio", width=60,
                        user_data=items,
                        callback=lambda s,a,u: compare_audio_popup(u))
                    tip("Compare les pistes audio de toutes les versions cote a cote\n(langue, codec, canaux, titre).\nLes lignes qui different sont surlignees.",
                        en="Compares the audio tracks of all versions side by side\n(language, codec, channels, title).\nDiffering rows are highlighted.")
                    dpg.add_button(label=t("ignore"), width=60,
                        user_data=(key,title),
                        callback=lambda s,a,u: do_ignore(u[0],u[1]))
                    tip("Marque ce groupe comme faux positif.\nIl n'apparaitra plus dans les resultats.\nRecuperez-le via 'Reinitialiser' les ignores.",
                        en="Marks this group as a false positive.\nIt won't appear in the results anymore.\nRestore it via 'Reset' ignored.")

                dpg.add_separator()

                # ── Sous-tableau fichiers ─────────────────────────
                with dpg.table(header_row=True, row_background=True,
                               borders_innerH=True, borders_outerH=True,
                               borders_innerV=True, borders_outerV=True,
                               policy=dpg.mvTable_SizingStretchProp):
                    dpg.add_table_column(label=t("col_file"),   width_stretch=True, init_width_or_weight=0.32)
                    dpg.add_table_column(label=t("col_qual"),   width_fixed=True,   init_width_or_weight=125)
                    dpg.add_table_column(label=t("col_folder"), width_stretch=True, init_width_or_weight=0.38)
                    dpg.add_table_column(label=t("col_dur"),    width_fixed=True,   init_width_or_weight=70)
                    dpg.add_table_column(label=t("col_size"),   width_fixed=True,   init_width_or_weight=68)
                    dpg.add_table_column(label=t("col_actions"),width_fixed=True,   init_width_or_weight=245)

                    # valeurs du groupe pour surligner durées/tailles identiques
                    _g_durs=[get_rich_metadata(it)["duration_s"] for it in items]
                    _g_szs =[(get_rich_metadata(it)["size_bytes"] or get_api_size(it)) for it in items]
                    for item in items:
                        sig=get_quality_signature(item)
                        qual=(f"{sig['res_label']} {sig['codec_raw']}"
                              + (" HDR" if sig["hdr"] else ""))
                        md  = get_rich_metadata(item)
                        _szv = md["size_bytes"] or get_api_size(item)
                        _dv  = md["duration_s"]
                        _dur_same = bool(_dv) and sum(1 for d in _g_durs if d and abs(d-_dv)<=1) >= 2
                        _sz_same  = bool(_szv) and sum(1 for z in _g_szs if z == _szv) >= 2
                        _dur_col  = (0,210,255) if _dur_same else (180,180,130)
                        _sz_col   = (255,200,80) if _sz_same else (220,220,220)
                        sz  = fmt_size(md["size_bytes"]) if md["size_bytes"] else fmt_size(get_api_size(item))
                        dur = fmt_duration(md["duration_s"])
                        item_id = item.get("Id","")
                        for path in get_all_sources(item):
                            wp=to_win(path,prefix,unc) or path
                            fname=Path(path).name if path else "-"
                            folder=str(Path(wp).parent) if wp else "-"
                            fd=folder if len(folder)<=55 else "..."+folder[-52:]
                            with dpg.table_row():
                                dpg.add_button(label=f"  {fname}", width=-1,
                                    user_data=wp,
                                    callback=lambda s,a,u: open_file(u, get_player()))
                                dpg.add_text(qual, color=(170,221,255))
                                dpg.add_text(fd)
                                dpg.add_text(dur, color=_dur_col)
                                dpg.add_text(sz, color=_sz_col)
                                with dpg.group(horizontal=True):
                                    dpg.add_button(label=t("play"), width=40,
                                        user_data=wp,
                                        callback=lambda s,a,u: open_file(u,get_player()))
                                    tip("Ouvre ce fichier avec le lecteur video configure.",
                                        en="Opens this file with the configured video player.")
                                    dpg.add_button(label=t("folder_btn"), width=55,
                                        user_data=wp,
                                        callback=lambda s,a,u: open_folder(u))
                                    tip("Ouvre l'explorateur Windows sur le dossier de ce fichier.",
                                        en="Opens Windows Explorer on this file's folder.")
                                    dpg.add_button(label=t("copy_path"), width=50,
                                        user_data=wp,
                                        callback=lambda s,a,u: dpg.set_clipboard_text(u))
                                    tip("Copie le chemin complet du fichier dans le presse-papiers.",
                                        en="Copies the file's full path to the clipboard.")
                                    if not is_mq and item_id:
                                        dpg.add_button(label=t("delete"), width=0,
                                            user_data=(item_id, fname, key),
                                            callback=lambda s,a,u: modal_confirm_delete(u[0],u[1],u[2]))
                                        tip("Supprime ce fichier via l'API Emby (avec confirmation).",
                                            en="Deletes this file via the Emby API (with confirmation).")


    # ══════════════════════════════════════════════════════════════
    #  POPUP COMPARAISON
    # ══════════════════════════════════════════════════════════════
    def compare_popup(items):
        nonlocal _mid; _mid+=1; tag=f"dbl_cmp{_mid}"
        first=items[0]
        prefix=G["nas_prefix"]; unc=G["nas_unc"]
        lx=[get_all_sources(it)[0] if get_all_sources(it) else "" for it in items]
        wp=[to_win(p,prefix,unc) or p for p in lx]
        metas=[get_rich_metadata(it) for it in items]
        w=min(430*len(items),1400)
        title_str=f"{first.get('Name','?')} ({first.get('ProductionYear','')})"

        with dpg.window(label=f"Comparaison - {title_str}",tag=tag,
                        modal=True,width=w,height=640,pos=[40,60]):

            def fmt_audio_tracks(md):
                tracks = md.get("audio_tracks",[])
                if not tracks: return "?"
                parts = []
                for track in tracks:
                    lang  = track["lang"] if track["lang"] != "?" else "?"
                    codec = track["codec"]
                    ch    = track["channels"]
                    label = f"{lang} {codec} {ch}ch"
                    if track["title"]: label += f" ({track['title'][:20]})"
                    parts.append(label)
                return " | ".join(parts)

            FIELDS=[
                ("Fichier",      lambda md,lp,wpp: Path(wpp).name if wpp else "-"),
                ("Resolution",   lambda md,lp,wpp: md["res_label"]),
                ("Codec video",  lambda md,lp,wpp: md["vcodec"]),
                ("HDR",          lambda md,lp,wpp: "Oui" if md["hdr"] else "Non"),
                ("Codec audio",  lambda md,lp,wpp: md["acodec"]),
                ("Canaux",       lambda md,lp,wpp: md["channels"]),
                ("Pistes audio", lambda md,lp,wpp: fmt_audio_tracks(md)),
                ("Bitrate",      lambda md,lp,wpp: f"{md['bitrate_kbps']} kbps" if md["bitrate_kbps"] else "?"),
                ("Duree",        lambda md,lp,wpp: fmt_duration(md["duration_s"])),
                ("Taille",       lambda md,lp,wpp: fmt_size(md["size_bytes"])),
                ("Ajoute le",    lambda md,lp,wpp: md["date_added"] or "?"),
                ("Dossier",      lambda md,lp,wpp: str(Path(wpp).parent) if wpp else "-"),
            ]
            with dpg.table(header_row=True,borders_outerH=True,
                           borders_innerV=True,borders_innerH=True,
                           policy=dpg.mvTable_SizingStretchProp):
                dpg.add_table_column(label="Champ",width_fixed=True,init_width_or_weight=100)
                for ci in range(len(items)):
                    dpg.add_table_column(label=f"Fichier #{ci+1}",width_stretch=True)
                for fname,fn in FIELDS:
                    vals=[fn(md,lp,wpp) for md,lp,wpp in zip(metas,lx,wp)]
                    diff=len(set(vals))>1
                    with dpg.table_row():
                        dpg.add_text(fname,color=(136,136,170))
                        for v in vals:
                            dpg.add_text(v,color=(255,220,100) if diff else (224,224,224))
            dpg.add_separator()
            with dpg.group(horizontal=True):
                dpg.add_button(label=t("open_both"), width=130,
                    user_data=wp,
                    callback=lambda s,a,u: open_all_now(u))
                tip("Ouvre tous les fichiers du groupe avec le lecteur configure.",
                    en="Opens all files in the group with the configured player.")
                dpg.add_spacer(width=10)
                for ci,wpp in enumerate(wp):
                    dpg.add_button(label=f"Lire #{ci+1}",width=90,
                        user_data=wpp,
                        callback=lambda s,a,u: open_file(u,get_player()))
                    tip("Ouvre ce fichier avec le lecteur video configure.",
                                        en="Opens this file with the configured video player.")
                    dpg.add_button(label=f"Dossier #{ci+1}",width=90,
                        user_data=wpp,
                        callback=lambda s,a,u: open_folder(u))
                    dpg.add_button(label=t("copy_path"), width=55,
                        user_data=wpp,
                        callback=lambda s,a,u: dpg.set_clipboard_text(u))
                    dpg.add_spacer(width=8)
            dpg.add_separator()
            dpg.add_button(label=t("close"),width=-1,
                user_data=tag,
                callback=lambda s,a,u: dpg.delete_item(u))

    # ══════════════════════════════════════════════════════════════
    #  ACTIONS
    # ══════════════════════════════════════════════════════════════

    def apply_lang():
        """Met a jour tous les widgets statiques taggues avec la langue courante."""
        L = LANGS[G["lang"]]
        try: dpg.configure_item("dbl_lbl_ignored", default_value=L["ign_count"]%len(G["ignored"]))
        except Exception: pass
        try: render_results()
        except Exception: pass
        # Boutons principaux
        for tag, key in [("dbl_btn_connect","connect"),("dbl_btn_scan","scan"),
                         ("dbl_btn_save","save"),("dbl_btn_load","load"),
                         ("dbl_btn_export","export"),("dbl_btn_reset_ign","reset_ign"),
                         ("dbl_btn_refresh","refresh_emby"),("dbl_btn_lang","lang_btn"),
                         ("dbl_btn_manage_ign","manage_ign"),
                         ("dbl_btn_chk_all","check_all"),("dbl_btn_chk_none","uncheck_all"),
                         ("dbl_hdr_criteria","criteria_hdr"),
                         ("dbl_chk_same_dur","flt_samedur"),("dbl_chk_same_size","flt_samesize")]:
            try: dpg.configure_item(tag, label=L[key])
            except Exception: pass
        # Labels texte
        for tag, key in [("dbl_lbl_tc","tc_lbl"),("dbl_lbl_url","url_lbl"),("dbl_lbl_apikey","apikey_lbl"),
                         ("dbl_t_appname","app_title"),
                         ("dbl_lbl_uid","userid_lbl"),("dbl_lbl_prefix","prefix_lbl"),
                         ("dbl_lbl_player","player_lbl"),("dbl_lbl_filter","filter_lbl"),
                         ("dbl_lbl_sort","sort_lbl"),("dbl_lbl_thresh","threshold_lbl"),
                         ("dbl_lbl_ign_txt","ignored_lbl"),("dbl_lbl_sub","sub"),
                         ("dbl_lbl_filter_res","filter_res"),("dbl_lbl_filter_codec","filter_codec")]:
            try: dpg.configure_item(tag, default_value=L[key])
            except Exception: pass
        # Hints champs texte
        for tag, key in [("dbl_inp_uid","userid_hint"),("dbl_inp_filter","filter_hint"),
                         ("dbl_inp_player","player_hint")]:
            try: dpg.configure_item(tag, hint=L[key])
            except Exception: pass
        # Combo tri
        opts = (L["sort_az"],L["sort_za"],L["sort_sz"],L["sort_cd"],L["sort_ca"])
        try:
            dpg.configure_item("dbl_cb_sort", items=opts, default_value=opts[0])
        except Exception: pass
        # Combos résolution / codec
        res_opts = (L["all_res"],"4K","1080p","720p","SD")
        cod_opts = (L["all_codec"],"H264","HEVC","AV1")
        try: dpg.configure_item("dbl_cb_filter_res",  items=res_opts, default_value=res_opts[0])
        except Exception: pass
        try: dpg.configure_item("dbl_cb_filter_codec",items=cod_opts, default_value=cod_opts[0])
        except Exception: pass
        # Criteres checkboxes
        for key in ["resolution","hdr","av1","3d","remaster","cut","bonus"]:
            try: dpg.configure_item(f"dbl_chk_{key}", label=L[f"dbl_cr_{key}"])
            except Exception: pass
        # Popup scan
        try: dpg.configure_item("dbl_scan_popup", label=L["scan_win"])
        except Exception: pass
        # Panel librairies hint
        try:
            kids = dpg.get_item_children("dbl_lib_panel", 1)
            if kids and not G["libraries"]:
                dpg.configure_item(kids[0], default_value=L["libs_hint"])
        except Exception: pass
        # Mettre a jour tous les tooltips enregistres
        for txt_tag, lang_key, wrap in _TIPS_REGISTRY:
            try: dpg.configure_item(txt_tag, default_value=t(lang_key))
            except Exception: pass
        # Reinitialiser render pour les textes dynamiques
        if G["dupes"] or G["multiqual"]:
            render_results()


    def toggle_lang():
        G["lang"] = "en" if G["lang"] == "fr" else "fr"
        apply_lang()

    def reclassify():
        """Re-répartit dupes/multiqual selon les critères courants, sans rescan.
        Fusionne les deux ensembles puis ré-applique is_intentional sur chaque groupe."""
        nonlocal _current_page
        all_groups = dict(G["dupes"])
        for k,(items,_reason) in G["multiqual"].items():
            all_groups[k] = items
        real, mq = {}, {}
        for k, items in all_groups.items():
            intl, reason = is_intentional(items)
            if intl: mq[k] = (items, reason)
            else:     real[k] = items
        G["dupes"] = real; G["multiqual"] = mq
        _current_page = 0
        render_results()

    def _set_criterion(key, val):
        G["criteria"][key] = val
        # Re-répartir immédiatement les groupes déjà chargés (sans rescan)
        if G["dupes"] or G["multiqual"]:
            reclassify()

    def do_ignore(key, title):
        G["ignored"].add(key); save_ignored(G["ignored"]); render_results()

    def do_reset_ignored():
        G["ignored"].clear(); save_ignored(G["ignored"]); render_results()

    def _schedule_render():
        """Demande un render dans _RENDER_DELAY secondes (debounce)."""
        nonlocal _render_timer
        _render_timer = time.time() + _RENDER_DELAY

    def on_filter(s,v,u):
        nonlocal _current_page
        G["filter"]=v; _current_page=0; _schedule_render()

    def on_score_threshold(s,v,u):
        nonlocal _current_page
        G["min_score"]=v; _current_page=0
        # Mettre à jour le label IMMÉDIATEMENT (pas besoin de render complet)
        try: dpg.configure_item("dbl_lbl_score_val", default_value=f"{v}%")
        except Exception: pass
        _schedule_render()

    def on_sort(s,v,u):
        nonlocal _current_page
        # Mapping multi-langue : toutes les valeurs possibles -> clé interne
        _MAP = {
            "Titre A>Z":"title_asc","Title A>Z":"title_asc",
            "Titre Z>A":"title_desc","Title Z>A":"title_desc",
            "Taille":"size","Size":"size",
            "Confiance v":"conf_desc","Confidence v":"conf_desc",
            "Confiance ^":"conf_asc","Confidence ^":"conf_asc",
        }
        G["sort"]=_MAP.get(v,"title_asc"); _current_page=0
        render_results()

    def on_filter_res(s,v,u):
        """Filtre par résolution - valeur combo → '' si 'Toutes'/'All'."""
        nonlocal _current_page
        all_lbl = t("all_res")
        G["filter_res"] = "" if v in (all_lbl,"Toutes","All") else v
        _current_page=0; render_results()

    def on_filter_codec(s,v,u):
        """Filtre par codec - valeur combo → '' si 'Tous'/'All'."""
        nonlocal _current_page
        all_lbl = t("all_codec")
        G["filter_codec"] = "" if v in (all_lbl,"Tous","All") else v
        _current_page=0; render_results()

    def on_same_dur(s,v,u):
        nonlocal _current_page
        G["same_dur"]=bool(v); _current_page=0; render_results()

    def on_same_size(s,v,u):
        nonlocal _current_page
        G["same_size"]=bool(v); _current_page=0; render_results()

    def browse_player():
        try:
            import tkinter as tk; from tkinter import filedialog
            r=tk.Tk(); r.withdraw()
            fp=filedialog.askopenfilename(title="Lecteur video",
                filetypes=[("Executables","*.exe"),("Tous","*.*")],
                initialdir=r"C:\Program Files")
            r.destroy()
            if fp:
                dpg.set_value("dbl_inp_player",fp); G["player"]=fp
        except Exception: pass

    def do_export():
        nonlocal _mid; _mid+=1
        win_tag = f"dbl_exp{_mid}"
        rb_tag  = f"dbl_rb{_mid}"   # tag UNIQUE pour eviter conflits entre ouvertures
        with dpg.window(label=t("export"),tag=win_tag,modal=True,
                        width=340,height=200,pos=[200,200],no_resize=True):
            dpg.add_text("Format :")
            # DPG radio_button retourne un ENTIER (0=HTML, 1=CSV) - pas une chaine
            dpg.add_radio_button(("HTML (rapport navigateur)","CSV (tableur)"),
                                 tag=rb_tag, default_value=0)
            dpg.add_separator()
            def go(wt=win_tag, rt=rb_tag):
                fmt_int = dpg.get_value(rt)   # 0 = HTML, 1 = CSV
                dpg.delete_item(wt)
                is_html = (fmt_int == 0)
                ext     = "html" if is_html else "csv"
                ts      = time.strftime("%Y%m%d_%H%M")
                defname = f"emby_doublons_{ts}.{ext}"
                try:
                    import tkinter as tk; from tkinter import filedialog
                    r=tk.Tk(); r.withdraw()
                    fp=filedialog.asksaveasfilename(
                        title=f"Exporter en {ext.upper()}",
                        initialfile=defname,
                        defaultextension=f".{ext}",
                        filetypes=[(f"Fichier {ext.upper()}",f"*.{ext}"),("Tous","*.*")])
                    r.destroy()
                    if not fp: return
                    prefix=G["nas_prefix"]; unc=G["nas_unc"]
                    if is_html:
                        export_html(G["dupes"],G["multiqual"],fp,prefix,unc)
                        import webbrowser; webbrowser.open(Path(fp).as_uri())
                    else:
                        export_csv(G["dupes"],G["multiqual"],fp,prefix,unc)
                    modal_info("Export OK",f"Fichier:\n{fp}")
                except Exception as e:
                    modal_err("Erreur export", str(e))
            dpg.add_button(label=t("export"),width=-1,callback=lambda s,a,u:go())

    def do_save():
        try:
            p=save_scan(G["dupes"],G["multiqual"],
                        dpg.get_value("dbl_inp_url").strip(),
                        G["nas_prefix"],G["nas_unc"])
            dpg.configure_item("dbl_lbl_scan_info",default_value=f"Sauvegarde {time.strftime('%d/%m/%Y %H:%M')}")
            modal_info("Sauvegarde",f"Fichier:\n{p}")
        except Exception as e: modal_err("Erreur",str(e))

    def do_load():
        def thread():
            try:
                dupes,mq,meta=load_scan()
                def on_done(d=dupes,m=mq,mt=meta):
                    nonlocal _current_page
                    G["dupes"]=d; G["multiqual"]=m
                    _meta_cache.clear(); _current_page=0
                    if mt["nas_prefix"]:
                        G["nas_prefix"]=mt["nas_prefix"]
                        dpg.set_value("dbl_inp_prefix",mt["nas_prefix"])
                    if mt["nas_unc"]:
                        G["nas_unc"]=mt["nas_unc"]
                        dpg.set_value("dbl_inp_unc",mt["nas_unc"])
                    dpg.configure_item("dbl_lbl_scan_info",default_value=f"Charge {mt['saved_at']}")
                    dpg.configure_item("dbl_btn_save",enabled=True)
                    reclassify()   # ré-applique les critères courants (et le code à jour) au scan chargé
                ui(on_done)
            except FileNotFoundError:
                ui(lambda: modal_err("Aucun scan",f"Fichier attendu:\n{SCAN_FILE}"))
            except Exception as e:
                ui(lambda m=str(e): modal_err("Erreur",m))
        threading.Thread(target=thread,daemon=True).start()

    # ══════════════════════════════════════════════════════════════
    #  SCAN (thread → queue → UI)
    # ══════════════════════════════════════════════════════════════
    def _get_params():
        return {
            "url":    dpg.get_value("dbl_inp_url").strip().rstrip("/"),
            "key":    dpg.get_value("dbl_inp_key").strip(),
            "uid":    dpg.get_value("dbl_inp_uid").strip(),
            "prefix": dpg.get_value("dbl_inp_prefix").strip(),
            "unc":    dpg.get_value("dbl_inp_unc").strip(),
            "player": dpg.get_value("dbl_inp_player").strip(),
        }

    def do_refresh_emby():
        """Déclenche l'analyse des médiathèques SÉLECTIONNÉES sur le serveur Emby."""
        p = _get_params()
        if not p["url"] or not p["key"]:
            modal_err("Missing parameters" if G["lang"]=="en" else "Parametres manquants",
                      "Emby URL and API key are required." if G["lang"]=="en"
                      else "L'URL et la cle API sont obligatoires.")
            return
        # Ne concerne QUE les médiathèques cochées ; au moins une est requise.
        if not G["lib_selected"]:
            modal_err(t("need_lib_title"), t("need_lib_msg"))
            return
        lib_ids = list(G["lib_selected"])
        dpg.configure_item("dbl_btn_refresh", enabled=False)
        _set_status(("Emby scan requested for %d selected library(ies)..." % len(lib_ids))
                    if G["lang"]=="en" else
                    ("Analyse demandée pour %d médiathèque(s) sélectionnée(s)..." % len(lib_ids)),
                    (240,160,0))
        def thread(lib_ids=lib_ids):
            ok = 0; last_err = None
            for lid in lib_ids:
                try:
                    emby_refresh_item(p["url"], p["key"], lid)
                    ok += 1
                except urllib.error.HTTPError as e:
                    last_err = "Non autorisé (401)." if e.code==401 else f"HTTP {e.code}: {e.reason}"
                except Exception as e:
                    last_err = str(e)
            def done(o=ok, n=len(lib_ids), err=last_err):
                if o == n:
                    _set_status(("OK - Emby is scanning %d selected library(ies). "
                                 "Wait, then re-run a scan." % n)
                                if G["lang"]=="en" else
                                ("OK - Emby analyse %d médiathèque(s) sélectionnée(s). "
                                 "Patientez, puis relancez un scan." % n),
                                (46,204,113))
                else:
                    _set_status((f"Scan: {o}/{n} OK - last error: {err}")
                                if G["lang"]=="en" else
                                (f"Analyse : {o}/{n} OK - dernière erreur : {err}"),
                                (233,69,96))
                dpg.configure_item("dbl_btn_refresh", enabled=True)
            ui(done)
        threading.Thread(target=thread, daemon=True).start()


    def do_connect():
        """Vérifie la connexion et charge les médiathèques disponibles."""
        p = _get_params()
        if not p["url"] or not p["key"]:
            modal_err("Parametres manquants","L'URL et la cle API sont obligatoires.")
            return

        dpg.configure_item("dbl_btn_connect", enabled=False)

        def thread():
            try:
                # Test connexion
                emby_get(p["url"],p["key"],"/System/Info/Public")
                # Récupérer les médiathèques
                libs_raw = emby_get(p["url"],p["key"],"/Library/VirtualFolders")
                libs = []
                for lib in libs_raw:
                    lib_id   = lib.get("ItemId","") or lib.get("Id","")
                    lib_name = lib.get("Name","?")
                    lib_type = lib.get("CollectionType","") or ""
                    if lib_id:
                        libs.append({"id":lib_id,"name":lib_name,"type":lib_type})

                def on_done(libs=libs):
                    G["libraries"] = libs
                    G["emby_url"]  = p["url"]   # stocké pour la fonction delete
                    G["api_key"]   = p["key"]
                    # Aucune sélection par défaut : l'utilisateur coche les médiathèques à scanner
                    G["lib_selected"] = set()
                    _rebuild_library_panel(libs)
                    dpg.configure_item("dbl_btn_connect", enabled=True)
                    dpg.configure_item("dbl_btn_scan",enabled=True)
                    dpg.configure_item("dbl_lbl_scan_info",
                        default_value=f"{len(libs)} mediatheque(s) trouvee(s)")
                ui(on_done)

            except urllib.error.HTTPError as e:
                msg = "Cle API invalide (401)." if e.code==401 else f"HTTP {e.code}: {e.reason}"
                ui(lambda m=msg: (modal_err("Erreur connexion",m),
                                  dpg.configure_item("dbl_btn_connect", enabled=True)))
            except Exception as e:
                ui(lambda m=str(e): (modal_err("Erreur connexion",m),
                                     dpg.configure_item("dbl_btn_connect", enabled=True)))

        threading.Thread(target=thread,daemon=True).start()


    def _rebuild_library_panel(libs):
        """Affiche les mediatheques en grille de checkboxes cliquables."""
        dpg.delete_item("dbl_lib_panel", children_only=True)

        if not libs:
            dpg.add_text("Aucune mediatheque trouvee.", parent="dbl_lib_panel",
                         color=(200,80,80))
            return

        # Boutons tout cocher / tout décocher
        with dpg.group(horizontal=True, parent="dbl_lib_panel"):
            dpg.add_text("Mediatheques :", color=(136,136,170))
            dpg.add_spacer(width=8)
            dpg.add_button(label=t("check_all"), tag="dbl_btn_chk_all", width=90,
                user_data=libs,
                callback=lambda s,a,u: _select_all_libs(u, True))
            dpg.add_button(label=t("uncheck_all"), tag="dbl_btn_chk_none", width=100,
                user_data=libs,
                callback=lambda s,a,u: _select_all_libs(u, False))
        dpg.add_spacer(height=4, parent="dbl_lib_panel")

        # Grille 4 colonnes dans un tableau DPG
        COLS = 4
        with dpg.table(parent="dbl_lib_panel", header_row=False,
                       policy=dpg.mvTable_SizingStretchSame):
            for _ in range(COLS):
                dpg.add_table_column()
            for i in range(0, len(libs), COLS):
                with dpg.table_row():
                    batch = libs[i:i+COLS]
                    for lib in batch:
                        icon = {"movies":"Films","tvshows":"Series","music":"Musique",
                                "books":"Livres","photos":"Photos",
                                "boxsets":"","mixed":"","homevideos":"Videos"}.get(
                                lib["type"], lib["type"] or "")
                        dpg.add_checkbox(
                            label=f"{lib['name']}  [{icon}]",
                            tag=f"dbl_chk_lib_{lib['id']}",
                            default_value=False,
                            user_data=lib["id"],
                            callback=lambda s,v,u: _toggle_lib(u, v))
                    # Remplir cellules vides si rang incomplet
                    for _ in range(COLS - len(batch)):
                        dpg.add_text("")


    def _select_all_libs(libs, checked):
        """Coche ou décoche toutes les mediatheques."""
        for lib in libs:
            try: dpg.set_value(f"dbl_chk_lib_{lib['id']}", checked)
            except Exception: pass
            _toggle_lib(lib["id"], checked)


    def _toggle_lib(lib_id, checked):
        if checked: G["lib_selected"].add(lib_id)
        else:       G["lib_selected"].discard(lib_id)



    def start_scan():
        p = _get_params()
        if not p["url"] or not p["key"]:
            modal_err("Parametres manquants","L'URL et la cle API sont obligatoires.")
            return

        G["nas_prefix"]=p["prefix"]; G["nas_unc"]=p["unc"]; G["player"]=p["player"]
        save_config({"url":p["url"],"api_key":p["key"],"user_id":p["uid"],
                     "nas_prefix":p["prefix"],"nas_unc":p["unc"],"player":p["player"]})

        # Le scan ne concerne QUE les médiathèques cochées ; au moins une est requise.
        if not G["lib_selected"]:
            modal_err("Sélection requise",
                      "Nécessite la sélection d'au moins une médiathèque.")
            return
        parent_ids = list(G["lib_selected"])

        dpg.configure_item("dbl_btn_scan",enabled=False,label="Scan...")
        dpg.configure_item("dbl_scan_popup",show=True)
        dpg.configure_item("dbl_btn_cancel_scan",enabled=True)
        dpg.set_value("dbl_scan_step","Connexion au serveur...")
        dpg.set_value("dbl_scan_pb",0.0)
        _cancel_scan.clear()   # réinitialiser le flag d'annulation
        _meta_cache.clear()    # invalider le cache pour un scan propre

        def thread():
            def set_step(msg,pct):
                ui(lambda m=msg,pp=pct:(
                    dpg.set_value("dbl_scan_step",m),
                    dpg.set_value("dbl_scan_pb",pp)))

            try:
                set_step(f"Connexion a {p['url']}...",0.02)
                emby_get(p["url"],p["key"],"/System/Info/Public")

                scope_msg = (f"{len(parent_ids)} mediatheque(s)"
                             if parent_ids else "toutes les mediatheques")
                set_step(f"OK - recuperation films ({scope_msg})...",0.05)
                t0=time.time()

                def on_page(fetched,total,page):
                    pct=0.05+0.55*(fetched/max(total,1))
                    el=time.time()-t0; rate=fetched/el if el>0 else 0
                    eta=(total-fetched)/rate if rate>0 else 0
                    msg=(f"Page {page} - {fetched} films ({rate:.0f}/s)"
                         +(f" ~{eta:.0f}s" if eta>2 else ""))
                    set_step(msg,pct)

                movies=fetch_movies(p["url"],p["key"],p["uid"],on_page,parent_ids)
                set_step(f"{len(movies)} films - analyse...",0.62)

                def on_step(idx,total,title):
                    set_step(f"{idx}/{total} - {title}",0.62+0.35*(idx/max(total,1)))

                dupes,multiqual=find_duplicates(movies,on_step)

                if _cancel_scan.is_set():
                    def on_cancel():
                        dpg.configure_item("dbl_scan_popup",show=False)
                        dpg.configure_item("dbl_btn_scan",enabled=True,label=t("scan"))
                        dpg.configure_item("dbl_btn_cancel_scan",enabled=False)
                        dpg.configure_item("dbl_lbl_scan_info",default_value=t("scan_cancelled"))
                    ui(on_cancel); return

                set_step("Sauvegarde...",0.98)

                try:
                    save_scan(dupes,multiqual,p["url"],p["prefix"],p["unc"])
                    saved_msg=f"Sauvegarde {time.strftime('%d/%m/%Y %H:%M')}"
                except Exception as e:
                    saved_msg=f"Erreur sauvegarde: {e}"

                def finish(d=dupes,mq=multiqual,sm=saved_msg):
                    nonlocal _current_page
                    G["dupes"]=d; G["multiqual"]=mq; _current_page=0
                    dpg.set_value("dbl_scan_pb",1.0)
                    dpg.configure_item("dbl_scan_popup",show=False)
                    dpg.configure_item("dbl_btn_scan",enabled=True,label=t("scan"))
                    dpg.configure_item("dbl_btn_cancel_scan",enabled=False)
                    dpg.configure_item("dbl_btn_save",enabled=True)
                    dpg.configure_item("dbl_lbl_scan_info",default_value=sm)
                    render_results()
                ui(finish)

            except urllib.error.HTTPError as e:
                msg="Cle API invalide (401)." if e.code==401 else f"HTTP {e.code}: {e.reason}"
                ui(lambda m=msg: (modal_err("Erreur API",m),
                    dpg.configure_item("dbl_scan_popup",show=False),
                    dpg.configure_item("dbl_btn_scan",enabled=True,label=t("scan")),
                    dpg.configure_item("dbl_btn_cancel_scan",enabled=False)))
            except Exception as e:
                msg=str(e)
                ui(lambda m=msg: (modal_err("Erreur scan",m),
                    dpg.configure_item("dbl_scan_popup",show=False),
                    dpg.configure_item("dbl_btn_scan",enabled=True,label=t("scan")),
                    dpg.configure_item("dbl_btn_cancel_scan",enabled=False)))

        threading.Thread(target=thread,daemon=True).start()

    # ══════════════════════════════════════════════════════════════
    #  THEME
    # ══════════════════════════════════════════════════════════════

    def _build_popups():
        # Popup scan
        with dpg.window(label="Scan en cours",tag="dbl_scan_popup",modal=True,
                        show=False,width=580,height=190,pos=[200,220],
                        no_close=True,no_resize=True):
            dpg.add_text("...",tag="dbl_scan_step",wrap=560)
            dpg.add_progress_bar(tag="dbl_scan_pb",default_value=0.0,width=-1,height=22)
            dpg.add_spacer(height=6)
            dpg.add_button(label=t("cancel"), tag="dbl_btn_cancel_scan", width=-1,
                           enabled=False,
                           callback=lambda s,a,u: _cancel_scan.set())

        # Fenetre principale

    def _build_body():

        # Titre + bouton langue + copyright
        with dpg.group(horizontal=True):
            dpg.add_text(t("app_title"),tag="dbl_t_appname",color=(233,69,96))
            dpg.add_text("- lecture seule (DirectX 11)",tag="dbl_lbl_sub",color=(136,136,170))
            dpg.add_spacer(width=20)
            dpg.add_button(label="EN",tag="dbl_btn_lang",width=36,show=False,
                callback=lambda s,a,u: toggle_lang())
            tip("Basculer la langue / Switch language")
        dpg.add_separator()

        # Ligne 1 : connexion (masquee : config commune en haut)
        with dpg.group(horizontal=True, tag="dbl_cfg_conn", show=False):
            dpg.add_text("URL",tag="dbl_lbl_url")
            dpg.add_input_text(tag="dbl_inp_url",default_value=CFG["emby"]["url"],width=210)
            dpg.add_spacer(width=6)
            dpg.add_text("Cle API",tag="dbl_lbl_apikey")
            dpg.add_input_text(tag="dbl_inp_key",default_value=CFG["emby"]["api_key"],
                               password=True,width=230)
            dpg.add_spacer(width=6)
            dpg.add_text("User ID",tag="dbl_lbl_uid")
            dpg.add_input_text(tag="dbl_inp_uid",default_value=CFG["emby"].get("user_id",""),
                               width=110,hint="optionnel")
        dpg.add_spacer(height=3)

        # Ligne 2 : NAS + lecteur (masquee)
        with dpg.group(horizontal=True, tag="dbl_cfg_nas", show=False):
            dpg.add_text("Prefixe",tag="dbl_lbl_prefix")
            dpg.add_input_text(tag="dbl_inp_prefix",
                               default_value=CFG["emby"].get("nas_prefix","/volume1"),width=110)
            dpg.add_text("->",color=(233,69,96))
            dpg.add_input_text(tag="dbl_inp_unc",
                               default_value=CFG["emby"].get("nas_unc",""),
                               width=200,hint=r"\\192.168.1.x")
            dpg.add_spacer(width=10)
            dpg.add_text("Lecteur video",tag="dbl_lbl_player")
            dpg.add_input_text(tag="dbl_inp_player",
                               default_value=CFG["emby"].get("player",""),
                               width=200,hint="C:\\...\\vlc.exe")
            tip_t("tip_player")
            dpg.add_button(label="...",callback=browse_player,width=24)
            tip_t("tip_browse")
        dpg.add_spacer(height=3)

        # Ligne 3 : boutons principaux
        with dpg.group(horizontal=True):
            dpg.add_button(label="Connecter",tag="dbl_btn_connect",show=False,
                           callback=lambda s,a,u:do_connect(),width=100)
            tip_t("tip_connect", wrap=380)
            dpg.add_button(label="Scanner",tag="dbl_btn_scan",callback=start_scan,
                           width=100,enabled=False)
            tip_t("tip_scan")
            dpg.add_button(label="Sauvegarder",tag="dbl_btn_save",callback=do_save,
                           width=110,enabled=False)
            tip_t("tip_save")
            dpg.add_button(label=t("load"),tag="dbl_btn_load",callback=do_load,width=110)
            tip_t("tip_load")
            dpg.add_button(label=t("export"),tag="dbl_btn_export",callback=do_export,width=90)
            tip_t("tip_export")
            dpg.add_button(label="Analyser Emby",tag="dbl_btn_refresh",
                           callback=lambda s,a,u: do_refresh_emby(),width=120)
            tip_t("tip_refresh_emby", wrap=380)
            dpg.add_spacer(width=10)
            dpg.add_text("Depart Ouvrir tout :",tag="dbl_lbl_tc",color=(136,136,170))
            dpg.add_input_text(tag="dbl_inp_tc",width=80,hint="0:45:00",
                               default_value=G.get("last_timecode") or load_last_timecode(),
                               callback=on_timecode_change)
            tip_t("tip_tc", wrap=360)
            dpg.add_text("",tag="dbl_lbl_tc_state",color=(136,136,170))
            dpg.add_spacer(width=10)
            # Sablier : visible seulement pendant une suppression
            with dpg.group(horizontal=True, tag="dbl_grp_del_busy", show=False):
                dpg.add_loading_indicator(style=1, radius=1.4, thickness=1.6,
                                          color=(240,160,0),
                                          secondary_color=(90,70,30))
                dpg.add_text("", tag="dbl_lbl_del_busy", color=(240,160,0))
                dpg.add_spacer(width=10)
            dpg.add_text("",tag="dbl_lbl_scan_info",color=(136,136,170))
        dpg.add_spacer(height=4)

        # Ligne 4 : médiathèques (remplie dynamiquement après Connecter)
        with dpg.child_window(tag="dbl_lib_panel",height=150,border=True,autosize_x=True):
            dpg.add_text("Cliquez sur Connecter pour charger les mediatheques.",
                         color=(136,136,170))
        dpg.add_spacer(height=4)

        # Ligne 5 : critères d'exclusion
        with dpg.collapsing_header(label=t("criteria_hdr"), tag="dbl_hdr_criteria",
                                   default_open=True):
            dpg.add_spacer(height=3)
            with dpg.group(horizontal=True):
                CRITERIA = [
                    ("resolution","Resolution differente (4K/HD/SD)","resolution"),
                    ("hdr",       "HDR vs SDR",                      "hdr"),
                    ("av1",       "Codec AV1",                       "av1"),
                    ("3d",        "Film 3D / SBS / MVC",             "3d"),
                    ("remaster",  "Remastered",                      "remaster"),
                    ("cut",       "Version longue / Extended / Director's Cut","cut"),
                    ("bonus",     "Bonus / Extras / Featurette",               "bonus"),
                ]
                for tag_suffix, label, key in CRITERIA:
                    _tip_key_map = {
                        "resolution":"tip_cr_resolution",
                        "hdr":"tip_cr_hdr","av1":"tip_cr_av1",
                        "3d":"tip_cr_3d","remaster":"tip_cr_remaster",
                        "cut":"tip_cr_cut","bonus":"tip_cr_bonus",
                    }
                    dpg.add_checkbox(
                        label=label, tag=f"dbl_chk_{tag_suffix}",
                        default_value=G["criteria"].get(key,True),
                        user_data=key,
                        callback=lambda s,v,u: _set_criterion(u,v))
                    tip_t(_tip_key_map.get(key,"tip_cr_resolution"), wrap=280)
                    dpg.add_spacer(width=14)
            dpg.add_spacer(height=3)
        dpg.add_spacer(height=3)

        # Ligne 6 : filtre + tri + seuil + ignorés
        with dpg.group(horizontal=True):
            dpg.add_text("Filtre :",tag="dbl_lbl_filter")
            dpg.add_input_text(tag="dbl_inp_filter",width=180,hint="Titre...",
                               callback=on_filter,on_enter=False)
            dpg.add_spacer(width=6)
            dpg.add_text("Trier :",tag="dbl_lbl_sort")
            dpg.add_combo(("Titre A>Z","Titre Z>A","Taille","Confiance v","Confiance ^"),
                          tag="dbl_cb_sort",default_value="Titre A>Z",
                          callback=on_sort,width=130)
            dpg.add_spacer(width=8)
            dpg.add_text("Res. :",tag="dbl_lbl_filter_res")
            dpg.add_combo(("Toutes","4K","1080p","720p","SD"),
                          tag="dbl_cb_filter_res",default_value="Toutes",
                          callback=on_filter_res,width=80)
            dpg.add_spacer(width=6)
            dpg.add_text("Codec :",tag="dbl_lbl_filter_codec")
            dpg.add_combo(("Tous","H264","HEVC","AV1"),
                          tag="dbl_cb_filter_codec",default_value="Tous",
                          callback=on_filter_codec,width=70)
            dpg.add_spacer(width=8)
            dpg.add_checkbox(label=t("flt_samedur"),tag="dbl_chk_same_dur",
                             default_value=False,callback=on_same_dur)
            tip_t("tip_samedur", wrap=340)
            dpg.add_checkbox(label=t("flt_samesize"),tag="dbl_chk_same_size",
                             default_value=False,callback=on_same_size)
            tip_t("tip_samesize", wrap=340)
            dpg.add_spacer(width=10)
            dpg.add_text("Seuil min :",tag="dbl_lbl_thresh")
            dpg.add_slider_int(tag="dbl_sld_score",default_value=0,min_value=0,max_value=100,
                               width=120,callback=on_score_threshold)
            tip_t("tip_score", wrap=300)
            dpg.add_text("0%",tag="dbl_lbl_score_val",color=(136,136,170))
            dpg.add_spacer(width=10)
            dpg.add_text("Ignores :",tag="dbl_lbl_ign_txt")
            dpg.add_text(t("ign_count")%len(G['ignored']),
                         tag="dbl_lbl_ignored",color=(230,126,34))
            dpg.add_spacer(width=4)
            dpg.add_button(label=t("manage_ign"),tag="dbl_btn_manage_ign",
                callback=lambda s,a,u: show_ignored_panel(), width=60)
            tip_t("manage_ign_tip")
            dpg.add_button(label="Reinitialiser",tag="dbl_btn_reset_ign",callback=do_reset_ignored,width=100)

        dpg.add_separator()
        dpg.add_child_window(tag="dbl_results_area",border=False,autosize_x=True,height=-1)


    # ══════════════════════════════════════════════════════════════
    #  POINT D'ENTREE
    # ══════════════════════════════════════════════════════════════

    _spin_state = {"shown": False, "txt": ""}

    def _update_delete_spinner():
        """Sablier anime + chrono pendant les suppressions en cours."""
        busy = bool(_deleting)
        try:
            if busy != _spin_state["shown"]:
                dpg.configure_item("dbl_grp_del_busy", show=busy)
                _spin_state["shown"] = busy
            if not busy:
                return
            en = G.get("lang", "fr") == "en"
            items = list(_deleting.values())
            name, t0 = min(items, key=lambda x: x[1])
            el = int(time.time() - t0)
            chrono = f"{el // 60}:{el % 60:02d}"
            if len(items) > 1:
                txt = (f"Deleting {len(items)} items... {chrono}" if en
                       else f"Suppression de {len(items)} éléments... {chrono}")
            else:
                short = name if len(name) <= 45 else name[:42] + "..."
                txt = (f"Deleting: {short}  {chrono}" if en
                       else f"Suppression : {short}  {chrono}")
            if txt != _spin_state["txt"]:
                dpg.set_value("dbl_lbl_del_busy", txt)
                _spin_state["txt"] = txt
        except Exception:
            pass

    def _tick():
        nonlocal _render_timer
        if _render_timer > 0 and time.time() >= _render_timer:
            _render_timer = 0.0
            render_results()
        _update_delete_spinner()

    def _set_lang(code):
        G["lang"] = "en" if str(code).upper() == "EN" else "fr"
        apply_lang()

    return {
        "build_popups": _build_popups,
        "build_body": _build_body,
        "tick": _tick,
        "connect": do_connect,
        "set_lang": _set_lang,
        "save": do_save,
        "legacy": {
            "url": CFG["emby"].get("url", ""),
            "api_key": CFG["emby"].get("api_key", ""),
            "user_id": CFG["emby"].get("user_id", ""),
            "nas_prefix": CFG["emby"].get("nas_prefix", ""),
            "nas_unc": CFG["emby"].get("nas_unc", ""),
            "player": CFG["emby"].get("player", ""),
        },
    }


# =====================================================================
#  Propagation des identifiants partages vers les champs des 3 onglets
# =====================================================================
def _push_genres(c):
    m = {"inp_url": "url", "inp_key": "api_key", "inp_uid": "user_id",
         "inp_prefix": "nas_prefix", "inp_unc": "nas_unc",
         "inp_player": "player", "inp_omdb_key": "omdb_key",
         "inp_tmdb_key": "tmdb_key"}
    for tag, k in m.items():
        if dpg.does_item_exist(tag):
            dpg.set_value(tag, c.get(k, "") or "")

def _push_refmatch(c):
    for tag, k in {"emby_url": "url", "emby_key": "api_key",
                   "tmdb_key": "tmdb_key"}.items():
        if dpg.does_item_exist(tag):
            dpg.set_value(tag, c.get(k, "") or "")

def _push_doublons(c):
    m = {"dbl_inp_url": "url", "dbl_inp_key": "api_key", "dbl_inp_uid": "user_id",
         "dbl_inp_prefix": "nas_prefix", "dbl_inp_unc": "nas_unc",
         "dbl_inp_player": "player"}
    for tag, k in m.items():
        if dpg.does_item_exist(tag):
            dpg.set_value(tag, c.get(k, "") or "")

def migrate_into_shared(app, DBL):
    """Au premier lancement : agrege les clefs des anciens fichiers de
    config (genres, refmatch, doublons) dans le magasin partage chiffre."""
    if SHARED_CREDS_FILE.exists():
        load_shared_creds()
        return

    def first(*vals):
        for v in vals:
            if v:
                return v
        return ""

    try:
        g = dict(CFG["emby"]) if CFG.has_section("emby") else {}
    except Exception:
        g = {}
    try:
        api = dict(API_CFG)
    except Exception:
        api = {}
    leg = DBL.get("legacy", {})

    save_shared_creds(
        url=first(g.get("url"), app.get("emby", "url", ""), leg.get("url")),
        api_key=first(g.get("api_key"),
                      decrypt_secret(app.get("emby", "api_key", "")),
                      leg.get("api_key")),
        user_id=first(g.get("user_id"), leg.get("user_id")),
        nas_prefix=first(g.get("nas_prefix"), leg.get("nas_prefix")),
        nas_unc=first(g.get("nas_unc"), leg.get("nas_unc")),
        player=first(g.get("player"), leg.get("player")),
        omdb_key=first(api.get("omdb_key")),
        tmdb_key=first(api.get("tmdb_key"),
                       decrypt_secret(app.get("tmdb", "api_key", ""))),
        provider=first(api.get("provider")),
        lang=first(getattr(app, "lang", "FR")),
    )

# =====================================================================
#  OUTIL 4 : MKV Renamer  --  renommage automatique des films MKV
#
#      "Max de betises.mkv"
#   -> "Max de betises.2001.MULTi.1080p.WEB-DL.X265.mkv"
#
#  Composition du nom :
#      Titre . Annee . Langue . Resolution . Source . Codec . extension
#
#  Moteur repris de film_rename_dpg.pyw et rendu BILINGUE : toute chaine
#  affichable est une paire (fr, en), pour que la bascule FR/EN du bandeau
#  commun retraduise aussi les explications deja calculees.
#  La cle TMDB est celle du bandeau de configuration commun (sh_tmdb).
# =====================================================================

MKV_IS_WIN = _IS_WIN
MKV_NO_WINDOW = 0x08000000 if MKV_IS_WIN else 0

MKV_VIDEO_EXTS_DEFAULT = ".mkv"
MKV_COMPANION_EXTS = {".srt", ".sub", ".idx", ".ass", ".ssa", ".sup", ".nfo",
                      ".txt", ".jpg", ".jpeg", ".png", ".vtt"}


def _mkv_writable(d):
    try:
        d.mkdir(parents=True, exist_ok=True)
        t = d / ".wtest"
        t.write_text("x", encoding="utf-8")
        t.unlink()
        return True
    except Exception:
        return False


MKV_DATA_DIR = APP_DIR if _mkv_writable(APP_DIR) else Path(
    os.environ.get("APPDATA") or os.path.expanduser("~")) / "EmbyToolbox"
if MKV_DATA_DIR is not APP_DIR:
    _mkv_writable(MKV_DATA_DIR)

MKV_INI_PATH = MKV_DATA_DIR / "emby_toolbox_mkvrename.ini"
MKV_DB_PATH = MKV_DATA_DIR / "emby_toolbox_mkv_cache.db"
MKV_JOURNAL_PATH = MKV_DATA_DIR / "emby_toolbox_mkv_journal.jsonl"
MKV_LEGACY_INI = MKV_DATA_DIR / "film_rename.ini"      # ancien script autonome


class MkvError(Exception):
    """Erreur portant ses deux traductions."""

    def __init__(self, fr, en):
        Exception.__init__(self, fr)
        self.pair = (fr, en)


def mkv_pair(exc):
    """Paire (fr, en) d'une exception, meme non traduite."""
    p = getattr(exc, "pair", None)
    if isinstance(p, tuple) and len(p) == 2:
        return p
    s = str(exc)
    return (s, s)


# ---------------------------------------------------------------------
#  Configuration propre a l'onglet (la cle TMDB reste dans le bandeau)
# ---------------------------------------------------------------------
MKV_DEFAULTS = {
    "general": {
        "dossier": "",
        "recursif": "0",
        "extensions": MKV_VIDEO_EXTS_DEFAULT,
    },
    "tmdb": {
        # Par defaut TMDB ne touche JAMAIS a une annee lue dans le nom de
        # fichier ou saisie a la main : il ne comble que les cases vides.
        "remplacer_annee_du_nom": "0",
    },
    "format": {
        "separateur": ".",
        "style_codec": "X265",
        "token_vf": "VF",
        "points_dans_titre": "0",
        "ajouter_10bit": "0",
        "ajouter_hdr": "0",
        "source_par_defaut": "WEB-DL",
        "renommer_associes": "1",
    },
    "chemins": {
        "ffprobe": "",
    },
}


def mkv_load_config():
    cfg = configparser.ConfigParser()
    cfg.read_dict(MKV_DEFAULTS)
    for path in (MKV_INI_PATH, MKV_LEGACY_INI):   # reprise de l'ancien .ini
        try:
            if path.exists():
                cfg.read(path, encoding="utf-8")
                break
        except Exception:
            pass
    return cfg


def mkv_save_config(cfg):
    try:
        with open(MKV_INI_PATH, "w", encoding="utf-8") as fh:
            cfg.write(fh)
        return True
    except Exception:
        return False


# ---------------------------------------------------------------------
#  ffprobe
# ---------------------------------------------------------------------
def mkv_find_ffprobe(configured=""):
    exe = "ffprobe.exe" if MKV_IS_WIN else "ffprobe"
    cands = []
    if configured:
        cands.append(configured)
    cands += [
        str(APP_DIR / exe),
        str(APP_DIR / "bin" / exe),
        str(APP_DIR / "ffmpeg" / "bin" / exe),
    ]
    if MKV_IS_WIN:
        cands += [
            r"C:\ffmpeg\bin\ffprobe.exe",
            r"C:\Program Files\ffmpeg\bin\ffprobe.exe",
            os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Links\ffprobe.exe"),
            os.path.expandvars(r"%USERPROFILE%\scoop\shims\ffprobe.exe"),
            r"C:\ProgramData\chocolatey\bin\ffprobe.exe",
        ]
    for c in cands:
        try:
            if c and Path(c).is_file():
                return str(Path(c))
        except Exception:
            continue
    import shutil as _sh
    return _sh.which("ffprobe") or ""


def mkv_run_ffprobe(ffprobe, path, timeout=90):
    """Dict JSON de ffprobe, ou leve MkvError (message bilingue)."""
    cmd = [ffprobe, "-v", "quiet", "-print_format", "json",
           "-show_streams", "-show_format", "-show_chapters", str(path)]
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=timeout, creationflags=MKV_NO_WINDOW)
    except FileNotFoundError:
        raise MkvError("ffprobe introuvable", "ffprobe not found")
    except subprocess.TimeoutExpired:
        raise MkvError("ffprobe : delai depasse", "ffprobe: timed out")
    if p.returncode != 0:
        err = (p.stderr or b"").decode("utf-8", "replace").strip()
        err = err[:200] or str(p.returncode)
        raise MkvError("ffprobe a echoue : %s" % err, "ffprobe failed: %s" % err)
    txt = (p.stdout or b"").decode("utf-8", "replace")
    try:
        return json.loads(txt)
    except Exception:
        raise MkvError("sortie ffprobe illisible", "unreadable ffprobe output")


# ---------------------------------------------------------------------
#  Moteur : normalisation des langues
# ---------------------------------------------------------------------
MKV_FR_CODES = {"fr", "fre", "fra", "fr-fr", "fr-ca", "fre-ca", "french",
                "francais", "france", "vf", "vff", "vfq", "vfi", "truefrench", "qc"}
MKV_LANG_MAP = {
    "eng": "en", "en": "en", "english": "en", "anglais": "en",
    "spa": "es", "es": "es", "esp": "es", "spanish": "es", "espagnol": "es",
    "ger": "de", "deu": "de", "de": "de", "german": "de", "allemand": "de",
    "ita": "it", "it": "it", "italian": "it", "italien": "it",
    "por": "pt", "pt": "pt", "jpn": "ja", "ja": "ja", "japanese": "ja",
    "chi": "zh", "zho": "zh", "zh": "zh", "kor": "ko", "ko": "ko",
    "rus": "ru", "ru": "ru", "dut": "nl", "nld": "nl", "nl": "nl",
    "ara": "ar", "ar": "ar", "pol": "pl", "pl": "pl", "tur": "tr", "tr": "tr",
    "swe": "sv", "sv": "sv", "dan": "da", "da": "da", "nor": "no", "no": "no",
    "fin": "fi", "fi": "fi", "ces": "cs", "cze": "cs", "cs": "cs",
    "hun": "hu", "hu": "hu", "heb": "he", "he": "he", "hin": "hi", "hi": "hi",
    "tha": "th", "th": "th", "ukr": "uk", "uk": "uk", "ron": "ro", "rum": "ro",
    "gre": "el", "ell": "el", "el": "el", "vie": "vi", "vi": "vi",
}
MKV_UNKNOWN_LANG = {"", "und", "unk", "zxx", "mis", "mul", "qaa", "none"}


def mkv_strip_accents(s):
    try:
        return "".join(ch for ch in unicodedata.normalize("NFD", str(s))
                       if unicodedata.category(ch) != "Mn")
    except Exception:
        return str(s)


def mkv_norm_lang(code):
    """Normalise un code de langue -> 'fr', 'en', ... ou '' si inconnu."""
    if not code:
        return ""
    c = mkv_strip_accents(str(code)).strip().lower()
    if c in MKV_UNKNOWN_LANG:
        return ""
    if c in MKV_FR_CODES:
        return "fr"
    if c in MKV_LANG_MAP:
        return MKV_LANG_MAP[c]
    c2 = c.split("-")[0]
    if c2 in MKV_FR_CODES:
        return "fr"
    return MKV_LANG_MAP.get(c2, c2[:3])


def mkv_lang_from_title(title):
    """Devine la langue depuis le libelle d'une piste ('VF', 'French', ...)."""
    if not title:
        return ""
    t = mkv_strip_accents(title).lower()
    if re.search(r"\b(vff?|vfq|vfi|vf2|truefrench|francais|french|fr)\b", t):
        return "fr"
    if re.search(r"\b(vo|eng|english|anglais|en)\b", t):
        return "en"
    for key, val in MKV_LANG_MAP.items():
        if re.search(r"\b%s\b" % re.escape(key), t):
            return val
    return ""


# ---------------------------------------------------------------------
#  Moteur : resolution, codec
# ---------------------------------------------------------------------
def mkv_resolution_token(width, height):
    """Hauteur effective : un film scope 1920x800 reste du 1080p."""
    try:
        w = int(width or 0)
        h = int(height or 0)
    except Exception:
        return ""
    if w <= 0 and h <= 0:
        return ""
    eff = max(h, int(round(w * 9.0 / 16.0)) if w else 0)
    if eff >= 1800:
        return "2160p"
    if eff >= 1300:
        return "1440p"
    if eff >= 900:
        return "1080p"
    if eff >= 620:
        return "720p"
    if eff >= 520:
        return "576p"
    if eff >= 380:
        return "480p"
    return "SD"


MKV_CODEC_FAMILY = {
    "h264": "h264", "avc": "h264", "avc1": "h264",
    "hevc": "h265", "h265": "h265",
    "av1": "av1",
    "vp9": "vp9", "vp8": "vp8",
    "mpeg2video": "mpeg2", "mpeg4": "xvid", "msmpeg4v3": "divx",
    "vc1": "vc1", "wmv3": "vc1",
    "prores": "prores",
}
MKV_CODEC_STYLES = {
    "X265": {"h264": "X264", "h265": "X265", "av1": "AV1", "vp9": "VP9",
             "vp8": "VP8", "mpeg2": "MPEG2", "xvid": "XviD", "divx": "DivX",
             "vc1": "VC1", "prores": "ProRes"},
    "x265": {"h264": "x264", "h265": "x265", "av1": "av1", "vp9": "vp9",
             "vp8": "vp8", "mpeg2": "mpeg2", "xvid": "xvid", "divx": "divx",
             "vc1": "vc1", "prores": "prores"},
    "H.265": {"h264": "H.264", "h265": "H.265", "av1": "AV1", "vp9": "VP9",
              "vp8": "VP8", "mpeg2": "MPEG-2", "xvid": "XviD", "divx": "DivX",
              "vc1": "VC-1", "prores": "ProRes"},
    "HEVC": {"h264": "AVC", "h265": "HEVC", "av1": "AV1", "vp9": "VP9",
             "vp8": "VP8", "mpeg2": "MPEG2", "xvid": "XviD", "divx": "DivX",
             "vc1": "VC1", "prores": "ProRes"},
}


def mkv_codec_token(codec_name, style="X265"):
    fam = MKV_CODEC_FAMILY.get(str(codec_name or "").lower())
    table = MKV_CODEC_STYLES.get(style, MKV_CODEC_STYLES["X265"])
    if not fam:
        return str(codec_name or "").upper()
    return table.get(fam, fam)


# ---------------------------------------------------------------------
#  Moteur : synthese d'un fichier a partir du JSON ffprobe
# ---------------------------------------------------------------------
MKV_NONE_FR = "(aucun)"
MKV_NONE_EN = "(none)"
MKV_NONE_TOKENS = {MKV_NONE_FR, MKV_NONE_EN}

MKV_LANG_CHOICES = ["MULTi", "VF", "VOSTFR", "VO", "TRUEFRENCH", "VFF", "VFQ"]
MKV_SOURCE_CHOICES = ["WEB-DL", "WEBRip", "BluRay", "BluRay.REMUX", "BDRip",
                      "HDTV", "DVDRip", "HDLight"]

MKV_AUDIO_BD = {"dts", "truehd", "mlp", "pcm_s16le", "pcm_s24le", "pcm_bluray",
                "pcm_dvd", "flac", "dts-hd"}
MKV_SUB_IMAGE = {"hdmv_pgs_subtitle", "dvd_subtitle", "dvb_subtitle", "xsub"}
MKV_SUB_TEXT = {"subrip", "ass", "ssa", "mov_text", "webvtt", "text"}

MKV_BR_WEB_MAX = {"2160p": 30.0, "1440p": 14.0, "1080p": 9.5, "720p": 4.5,
                  "576p": 2.8, "480p": 2.0, "SD": 1.6}
MKV_BR_BD_MIN = {"2160p": 45.0, "1440p": 20.0, "1080p": 12.0, "720p": 6.0,
                 "576p": 4.0, "480p": 3.0, "SD": 2.5}
MKV_BR_REMUX_MIN = {"2160p": 55.0, "1440p": 35.0, "1080p": 25.0, "720p": 15.0,
                    "576p": 10.0, "480p": 8.0, "SD": 6.0}


def _mkv_f(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return 0.0


def mkv_analyse_probe(data, filename=""):
    """Transforme la sortie ffprobe en dictionnaire de caracteristiques."""
    streams = data.get("streams") or []
    fmt = data.get("format") or {}
    chapters = data.get("chapters") or []

    video = None
    for s in streams:
        if s.get("codec_type") == "video":
            disp = s.get("disposition") or {}
            if disp.get("attached_pic"):
                continue
            if video is None or int(s.get("width") or 0) > int(video.get("width") or 0):
                video = s
    audios = [s for s in streams if s.get("codec_type") == "audio"]
    subs = [s for s in streams if s.get("codec_type") == "subtitle"]

    info = {
        "width": int((video or {}).get("width") or 0),
        "height": int((video or {}).get("height") or 0),
        "video_codec": (video or {}).get("codec_name") or "",
        "pix_fmt": (video or {}).get("pix_fmt") or "",
        "color_transfer": (video or {}).get("color_transfer") or "",
        "duration": _mkv_f(fmt.get("duration")) or _mkv_f((video or {}).get("duration")),
        "size": _mkv_f(fmt.get("size")),
        "bit_rate": _mkv_f(fmt.get("bit_rate")),
        "audio_codecs": [str(a.get("codec_name") or "").lower() for a in audios],
        "audio_langs": [],
        "audio_titles": [],
        "sub_codecs": [str(s.get("codec_name") or "").lower() for s in subs],
        "sub_langs": [],
        "n_chapters": len(chapters),
        "tags": {str(k).lower(): str(v) for k, v in (fmt.get("tags") or {}).items()},
        "video_tags": {str(k).lower(): str(v)
                       for k, v in ((video or {}).get("tags") or {}).items()},
        "filename": filename or os.path.basename(str(fmt.get("filename") or "")),
    }
    for a in audios:
        tags = a.get("tags") or {}
        lg = mkv_norm_lang(tags.get("language") or tags.get("LANGUAGE"))
        title = tags.get("title") or tags.get("TITLE") or ""
        if not lg:
            lg = mkv_lang_from_title(title)
        info["audio_langs"].append(lg)
        info["audio_titles"].append(title)
    for s in subs:
        tags = s.get("tags") or {}
        lg = mkv_norm_lang(tags.get("language") or tags.get("LANGUAGE"))
        if not lg:
            lg = mkv_lang_from_title(tags.get("title") or tags.get("TITLE") or "")
        info["sub_langs"].append(lg)

    br = info["bit_rate"]
    if not br and info["size"] and info["duration"]:
        br = info["size"] * 8.0 / info["duration"]
    info["mbps"] = round(br / 1e6, 2) if br else 0.0

    info["resolution"] = mkv_resolution_token(info["width"], info["height"])
    info["is_10bit"] = "10le" in info["pix_fmt"] or "10be" in info["pix_fmt"]
    ct = info["color_transfer"].lower()
    info["is_hdr"] = ct in ("smpte2084", "arib-std-b67") or "bt2020" in ct
    return info


def mkv_language_token(info, token_vf="VF"):
    """(token, (raison_fr, raison_en)) : MULTi / VF / VOSTFR / VO."""
    langs = [l for l in info.get("audio_langs", []) if l]
    n_audio = len(info.get("audio_langs", []))
    has_fr = "fr" in langs
    others = sorted(set(l for l in langs if l != "fr"))
    oth = ", ".join(others)

    if has_fr and others:
        return "MULTi", ("pistes audio : fr + %s" % oth,
                         "audio tracks: fr + %s" % oth)
    if has_fr and not others:
        if n_audio > len(langs):
            return token_vf, ("piste(s) francaise(s), autres pistes sans code langue",
                              "French track(s), other tracks without a language code")
        n = max(n_audio, 1)
        return token_vf, ("%d piste(s) audio, francais uniquement" % n,
                          "%d audio track(s), French only" % n)
    if not has_fr and others:
        if "fr" in info.get("sub_langs", []):
            return "VOSTFR", ("audio %s, sous-titres francais" % oth,
                              "%s audio, French subtitles" % oth)
        return "VO", ("audio %s, aucun sous-titre francais" % oth,
                      "%s audio, no French subtitles" % oth)
    if "fr" in info.get("sub_langs", []):
        return "VOSTFR", ("aucun code langue audio, sous-titres francais presents",
                          "no audio language code, French subtitles present")
    return "", ("langue indeterminee (pistes audio sans code langue)",
                "undetermined language (audio tracks without a language code)")


def mkv_guess_source(info):
    """(source, cle_de_confiance, [(raison_fr, raison_en)])."""
    score = {}
    reasons = []

    def add(src, pts, why_fr, why_en):
        score[src] = score.get(src, 0.0) + pts
        head = "%+.0f %s : " % (pts, src)
        reasons.append((head + why_fr, head + why_en))

    name = mkv_strip_accents(info.get("filename", "")).lower()
    named = None
    for pat, src in ((r"blu-?ray.?remux|bd.?remux|\bremux\b", "BluRay.REMUX"),
                     (r"web-?dl", "WEB-DL"),
                     (r"web-?rip|\bwebrip\b", "WEBRip"),
                     (r"blu-?ray|\bbdrip\b|\bbrrip\b", "BluRay"),
                     (r"\bhdtv\b", "HDTV"),
                     (r"\bdvdrip\b|\bdvdscr\b", "DVDRip"),
                     (r"\bhdlight\b", "HDLight")):
        if re.search(pat, name):
            named = src
            break
    if named:
        add(named, 6, "deja present dans le nom du fichier",
            "already present in the file name")

    ac = set(info.get("audio_codecs", []))
    if ac & {"dts", "truehd", "mlp"}:
        add("BluRay", 4, "audio DTS/TrueHD (absent des plateformes de streaming)",
            "DTS/TrueHD audio (not found on streaming platforms)")
    elif ac & {"pcm_s16le", "pcm_s24le", "pcm_bluray", "flac"}:
        add("BluRay", 3, "audio PCM/FLAC (piste de disque)",
            "PCM/FLAC audio (disc track)")
    if "eac3" in ac:
        add("WEB-DL", 3, "audio E-AC-3 (Dolby Digital Plus, signature streaming)",
            "E-AC-3 audio (Dolby Digital Plus, streaming signature)")
    elif "aac" in ac and not (ac & MKV_AUDIO_BD):
        add("WEB-DL", 1, "audio AAC", "AAC audio")

    sc = set(info.get("sub_codecs", []))
    if sc & MKV_SUB_IMAGE:
        add("BluRay", 3, "sous-titres image PGS/VobSub (issus d'un disque)",
            "PGS/VobSub image subtitles (from a disc)")
    elif sc & MKV_SUB_TEXT:
        add("WEB-DL", 1, "sous-titres texte uniquement", "text subtitles only")

    res = info.get("resolution") or "1080p"
    fam = MKV_CODEC_FAMILY.get(str(info.get("video_codec", "")).lower(), "h264")
    factor = 0.6 if fam in ("h265", "av1", "vp9") else 1.0
    mbps = info.get("mbps") or 0.0
    if mbps > 0:
        web_max = MKV_BR_WEB_MAX.get(res, 9.5) * factor
        bd_min = MKV_BR_BD_MIN.get(res, 12.0) * factor
        remux_min = MKV_BR_REMUX_MIN.get(res, 25.0)
        if mbps >= remux_min:
            add("BluRay.REMUX", 4,
                "debit %.1f Mb/s tres eleve (>= %.0f)" % (mbps, remux_min),
                "bitrate %.1f Mb/s very high (>= %.0f)" % (mbps, remux_min))
        elif mbps >= bd_min:
            add("BluRay", 2,
                "debit %.1f Mb/s eleve (>= %.1f)" % (mbps, bd_min),
                "bitrate %.1f Mb/s high (>= %.1f)" % (mbps, bd_min))
        elif mbps <= web_max:
            add("WEB-DL", 2,
                "debit %.1f Mb/s modere (<= %.1f)" % (mbps, web_max),
                "bitrate %.1f Mb/s moderate (<= %.1f)" % (mbps, web_max))
        else:
            reasons.append(("  0 debit %.1f Mb/s : zone ambigue" % mbps,
                            "  0 bitrate %.1f Mb/s: ambiguous range" % mbps))

    tags_blob = " ".join(list(info.get("tags", {}).values()) +
                         list(info.get("video_tags", {}).values())).lower()
    if "makemkv" in tags_blob:
        add("BluRay.REMUX", 4, "muxe par MakeMKV (extraction de disque)",
            "muxed by MakeMKV (disc rip)")
    elif "handbrake" in tags_blob:
        add("BDRip", 2, "encode par HandBrake", "encoded by HandBrake")
    elif "lavf" in tags_blob and "mkvmerge" not in tags_blob:
        add("WEB-DL", 1, "muxe par ffmpeg/Lavf (typique d'une capture streaming)",
            "muxed by ffmpeg/Lavf (typical of a streaming capture)")
    if re.search(r"\b(amzn|nf|netflix|dsnp|disney|atvp|hmax|pmtp|itunes|crav|stan)\b",
                 tags_blob + " " + name):
        add("WEB-DL", 4, "identifiant de plateforme trouve dans les tags/le nom",
            "platform identifier found in the tags/name")

    if info.get("n_chapters", 0) >= 8:
        n = info["n_chapters"]
        add("BluRay", 1, "%d chapitres (typique d'un disque)" % n,
            "%d chapters (typical of a disc)" % n)

    if not score:
        return "", "none", [("aucun indice exploitable", "no usable clue")]

    ranked = sorted(score.items(), key=lambda kv: kv[1], reverse=True)
    best, best_pts = ranked[0]
    second_pts = ranked[1][1] if len(ranked) > 1 else 0.0
    # BluRay.REMUX et BluRay ne se contredisent pas
    if best.startswith("BluRay"):
        second_pts = max([v for k, v in ranked[1:] if not k.startswith("BluRay")] or [0.0])
        best_pts = sum(v for k, v in ranked if k.startswith("BluRay"))
        best = "BluRay.REMUX" if score.get("BluRay.REMUX", 0) >= 4 else "BluRay"
    delta = best_pts - second_pts
    conf = "high" if delta >= 4 else ("medium" if delta >= 2 else "low")
    return best, conf, reasons


# ---------------------------------------------------------------------
#  Moteur : nettoyage du titre, annee, construction du nom
# ---------------------------------------------------------------------
MKV_JUNK_TOKENS = [
    r"2160p", r"1440p", r"1080p", r"720p", r"576p", r"480p", r"4k", r"uhd",
    r"x264", r"x265", r"h\.?264", r"h\.?265", r"hevc", r"avc", r"xvid", r"divx",
    r"av1", r"vp9", r"10\s?bits?", r"8\s?bits?", r"hdr10\+?", r"hdr", r"sdr",
    r"dolby\s?vision", r"dovi",
    r"web-?dl", r"web-?rip", r"blu-?ray", r"bdrip", r"brrip", r"bdremux",
    r"remux", r"hdtv", r"hdlight", r"dvdrip", r"dvdscr", r"hddvd", r"vodr",
    r"multi", r"vff", r"vfq", r"vfi", r"vf2", r"truefrench", r"french",
    r"vostfr", r"vost", r"subfrench", r"fastsub",
    r"ac3", r"eac3", r"ddp?5\.?1", r"dd\+?", r"dts-?hd(-?ma)?", r"dts",
    r"truehd", r"atmos", r"aac", r"flac", r"opus", r"mp3", r"5\.1", r"7\.1",
    r"2\.0", r"repack", r"proper", r"internal", r"limited",
]
MKV_JUNK_RE = re.compile(r"(?<![a-z0-9])(?:%s)(?![a-z0-9])" % "|".join(MKV_JUNK_TOKENS),
                         re.IGNORECASE)
MKV_YEAR_RE = re.compile(r"(?<!\d)(19\d{2}|20\d{2})(?!\d)")


def mkv_extract_year(name):
    """Derniere annee plausible trouvee dans le nom, ou ''."""
    limit = time.localtime().tm_year + 2
    found = [y for y in MKV_YEAR_RE.findall(name) if 1900 <= int(y) <= limit]
    return found[-1] if found else ""


def mkv_clean_title(filename):
    """Nom de fichier -> titre lisible (sans extension ni tokens techniques)."""
    stem = re.sub(r"\.[A-Za-z0-9]{2,4}$", "", str(filename))
    stem = re.sub(r"[\[\{\(][^\]\}\)]{0,40}[\]\}\)]", " ", stem)
    stem = re.sub(r"[._]+", " ", stem)
    year = mkv_extract_year(stem)
    if year:
        idx = stem.rfind(year)          # tout ce qui suit l'annee est technique
        if idx > 2:
            stem = stem[:idx]
    stem = MKV_JUNK_RE.sub(" ", stem)
    stem = re.sub(r"-\s*[A-Za-z0-9]{2,12}\s*$", " ", stem)          # -GROUPE final
    stem = re.sub(r"\s*[-\u2013]\s*$", " ", stem)
    stem = re.sub(r"\s{2,}", " ", stem).strip(" -_.")
    return stem


MKV_INVALID_MAP = {"<": "", ">": "", ":": "", '"': "'", "/": "-", "\\": "-",
                   "|": "-", "?": "", "*": ""}


def mkv_sanitize_filename(name):
    out = []
    for ch in str(name):
        if ord(ch) < 32:
            continue
        out.append(MKV_INVALID_MAP.get(ch, ch))
    s = "".join(out)
    s = re.sub(r"\s{2,}", " ", s).strip()
    return s.rstrip(" .")


def mkv_build_name(title, year, lang, resolution, source, codec, ext,
                   sep=".", dots_in_title=False, hdr="", tenbit=""):
    title = mkv_sanitize_filename(title)
    if dots_in_title:
        title = re.sub(r"\s+", ".", title)
    parts = [p for p in (year, lang, resolution, source, hdr, tenbit, codec)
             if p and p not in MKV_NONE_TOKENS]
    base = title
    if parts:
        base = title + sep + sep.join(parts)
    return mkv_sanitize_filename(base) + (ext or "")


# ---------------------------------------------------------------------
#  TMDB (annee uniquement) + cache SQLite
# ---------------------------------------------------------------------
class MkvTmdbCache(object):
    def __init__(self, path):
        self.path = str(path)
        self.lock = threading.Lock()
        self._init()

    def _conn(self):
        return sqlite3.connect(self.path, timeout=10)

    def _init(self):
        try:
            with self.lock, self._conn() as c:
                c.execute("CREATE TABLE IF NOT EXISTS tmdb_cache ("
                          "query TEXT PRIMARY KEY, year TEXT, title TEXT, "
                          "ts INTEGER, payload TEXT)")
        except Exception:
            pass

    def get(self, key):
        try:
            with self.lock, self._conn() as c:
                r = c.execute("SELECT year, title, payload FROM tmdb_cache "
                              "WHERE query=?", (key,)).fetchone()
            if r:
                return {"year": r[0] or "", "title": r[1] or "",
                        "results": json.loads(r[2] or "[]")}
        except Exception:
            pass
        return None

    def put(self, key, year, title, results):
        try:
            with self.lock, self._conn() as c:
                c.execute("INSERT OR REPLACE INTO tmdb_cache VALUES (?,?,?,?,?)",
                          (key, year or "", title or "", int(time.time()),
                           json.dumps(results[:8], ensure_ascii=False)))
        except Exception:
            pass

    def clear(self):
        try:
            with self.lock, self._conn() as c:
                c.execute("DELETE FROM tmdb_cache")
            return True
        except Exception:
            return False


def mkv_key_kind(api_key):
    """'v4' pour un jeton Bearer (JWT), 'v3' pour une cle api_key classique."""
    key = (api_key or "").strip()
    return "v4" if (len(key) > 60 or key.count(".") >= 2) else "v3"


def _mkv_tmdb_request(api_key, path, params=None, timeout=15):
    """Appel brut a l'API TMDB. Leve urllib.error.HTTPError / OSError."""
    params = dict(params or {})
    headers = {"User-Agent": "EmbyToolbox-MkvRenamer/1.0"}
    key = (api_key or "").strip()
    if mkv_key_kind(key) == "v4":
        headers["Authorization"] = "Bearer " + key
    else:
        params["api_key"] = key
    url = "https://api.themoviedb.org/3/%s?%s" % (path, urllib.parse.urlencode(params))
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8", "replace"))


def _mkv_http_error_message(exc):
    """Message lisible renvoye par TMDB dans le corps d'une erreur HTTP."""
    try:
        body = json.loads(exc.read().decode("utf-8", "replace"))
        return body.get("status_message") or ""
    except Exception:
        return ""


def mkv_tmdb_search(api_key, query, language="fr-FR", year_hint="", timeout=15):
    """Liste de candidats TMDB, non triee."""
    if not api_key or not query.strip():
        return []
    params = {"query": query, "language": language, "include_adult": "false"}
    if year_hint:
        params["year"] = year_hint
    data = _mkv_tmdb_request(api_key, "search/movie", params, timeout=timeout)
    out = []
    for r in (data.get("results") or []):
        date = r.get("release_date") or ""
        out.append({
            "title": r.get("title") or r.get("original_title") or "",
            "original_title": r.get("original_title") or "",
            "year": date[:4] if len(date) >= 4 else "",
            "popularity": r.get("popularity") or 0,
            "votes": r.get("vote_count") or 0,
        })
    return out


def mkv_tmdb_check_key(api_key, language="fr-FR", timeout=12):
    """Teste la cle TMDB. Retourne (ok, (resume_fr, resume_en), [(fr, en)])."""
    key = (api_key or "").strip()
    if not key:
        return (False, ("Aucune cle TMDB dans la configuration commune.",
                        "No TMDB key in the common configuration."),
                [("Renseigne une cle v3 ou un jeton v4 dans le bandeau du haut.",
                  "Enter a v3 key or a v4 token in the top bar.")])
    kind = mkv_key_kind(key)
    label = ("jeton v4 (en-tete Bearer)", "v4 token (Bearer header)") if kind == "v4" \
        else ("cle v3 (parametre api_key)", "v3 key (api_key parameter)")
    details = [("Type detecte : %s" % label[0], "Detected type: %s" % label[1]),
               ("Longueur : %d caracteres" % len(key),
                "Length: %d characters" % len(key))]
    if kind == "v3" and not re.fullmatch(r"[0-9a-fA-F]{32}", key):
        details.append(("Attention : une cle v3 fait normalement 32 caracteres "
                        "hexadecimaux.",
                        "Warning: a v3 key is normally 32 hexadecimal characters."))
    try:
        cfg = _mkv_tmdb_request(key, "configuration", timeout=timeout)
    except urllib.error.HTTPError as exc:
        msg = _mkv_http_error_message(exc)
        if exc.code == 401:
            return (False, ("Cle refusee par TMDB (401).",
                            "Key rejected by TMDB (401)."),
                    details + [(msg or "Cle invalide, revoquee, ou mauvais type de jeton.",
                                msg or "Invalid key, revoked, or wrong token type."),
                               ("Verifie sur themoviedb.org : Parametres > API.",
                                "Check on themoviedb.org: Settings > API.")])
        if exc.code == 429:
            return (False, ("Quota TMDB depasse (429).", "TMDB quota exceeded (429)."),
                    details + [(msg or "Trop de requetes, reessaie dans un instant.",
                                msg or "Too many requests, try again shortly.")])
        return (False, ("Erreur HTTP %s." % exc.code, "HTTP error %s." % exc.code),
                details + [(msg or str(exc.reason), msg or str(exc.reason))])
    except Exception as exc:
        return (False, ("TMDB injoignable.", "TMDB unreachable."),
                details + [(str(exc), str(exc)),
                           ("Verifie la connexion, le proxy ou le pare-feu.",
                            "Check the connection, proxy or firewall.")])
    details.append(("Authentification acceptee (endpoint /configuration).",
                    "Authentication accepted (/configuration endpoint)."))
    base = ((cfg.get("images") or {}).get("secure_base_url")) or ""
    if base:
        details.append(("Serveur d'images : %s" % base, "Image server: %s" % base))

    probe = "Le Dernier Chateau"
    try:
        res = mkv_tmdb_search(key, probe, language=language, timeout=timeout)
    except urllib.error.HTTPError as exc:
        m = _mkv_http_error_message(exc) or str(exc.reason)
        return (False, ("Authentification OK, mais la recherche echoue (HTTP %s)." % exc.code,
                        "Authentication OK, but the search fails (HTTP %s)." % exc.code),
                details + [(m, m)])
    except Exception as exc:
        return (False, ("Authentification OK, mais la recherche echoue.",
                        "Authentication OK, but the search fails."),
                details + [(str(exc), str(exc))])
    if not res:
        return (True, ("Cle valide, mais la recherche test n'a rien renvoye.",
                       "Valid key, but the test search returned nothing."),
                details + [("Requete test : \"%s\" (langue %s) -> 0 resultat."
                            % (probe, language),
                            "Test query: \"%s\" (language %s) -> 0 result."
                            % (probe, language))])
    top = res[0]
    details.append(("Requete test : \"%s\" (langue %s) -> %d resultat(s)."
                    % (probe, language, len(res)),
                    "Test query: \"%s\" (language %s) -> %d result(s)."
                    % (probe, language, len(res))))
    details.append(("Premier resultat : %s (%s)" % (top["title"], top["year"] or "?"),
                    "First result: %s (%s)" % (top["title"], top["year"] or "?")))
    return (True, ("Cle valide -- %s." % label[0], "Valid key -- %s." % label[1]),
            details)


def _mkv_norm_key(s):
    return re.sub(r"[^a-z0-9]+", " ", mkv_strip_accents(str(s)).lower()).strip()


def mkv_tmdb_year_for(api_key, title, cache, language="fr-FR"):
    """(annee, origine, candidats). origine = cache / tmdb / ''."""
    q = _mkv_norm_key(title)
    if not q or not (api_key or "").strip():
        return "", "", []          # sans cle : ne rien mettre en cache
    ck = "%s|%s" % (language, q)
    hit = cache.get(ck) if cache else None
    if hit:
        return hit["year"], "cache", hit.get("results") or []
    res = mkv_tmdb_search(api_key, title, language=language)
    if not res:
        return "", "", []          # echec non mis en cache : nouvel essai possible
    exact = [r for r in res if _mkv_norm_key(r["title"]) == q
             or _mkv_norm_key(r["original_title"]) == q]
    pool = sorted(exact or res, key=lambda r: (r.get("votes") or 0,
                                               r.get("popularity") or 0), reverse=True)
    best = pool[0]
    if cache:
        cache.put(ck, best["year"], best["title"], pool)
    return best["year"], "tmdb", pool


# ---------------------------------------------------------------------
#  Renommage + journal d'annulation
# ---------------------------------------------------------------------
def mkv_companion_files(video_path):
    """Fichiers annexes partageant le meme radical (srt, nfo, jpg...)."""
    p = Path(video_path)
    stem = p.stem
    out = []
    try:
        for f in p.parent.iterdir():
            if not f.is_file() or f == p:
                continue
            if f.suffix.lower() not in MKV_COMPANION_EXTS:
                continue
            if f.name.startswith(stem + ".") or f.stem == stem:
                out.append(f)
    except Exception:
        pass
    return out


def mkv_journal_append(batch_id, records):
    try:
        with open(MKV_JOURNAL_PATH, "a", encoding="utf-8") as fh:
            for old, new in records:
                fh.write(json.dumps({"batch": batch_id, "ts": int(time.time()),
                                     "old": str(old), "new": str(new)},
                                    ensure_ascii=False) + "\n")
    except Exception:
        pass


def mkv_journal_last_batch():
    if not MKV_JOURNAL_PATH.exists():
        return None, []
    try:
        lines = MKV_JOURNAL_PATH.read_text(encoding="utf-8").strip().splitlines()
    except Exception:
        return None, []
    recs = []
    for ln in lines:
        try:
            recs.append(json.loads(ln))
        except Exception:
            continue
    if not recs:
        return None, []
    last = recs[-1]["batch"]
    return last, [r for r in recs if r.get("batch") == last]


def mkv_journal_drop_batch(batch_id):
    if not MKV_JOURNAL_PATH.exists():
        return
    try:
        lines = MKV_JOURNAL_PATH.read_text(encoding="utf-8").strip().splitlines()
        keep = []
        for ln in lines:
            try:
                if json.loads(ln).get("batch") != batch_id:
                    keep.append(ln)
            except Exception:
                keep.append(ln)
        MKV_JOURNAL_PATH.write_text("\n".join(keep) + ("\n" if keep else ""),
                                    encoding="utf-8")
    except Exception:
        pass


def mkv_rename_one(src, new_name, rename_companions=True):
    """Renomme le fichier (+ annexes) -> (ok, (msg_fr, msg_en), [(old, new)])."""
    src = Path(src)
    dst = src.parent / new_name
    if str(dst) == str(src):
        return True, ("deja au bon nom", "already correctly named"), []
    if dst.exists():
        return False, ("cible existante : %s" % dst.name,
                       "target exists: %s" % dst.name), []
    if len(str(dst)) > 255 and MKV_IS_WIN:
        n = len(str(dst))
        return False, ("chemin trop long (%d caracteres)" % n,
                       "path too long (%d characters)" % n), []
    done = []
    try:
        os.rename(str(src), str(dst))
        done.append((str(src), str(dst)))
    except Exception as exc:
        return False, ("echec : %s" % exc, "failed: %s" % exc), []
    if rename_companions:
        new_stem = dst.stem
        for f in mkv_companion_files(src):
            suffix = f.name[len(src.stem):]          # ".fr.srt", ".nfo", ...
            target = f.parent / (new_stem + suffix)
            if target.exists() or str(target) == str(f):
                continue
            try:
                os.rename(str(f), str(target))
                done.append((str(f), str(target)))
            except Exception:
                pass
    return True, ("renomme", "renamed"), done


# ---------------------------------------------------------------------
#  Selection de dossier (sous-processus tkinter : evite le gel de la fenetre)
# ---------------------------------------------------------------------
def mkv_pick_folder_child():
    """Processus enfant lance avec --pick-folder <initial> <fichier_sortie>."""
    argv = sys.argv
    i = argv.index("--pick-folder")
    initial = argv[i + 1] if len(argv) > i + 1 else ""
    outfile = argv[i + 2] if len(argv) > i + 2 else ""
    path = ""
    try:
        import tkinter as tk
        from tkinter import filedialog
        root = tk.Tk()
        root.withdraw()
        try:
            root.attributes("-topmost", True)
        except Exception:
            pass
        path = filedialog.askdirectory(
            initialdir=initial if initial and os.path.isdir(initial) else None,
            title="Choisir le dossier de films / Choose the movie folder") or ""
        root.destroy()
    except Exception:
        path = ""
    if outfile:
        try:
            with open(outfile, "w", encoding="utf-8") as fh:
                fh.write(path)
        except Exception:
            pass
    return 0


def mkv_ask_folder(initial=""):
    """Bloquant : a appeler depuis un thread de travail, jamais depuis l'UI."""
    import tempfile
    fd, tmp = tempfile.mkstemp(suffix=".txt", prefix="mkvpick_")
    os.close(fd)
    try:
        cmd = [sys.executable]
        if not getattr(sys, "frozen", False):
            cmd.append(os.path.abspath(__file__))
        cmd += ["--pick-folder", initial or "", tmp]
        subprocess.run(cmd, timeout=600, creationflags=MKV_NO_WINDOW)
        with open(tmp, "r", encoding="utf-8") as fh:
            return fh.read().strip()
    except Exception:
        try:                                    # repli : tkinter dans ce thread
            import tkinter as tk
            from tkinter import filedialog
            root = tk.Tk()
            root.withdraw()
            p = filedialog.askdirectory(
                title="Choisir le dossier de films / Choose the movie folder") or ""
            root.destroy()
            return p
        except Exception:
            return ""
    finally:
        try:
            os.unlink(tmp)
        except Exception:
            pass


def mkv_fmt_duration(seconds):
    try:
        s = int(float(seconds))
    except Exception:
        return "?"
    return "%dh%02dm%02ds" % (s // 3600, (s % 3600) // 60, s % 60)


# =====================================================================
#  Onglet MKV Renamer : etat, widgets, workers  (espace de noms isole)
# =====================================================================
def _init_mkv():
    COL_OK = (46, 204, 113)
    COL_WARN = (235, 185, 90)
    COL_ERR = (235, 110, 110)
    COL_MUTED = (136, 136, 170)
    ACCENT = (233, 69, 96)

    M = {
        "lang": "fr",
        "rows": [],
        "cfg": mkv_load_config(),
        "cache": MkvTmdbCache(MKV_DB_PATH),
        "ffprobe": "",
        "busy": False,
        "stop": threading.Event(),
        "uid": 0,
        "log": [],            # [(horodatage, fr, en)]
        "status": ("Pret.", "Ready."),
        "status_color": COL_OK,
        "progress": (0.0, ("", "")),
        "detail_row": None,
        "themes": {},
    }

    def mx(fr, en):
        return en if M["lang"] == "en" else fr

    def mp(pair):
        """Rend une paire (fr, en) -- tolere une chaine simple."""
        if isinstance(pair, tuple) and len(pair) == 2:
            return mx(pair[0], pair[1])
        return str(pair or "")

    def post(fn, *args, **kwargs):
        """Execute fn dans le thread UI (file du toolbox)."""
        ui_post(lambda: fn(*args, **kwargs))

    def sv(tag, value):
        if dpg.does_item_exist(tag):
            try:
                dpg.set_value(tag, value)
            except Exception:
                pass

    def cfgi(tag, **kw):
        if dpg.does_item_exist(tag):
            try:
                dpg.configure_item(tag, **kw)
            except Exception:
                pass

    def gv(tag, default=""):
        return dpg.get_value(tag) if dpg.does_item_exist(tag) else default

    # -- registres de traduction ---------------------------------------
    TR = {
        "mk_t_title":     ("MKV Renamer", "MKV Renamer"),
        "mk_t_subtitle":  ("- renomme les films : Titre.Annee.Langue.Resolution.Source.Codec",
                           "- renames movies: Title.Year.Language.Resolution.Source.Codec"),
        "mk_t_folder":    ("Dossier", "Folder"),
        "mk_t_ffstate":   ("Moteur ffprobe :", "ffprobe engine:"),
        "mk_btn_getff":   ("Telecharger ffmpeg", "Download ffmpeg"),
        "mk_btn_browse_ff": ("Choisir ffprobe...", "Choose ffprobe..."),
        "mk_btn_browse":  ("Parcourir", "Browse"),
        "mk_cb_recursive": ("Sous-dossiers", "Subfolders"),
        "mk_t_exts":      ("Extensions", "Extensions"),
        "mk_btn_scan":    ("Analyser", "Analyse"),
        "mk_btn_stop":    ("Stop", "Stop"),
        "mk_opt_header":  ("Options de renommage", "Renaming options"),
        "mk_t_tmdb":      ("Cle TMDB commune :", "Common TMDB key:"),
        "mk_btn_test_key": ("Tester la cle", "Test the key"),
        "mk_cb_replace_year": ("Autoriser TMDB a remplacer une annee deja presente",
                               "Allow TMDB to replace a year already present"),
        "mk_btn_clear_cache": ("Vider le cache TMDB", "Clear the TMDB cache"),
        "mk_t_yearnote":  ("Par defaut TMDB ne comble que les annees manquantes : une annee lue "
                           "dans le nom de fichier ou saisie a la main n'est jamais ecrasee.",
                           "By default TMDB only fills in missing years: a year read from the "
                           "file name or typed by hand is never overwritten."),
        "mk_t_sep":       ("Separateur", "Separator"),
        "mk_t_codecstyle": ("Style codec", "Codec style"),
        "mk_t_tokenvf":   ("Token VF", "VF token"),
        "mk_t_defsrc":    ("Source par defaut", "Default source"),
        "mk_cb_dots":     ("Points dans le titre", "Dots in the title"),
        "mk_cb_10bit":    ("Ajouter 10bit", "Add 10bit"),
        "mk_cb_hdr":      ("Ajouter HDR", "Add HDR"),
        "mk_cb_companions": ("Renommer les fichiers associes (srt, nfo...)",
                             "Rename companion files (srt, nfo...)"),
        "mk_t_ffprobe":   ("ffprobe", "ffprobe"),
        "mk_btn_apply":   ("Appliquer et recalculer", "Apply and recompute"),
        "mk_btn_all":     ("Tout cocher", "Check all"),
        "mk_btn_none":    ("Tout decocher", "Uncheck all"),
        "mk_btn_recompute": ("Recalculer les noms", "Recompute the names"),
        "mk_btn_years":   ("Chercher les annees (TMDB)", "Fetch the years (TMDB)"),
        "mk_btn_rename":  ("RENOMMER LA SELECTION", "RENAME THE SELECTION"),
        "mk_btn_undo":    ("Annuler le dernier lot", "Undo the last batch"),
        "mk_detail_window": ("Details du fichier", "File details"),
        "mk_btn_close_detail": ("Fermer", "Close"),
    }
    HINTS = {
        "mk_in_folder":  (r"D:\Films", r"D:\Movies"),
        "mk_in_exts":    (".mkv, .mp4", ".mkv, .mp4"),
        "mk_in_ffprobe": ("laisser vide pour detection automatique",
                          "leave empty for automatic detection"),
    }
    COLS = {
        "mk_col_check":  ("", ""),
        "mk_col_orig":   ("Fichier actuel", "Current file"),
        "mk_col_title":  ("Titre", "Title"),
        "mk_col_year":   ("Annee", "Year"),
        "mk_col_lang":   ("Langue", "Language"),
        "mk_col_res":    ("Res.", "Res."),
        "mk_col_source": ("Source", "Source"),
        "mk_col_codec":  ("Codec", "Codec"),
        "mk_col_new":    ("Nouveau nom", "New name"),
        "mk_col_status": ("Statut", "Status"),
        "mk_col_det":    ("", ""),
    }
    TIPS = []       # [(tag_texte, fr, en)]
    _tipn = [0]

    def tip(fr, en, wrap=320):
        _tipn[0] += 1
        tag = "mk_tip_%d" % _tipn[0]
        with dpg.tooltip(dpg.last_item()):
            dpg.add_text(mx(fr, en), tag=tag, wrap=wrap)
        TIPS.append((tag, fr, en))

    CONF_TR = {"high": ("forte", "high"), "medium": ("moyenne", "medium"),
               "low": ("faible", "low"), "none": ("aucune", "none"), "": ("?", "?")}
    ORIGIN_TR = {
        "name": ("Annee %s -- lue dans le nom de fichier.\nTMDB ne la remplacera pas.",
                 "Year %s -- read from the file name.\nTMDB will not replace it."),
        "manual": ("Annee %s -- saisie manuellement.\nTMDB ne la remplacera pas.",
                   "Year %s -- typed by hand.\nTMDB will not replace it."),
        "tmdb": ("Annee %s -- trouvee sur TMDB.\nModifiable, et re-interrogeable.",
                 "Year %s -- found on TMDB.\nEditable, and can be queried again."),
    }

    def none_token():
        return mx(MKV_NONE_FR, MKV_NONE_EN)

    def lang_choices():
        items = list(MKV_LANG_CHOICES)
        tok = (M["cfg"]["format"]["token_vf"] or "").strip()
        if tok and tok not in items:
            items.insert(1, tok)
        return items + [none_token()]

    def source_choices():
        return list(MKV_SOURCE_CHOICES) + [none_token()]

    def year_origin_label(row):
        if not row.year:
            return mx("Aucune annee. TMDB comblera cette case,\nou saisis-la a la main.",
                      "No year. TMDB will fill this cell,\nor type it by hand.")
        pair = ORIGIN_TR.get(row.year_origin,
                             ("Annee %s -- origine inconnue.", "Year %s -- unknown origin."))
        return mx(pair[0], pair[1]) % row.year

    # -- ligne de la table ---------------------------------------------
    class Row(object):
        __slots__ = ("path", "orig_name", "ext", "info", "title", "year", "lang",
                     "resolution", "source", "codec", "conf", "reasons", "lang_why",
                     "new_name", "status", "status_color", "selected",
                     "tmdb_candidates", "renamed", "uid", "year_origin")

        def __init__(self, path):
            M["uid"] += 1
            self.uid = M["uid"]            # tags uniques : aucune collision au re-scan
            self.year_origin = ""          # name | manual | tmdb (protege de TMDB)
            self.path = str(path)
            self.orig_name = os.path.basename(str(path))
            self.ext = os.path.splitext(self.orig_name)[1]
            self.info = {}
            self.title = ""
            self.year = ""
            self.lang = ""
            self.resolution = ""
            self.source = ""
            self.codec = ""
            self.conf = ""
            self.reasons = []              # [(fr, en)]
            self.lang_why = ("", "")
            self.new_name = ""
            self.status = ("", "")         # (fr, en)
            self.status_color = COL_MUTED
            self.selected = True
            self.tmdb_candidates = []
            self.renamed = False

    def tag_of(prefix, idx):
        return "%s_%d" % (prefix, M["rows"][idx].uid)

    # -- journal / statut ----------------------------------------------
    def log(fr, en):
        M["log"].append((time.strftime("%H:%M:%S"), fr, en))
        if len(M["log"]) > 500:
            del M["log"][:-500]
        post(redraw_log)

    def redraw_log():
        sv("mk_log", "\n".join("[%s] %s" % (s, mx(fr, en)) for s, fr, en in M["log"]))

    def set_status(fr, en, color=None):
        M["status"] = (fr, en)
        M["status_color"] = color or COL_OK
        for t in ("mk_status", "mk_status_top"):
            post(sv, t, mx(fr, en))
            post(cfgi, t, color=M["status_color"])

    def set_progress(frac, fr="", en=""):
        M["progress"] = (max(0.0, min(1.0, frac)), (fr, en))
        post(sv, "mk_progress", M["progress"][0])
        post(cfgi, "mk_progress", overlay=mx(fr, en))

    def set_busy(busy):
        M["busy"] = busy
        for t in ("mk_btn_scan", "mk_btn_rename", "mk_btn_undo", "mk_btn_browse",
                  "mk_btn_years", "mk_btn_test_key", "mk_btn_recompute",
                  "mk_btn_apply"):
            post(cfgi, t, enabled=not busy)
        post(cfgi, "mk_btn_stop", show=busy)

    # -- configuration <-> widgets -------------------------------------
    def read_widgets_to_cfg():
        c = M["cfg"]
        c["general"]["dossier"] = gv("mk_in_folder") or ""
        c["general"]["recursif"] = "1" if gv("mk_cb_recursive") else "0"
        c["general"]["extensions"] = gv("mk_in_exts") or MKV_VIDEO_EXTS_DEFAULT
        c["tmdb"]["remplacer_annee_du_nom"] = "1" if gv("mk_cb_replace_year") else "0"
        c["format"]["separateur"] = gv("mk_in_sep") or "."
        c["format"]["style_codec"] = gv("mk_cmb_codec_style") or "X265"
        c["format"]["token_vf"] = gv("mk_in_token_vf") or "VF"
        c["format"]["points_dans_titre"] = "1" if gv("mk_cb_dots") else "0"
        c["format"]["ajouter_10bit"] = "1" if gv("mk_cb_10bit") else "0"
        c["format"]["ajouter_hdr"] = "1" if gv("mk_cb_hdr") else "0"
        src = gv("mk_cmb_default_source") or "WEB-DL"
        c["format"]["source_par_defaut"] = "" if src in MKV_NONE_TOKENS else src
        c["format"]["renommer_associes"] = "1" if gv("mk_cb_companions") else "0"
        c["chemins"]["ffprobe"] = gv("mk_in_ffprobe") or ""

    def apply_cfg_to_widgets():
        c = M["cfg"]
        sv("mk_in_folder", c["general"]["dossier"])
        sv("mk_cb_recursive", c["general"]["recursif"] == "1")
        sv("mk_in_exts", c["general"]["extensions"])
        sv("mk_cb_replace_year", c["tmdb"]["remplacer_annee_du_nom"] == "1")
        sv("mk_in_sep", c["format"]["separateur"])
        sv("mk_cmb_codec_style", c["format"]["style_codec"])
        sv("mk_in_token_vf", c["format"]["token_vf"])
        sv("mk_cb_dots", c["format"]["points_dans_titre"] == "1")
        sv("mk_cb_10bit", c["format"]["ajouter_10bit"] == "1")
        sv("mk_cb_hdr", c["format"]["ajouter_hdr"] == "1")
        sv("mk_cmb_default_source", c["format"]["source_par_defaut"] or none_token())
        sv("mk_cb_companions", c["format"]["renommer_associes"] == "1")
        sv("mk_in_ffprobe", c["chemins"]["ffprobe"])

    # -- cle TMDB commune ----------------------------------------------
    def tmdb_key():
        """Cle du bandeau de configuration commun (jamais une cle locale)."""
        v = (gv("sh_tmdb") or "").strip()
        if v:
            return v
        try:
            return (get_shared_creds().get("tmdb_key") or "").strip()
        except Exception:
            return ""

    def tmdb_lang():
        return "en-US" if M["lang"] == "en" else "fr-FR"

    def refresh_tmdb_state():
        """Ligne d'etat : la cle vient du bandeau commun, jamais d'ici."""
        if tmdb_key():
            sv("mk_tmdb_state", mx("renseignee dans le bandeau du haut (partagee)",
                                   "set in the top bar (shared)"))
            cfgi("mk_tmdb_state", color=COL_OK)
        else:
            sv("mk_tmdb_state", mx("absente -- renseigne-la dans le bandeau du haut",
                                   "missing -- set it in the top bar"))
            cfgi("mk_tmdb_state", color=COL_WARN)

    def render_ffprobe_state():
        """Etat de ffprobe affiche EN HAUT de l'onglet (jamais hors ecran)."""
        ff = M["ffprobe"]
        if ff:
            sv("mk_ffstate", ff)
            cfgi("mk_ffstate", color=COL_OK)
        else:
            sv("mk_ffstate", mx("INTROUVABLE -- l'analyse est impossible sans lui "
                                "(ffprobe.exe fait partie de ffmpeg)",
                                "NOT FOUND -- analysis is impossible without it "
                                "(ffprobe.exe ships with ffmpeg)"))
            cfgi("mk_ffstate", color=COL_ERR)
        cfgi("mk_btn_getff", show=not ff)
        cfgi("mk_btn_browse_ff", show=not ff)

    def refresh_ffprobe_state():
        """Redetecte ffprobe et rafraichit l'affichage. Retourne le chemin."""
        M["ffprobe"] = mkv_find_ffprobe(M["cfg"]["chemins"]["ffprobe"])
        render_ffprobe_state()
        return M["ffprobe"]

    def err_modal(fr, en):
        """Message bloquant : le journal en bas peut etre hors ecran."""
        modal_err(mx("MKV Renamer", "MKV Renamer"), mx(fr, en))

    def run_worker(fn, *args):
        """Lance un worker : une exception ne peut plus mourir en silence."""
        def wrapper():
            try:
                fn(*args)
            except Exception:
                tb = traceback.format_exc()
                for line in tb.strip().splitlines():
                    M["log"].append((time.strftime("%H:%M:%S"), line, line))
                post(redraw_log)
                last = tb.strip().splitlines()[-1]
                set_status("Erreur interne : %s" % last, "Internal error: %s" % last, COL_ERR)
                post(err_modal, "Erreur interne pendant l'operation :\n\n" + tb[-900:],
                     "Internal error during the operation:\n\n" + tb[-900:])
                set_busy(False)
                set_progress(0.0)
        threading.Thread(target=wrapper, daemon=True).start()

    def push_shared(_c=None):
        """Appele quand le bandeau commun change."""
        try:
            refresh_tmdb_state()
        except Exception:
            pass

    # -- calcul du nouveau nom -----------------------------------------
    def compute_name(row):
        c = M["cfg"]
        hdr = "HDR" if (c["format"]["ajouter_hdr"] == "1"
                        and row.info.get("is_hdr")) else ""
        tenb = "10bit" if (c["format"]["ajouter_10bit"] == "1"
                           and row.info.get("is_10bit")) else ""
        return mkv_build_name(row.title, row.year, row.lang, row.resolution,
                              row.source, row.codec, row.ext,
                              sep=c["format"]["separateur"] or ".",
                              dots_in_title=c["format"]["points_dans_titre"] == "1",
                              hdr=hdr, tenbit=tenb)

    def set_row_status(idx, fr, en, color):
        row = M["rows"][idx]
        row.status = (fr, en)
        row.status_color = color
        t = tag_of("mk_txt_status", idx)
        sv(t, mx(fr, en))
        cfgi(t, color=color)

    def refresh_row_name(idx):
        row = M["rows"][idx]
        row.new_name = compute_name(row)
        sv(tag_of("mk_txt_new", idx), row.new_name)
        if row.renamed:
            return
        if row.new_name == row.orig_name:
            set_row_status(idx, "identique", "identical", COL_MUTED)
        elif os.path.exists(os.path.join(os.path.dirname(row.path), row.new_name)):
            set_row_status(idx, "cible existante", "target exists", COL_ERR)
        elif not row.year:
            set_row_status(idx, "annee manquante", "missing year", COL_WARN)
        elif not row.lang:
            set_row_status(idx, "langue indeterminee", "undetermined language", COL_WARN)
        elif row.conf == "low":
            set_row_status(idx, "source incertaine", "uncertain source", COL_WARN)
        else:
            set_row_status(idx, "pret", "ready", COL_OK)

    # -- callbacks (DearPyGui exige 3 arguments) -----------------------
    def cb_browse(sender=None, app_data=None, user_data=None):
        if M["busy"]:
            return
        run_worker(_browse_worker)

    def _browse_worker():
        p = mkv_ask_folder(gv("mk_in_folder") or "")
        if p:
            post(sv, "mk_in_folder", p)

    def cb_scan(sender=None, app_data=None, user_data=None):
        if M["busy"]:
            set_status("Operation deja en cours...", "An operation is already running...")
            return
        read_widgets_to_cfg()
        folder = (gv("mk_in_folder") or "").strip('" ')
        if not folder:
            set_status("Aucun dossier indique.", "No folder given.", COL_ERR)
            err_modal("Indique d'abord le dossier contenant les films "
                      "(bouton « Parcourir »).",
                      "First give the folder holding the movies (\"Browse\" button).")
            return
        if not os.path.isdir(folder):
            log("Dossier invalide : %s" % folder, "Invalid folder: %s" % folder)
            set_status("Dossier introuvable.", "Folder not found.", COL_ERR)
            err_modal("Ce dossier est introuvable :\n%s" % folder,
                      "This folder does not exist:\n%s" % folder)
            return
        if not refresh_ffprobe_state():
            log("ffprobe introuvable : analyse impossible.",
                "ffprobe not found: analysis impossible.")
            set_status("ffprobe introuvable.", "ffprobe not found.", COL_ERR)
            err_modal(
                "ffprobe est INTROUVABLE : l'onglet ne peut pas lire les fichiers, "
                "donc l'analyse ne demarre pas.\n\n"
                "ffprobe.exe fait partie de ffmpeg. Trois solutions :\n\n"
                "  1. Bouton « Telecharger ffmpeg » en haut de l'onglet, puis "
                "decompresser l'archive et\n     copier ffprobe.exe a cote de ce "
                "programme (ou dans un sous-dossier bin\\).\n\n"
                "  2. Bouton « Choisir ffprobe... » si ffmpeg est deja installe "
                "quelque part sur le PC.\n\n"
                "  3. Installer ffmpeg dans le PATH Windows "
                "(winget install Gyan.FFmpeg).",
                "ffprobe was NOT FOUND: this tab cannot read the files, so the "
                "analysis does not start.\n\n"
                "ffprobe.exe ships with ffmpeg. Three options:\n\n"
                "  1. \"Download ffmpeg\" button at the top of the tab, then unzip "
                "the archive and copy\n     ffprobe.exe next to this program (or into "
                "a bin\\ subfolder).\n\n"
                "  2. \"Choose ffprobe...\" button if ffmpeg is already installed "
                "somewhere on the PC.\n\n"
                "  3. Install ffmpeg into the Windows PATH "
                "(winget install Gyan.FFmpeg).")
            return
        mkv_save_config(M["cfg"])
        M["stop"].clear()
        log("Analyse demandee : %s" % folder, "Analysis requested: %s" % folder)
        set_status("Analyse en cours...", "Analysis in progress...")
        run_worker(_scan_worker, folder)

    def cb_stop(sender=None, app_data=None, user_data=None):
        M["stop"].set()
        log("Interruption demandee.", "Interruption requested.")

    def cb_select_all(sender=None, app_data=None, user_data=None):
        value = bool(user_data)
        for idx, row in enumerate(M["rows"]):
            if row.renamed:
                continue
            row.selected = value
            sv(tag_of("mk_cb_row", idx), value)

    def cb_row_check(sender=None, app_data=None, user_data=None):
        idx = user_data
        if 0 <= idx < len(M["rows"]):
            M["rows"][idx].selected = bool(app_data)

    def cb_row_title(sender=None, app_data=None, user_data=None):
        idx = user_data
        if 0 <= idx < len(M["rows"]):
            M["rows"][idx].title = app_data
            refresh_row_name(idx)

    def cb_row_year(sender=None, app_data=None, user_data=None):
        idx = user_data
        if 0 <= idx < len(M["rows"]):
            row = M["rows"][idx]
            row.year = re.sub(r"\D", "", app_data or "")[:4]
            row.year_origin = "manual" if row.year else ""
            sv("mk_tip_year_%d" % row.uid, year_origin_label(row))
            refresh_row_name(idx)

    def cb_row_lang(sender=None, app_data=None, user_data=None):
        idx = user_data
        if 0 <= idx < len(M["rows"]):
            M["rows"][idx].lang = "" if app_data in MKV_NONE_TOKENS else app_data
            refresh_row_name(idx)

    def cb_row_source(sender=None, app_data=None, user_data=None):
        idx = user_data
        if 0 <= idx < len(M["rows"]):
            M["rows"][idx].source = "" if app_data in MKV_NONE_TOKENS else app_data
            refresh_row_name(idx)

    def detail_text(row):
        i = row.info
        lines = [
            "%s : %s" % (mx("Fichier      ", "File         "), row.orig_name),
            "%s : %s" % (mx("Dossier      ", "Folder       "), os.path.dirname(row.path)),
            "",
            "%s : %s  %dx%d  (%s)" % (mx("Video        ", "Video        "),
                                      i.get("video_codec", "?"), i.get("width", 0),
                                      i.get("height", 0), row.resolution),
            "%s : %.2f Mb/s" % (mx("Debit total  ", "Total bitrate"), i.get("mbps") or 0.0),
            "%s : %s" % (mx("Duree        ", "Duration     "),
                         mkv_fmt_duration(i.get("duration") or 0)),
            "%s : %s%s" % (mx("Pixels       ", "Pixels       "), i.get("pix_fmt", "?"),
                           "  HDR" if i.get("is_hdr") else ""),
            "",
            "%s : %s" % (mx("Audio        ", "Audio        "), ", ".join(
                "%s [%s]%s" % (c, l or "?", (" \"%s\"" % t) if t else "")
                for c, l, t in zip(i.get("audio_codecs", []), i.get("audio_langs", []),
                                   i.get("audio_titles", []))) or mx("aucune", "none")),
            "%s : %s" % (mx("Sous-titres  ", "Subtitles    "), ", ".join(
                "%s [%s]" % (c, l or "?")
                for c, l in zip(i.get("sub_codecs", []),
                                i.get("sub_langs", []))) or mx("aucun", "none")),
            "%s : %d" % (mx("Chapitres    ", "Chapters     "), i.get("n_chapters", 0)),
            "%s : %s" % (mx("Tags conteneur", "Container tags"),
                         json.dumps(i.get("tags", {}), ensure_ascii=False)
                         if i.get("tags") else mx("aucun", "none")),
            "",
            "%s -> %s" % (mx("Annee", "Year"), year_origin_label(row).replace("\n", " ")),
            "",
            "%s -> %s (%s)" % (mx("Langue", "Language"), row.lang or "?", mp(row.lang_why)),
            "",
            "%s -> %s (%s %s)" % (mx("Source", "Source"), row.source or "?",
                                  mx("confiance", "confidence"),
                                  mp(CONF_TR.get(row.conf, CONF_TR[""]))),
        ] + ["   " + mp(r) for r in row.reasons]
        return "\n".join(lines)

    def cb_row_detail(sender=None, app_data=None, user_data=None):
        idx = user_data
        if not (0 <= idx < len(M["rows"])):
            return
        M["detail_row"] = M["rows"][idx]
        sv("mk_detail_text", detail_text(M["detail_row"]))
        cfgi("mk_detail_window", show=True)

    def cb_close_detail(sender=None, app_data=None, user_data=None):
        cfgi("mk_detail_window", show=False)

    def cb_recompute(sender=None, app_data=None, user_data=None):
        read_widgets_to_cfg()
        mkv_save_config(M["cfg"])
        style = M["cfg"]["format"]["style_codec"]
        token_vf = (M["cfg"]["format"]["token_vf"] or "").strip() or "VF"
        items = lang_choices()
        for idx, row in enumerate(M["rows"]):
            if row.renamed or not row.info:
                continue
            row.codec = mkv_codec_token(row.info.get("video_codec"), style)
            lang_tag = tag_of("mk_cmb_lang", idx)
            cfgi(lang_tag, items=items)
            if row.lang in ("VF", "TRUEFRENCH", "VFF", "VFQ") and row.lang != token_vf:
                row.lang = token_vf
                sv(lang_tag, token_vf)
            sv(tag_of("mk_txt_codec", idx), row.codec)
            refresh_row_name(idx)
        refresh_ffprobe_state()
        set_status("Noms recalcules.", "Names recomputed.")

    def cb_fetch_years(sender=None, app_data=None, user_data=None):
        if M["busy"]:
            return
        read_widgets_to_cfg()
        mkv_save_config(M["cfg"])
        if not tmdb_key():
            log("Aucune cle TMDB : renseigne-la dans le bandeau de configuration commun.",
                "No TMDB key: set it in the common configuration bar.")
            set_status("Cle TMDB manquante (bandeau du haut).",
                       "Missing TMDB key (top bar).", COL_WARN)
            return
        M["stop"].clear()
        run_worker(_years_worker, True)

    def cb_rename(sender=None, app_data=None, user_data=None):
        if M["busy"]:
            return
        read_widgets_to_cfg()
        mkv_save_config(M["cfg"])
        targets = [(i, r) for i, r in enumerate(M["rows"])
                   if r.selected and not r.renamed and r.new_name]
        if not targets:
            set_status("Rien a renommer.", "Nothing to rename.", COL_WARN)
            return
        run_worker(_rename_worker, targets)

    def cb_undo(sender=None, app_data=None, user_data=None):
        if M["busy"]:
            return
        run_worker(_undo_worker)

    def cb_clear_cache(sender=None, app_data=None, user_data=None):
        ok = M["cache"].clear()
        if ok:
            log("Cache TMDB vide.", "TMDB cache cleared.")
        else:
            log("Cache TMDB : echec du vidage.", "TMDB cache: clearing failed.")

    def cb_browse_ffprobe(sender=None, app_data=None, user_data=None):
        """Selection manuelle de ffprobe.exe (meme approche que le lecteur video)."""
        try:
            import tkinter as tk
            from tkinter import filedialog
            r = tk.Tk(); r.withdraw(); r.attributes("-topmost", True)
            fp = filedialog.askopenfilename(
                title=mx("Emplacement de ffprobe", "Location of ffprobe"),
                filetypes=([("ffprobe", "ffprobe.exe"), ("Executables", "*.exe"),
                            ("Tous les fichiers", "*.*")] if MKV_IS_WIN
                           else [("ffprobe", "ffprobe"), ("Tous les fichiers", "*")]))
            r.destroy()
        except Exception:
            fp = ""
        if fp:
            sv("mk_in_ffprobe", fp)
            M["cfg"]["chemins"]["ffprobe"] = fp
            mkv_save_config(M["cfg"])
            if refresh_ffprobe_state():
                set_status("ffprobe trouve : l'analyse est possible.",
                           "ffprobe found: analysis is possible.")
            else:
                err_modal("Ce fichier n'est pas utilisable comme ffprobe :\n%s" % fp,
                          "This file cannot be used as ffprobe:\n%s" % fp)

    def cb_get_ffmpeg(sender=None, app_data=None, user_data=None):
        try:
            webbrowser.open("https://ffmpeg.org/download.html")
        except Exception:
            pass

    def cb_test_key(sender=None, app_data=None, user_data=None):
        if M["busy"]:
            return
        sv("mk_tmdb_state", mx("Test en cours...", "Test in progress..."))
        cfgi("mk_tmdb_state", color=COL_WARN)
        run_worker(_test_key_worker)

    def _test_key_worker():
        set_busy(True)
        try:
            t0 = time.time()
            ok, resume, details = mkv_tmdb_check_key(tmdb_key(), language=tmdb_lang())
            elapsed = time.time() - t0
            post(sv, "mk_tmdb_state", mp(resume))
            post(cfgi, "mk_tmdb_state", color=COL_OK if ok else COL_ERR)
            log("Test de la cle TMDB : %s (%.1f s)" % (resume[0], elapsed),
                "TMDB key test: %s (%.1f s)" % (resume[1], elapsed))
            for d in details:
                log("   " + d[0], "   " + d[1])
            set_status(resume[0], resume[1])
        finally:
            set_busy(False)

    # -- table ----------------------------------------------------------
    def clear_table():
        if dpg.does_item_exist("mk_table"):
            try:
                dpg.delete_item("mk_table", children_only=True, slot=1)
            except Exception:
                pass

    def add_row_widgets(idx):
        row = M["rows"][idx]
        u = row.uid
        with dpg.table_row(parent="mk_table"):
            dpg.add_checkbox(tag="mk_cb_row_%d" % u, default_value=row.selected,
                             callback=cb_row_check, user_data=idx)
            dpg.add_text(row.orig_name, tag="mk_txt_orig_%d" % u)
            dpg.add_input_text(tag="mk_in_title_%d" % u, default_value=row.title,
                               width=-1, callback=cb_row_title, user_data=idx,
                               on_enter=False)
            dpg.add_input_text(tag="mk_in_year_%d" % u, default_value=row.year,
                               width=-1, callback=cb_row_year, user_data=idx)
            dpg.add_combo(lang_choices(), tag="mk_cmb_lang_%d" % u,
                          default_value=row.lang or none_token(), width=-1,
                          callback=cb_row_lang, user_data=idx)
            dpg.add_text(row.resolution, tag="mk_txt_res_%d" % u)
            dpg.add_combo(source_choices(), tag="mk_cmb_src_%d" % u,
                          default_value=row.source or none_token(), width=-1,
                          callback=cb_row_source, user_data=idx)
            dpg.add_text(row.codec, tag="mk_txt_codec_%d" % u)
            dpg.add_text(row.new_name, tag="mk_txt_new_%d" % u)
            dpg.add_text(mp(row.status), tag="mk_txt_status_%d" % u,
                         color=row.status_color)
            dpg.add_button(label=mx("Details", "Details"), tag="mk_btn_det_%d" % u,
                           small=True, callback=cb_row_detail, user_data=idx)
        # hors du contexte table_row : une infobulle n'est pas une cellule
        with dpg.tooltip(parent="mk_in_year_%d" % u):
            dpg.add_text(year_origin_label(row), tag="mk_tip_year_%d" % u)
        if row.info and not row.status[0]:
            refresh_row_name(idx)

    # -- workers --------------------------------------------------------
    def _scan_worker(folder):
        set_busy(True)
        try:
            post(refresh_ffprobe_state)
            if not M["ffprobe"]:
                log("ffprobe introuvable. Installe ffmpeg ou indique le chemin dans "
                    "les options.",
                    "ffprobe not found. Install ffmpeg or set the path in the options.")
                set_status("ffprobe introuvable.", "ffprobe not found.", COL_ERR)
                return
            exts = tuple(e.strip().lower() if e.strip().startswith(".")
                         else "." + e.strip().lower()
                         for e in (M["cfg"]["general"]["extensions"] or ".mkv").split(",")
                         if e.strip())
            recursive = M["cfg"]["general"]["recursif"] == "1"
            files = []
            if recursive:
                for base, _dirs, names in os.walk(folder):
                    for n in sorted(names):
                        if n.lower().endswith(exts):
                            files.append(os.path.join(base, n))
            else:
                for n in sorted(os.listdir(folder)):
                    p = os.path.join(folder, n)
                    if os.path.isfile(p) and n.lower().endswith(exts):
                        files.append(p)
            log("%d fichier(s) trouve(s) dans %s" % (len(files), folder),
                "%d file(s) found in %s" % (len(files), folder))
            if not files:
                set_status("Aucun fichier correspondant dans ce dossier.",
                           "No matching file in this folder.", COL_WARN)
                post(clear_table)
                post(err_modal,
                     "Aucun fichier ne correspond dans :\n%s\n\nExtensions cherchees : "
                     "%s\nCoche « Sous-dossiers » si les films sont ranges par dossier."
                     % (folder, ", ".join(exts) or ".mkv"),
                     "No matching file in:\n%s\n\nExtensions searched: %s\nTick "
                     "\"Subfolders\" if the movies are stored in one folder each."
                     % (folder, ", ".join(exts) or ".mkv"))
                return

            M["rows"] = []
            post(clear_table)
            style = M["cfg"]["format"]["style_codec"]
            token_vf = M["cfg"]["format"]["token_vf"]
            default_source = M["cfg"]["format"]["source_par_defaut"]

            for n, path in enumerate(files):
                if M["stop"].is_set():
                    log("Analyse interrompue.", "Analysis interrupted.")
                    break
                set_progress((n + 1) / float(len(files)),
                             "Analyse %d/%d" % (n + 1, len(files)),
                             "Analysing %d/%d" % (n + 1, len(files)))
                row = Row(path)
                try:
                    data = mkv_run_ffprobe(M["ffprobe"], path)
                    row.info = mkv_analyse_probe(data, row.orig_name)
                except Exception as exc:
                    pair = mkv_pair(exc)
                    row.info = {}
                    row.status = pair
                    row.status_color = COL_ERR
                    log("%s : %s" % (row.orig_name, pair[0]),
                        "%s: %s" % (row.orig_name, pair[1]))
                if row.info:
                    row.title = mkv_clean_title(row.orig_name)
                    row.year = mkv_extract_year(row.orig_name)
                    row.year_origin = "name" if row.year else ""
                    row.resolution = row.info.get("resolution", "")
                    row.codec = mkv_codec_token(row.info.get("video_codec"), style)
                    row.lang, row.lang_why = mkv_language_token(row.info, token_vf)
                    src, conf, reasons = mkv_guess_source(row.info)
                    row.source = src or default_source
                    row.conf = conf if src else "none"
                    row.reasons = reasons
                    row.new_name = compute_name(row)
                M["rows"].append(row)
                post(add_row_widgets, len(M["rows"]) - 1)

            set_progress(0.0)
            set_status("%d fichier(s) analyse(s)." % len(M["rows"]),
                       "%d file(s) analysed." % len(M["rows"]))
            kept = len([r for r in M["rows"] if r.info and r.year_origin == "name"])
            if kept:
                log("%d annee(s) lue(s) dans le nom de fichier : conservee(s) telle(s) "
                    "quelle(s)." % kept,
                    "%d year(s) read from the file name: kept as is." % kept)
            need_year = [r for r in M["rows"] if r.info and not r.year]
            if need_year and tmdb_key():
                _years_worker(force=False, keep_busy=True)
            elif need_year:
                log("%d fichier(s) sans annee. Renseigne une cle TMDB dans le bandeau "
                    "du haut pour la recuperer automatiquement." % len(need_year),
                    "%d file(s) without a year. Set a TMDB key in the top bar to fetch "
                    "it automatically." % len(need_year))
        finally:
            set_busy(False)
            set_progress(0.0)

    def _years_worker(force=False, keep_busy=False):
        if not keep_busy:
            set_busy(True)
        try:
            key = tmdb_key()
            lang = tmdb_lang()
            allow_replace = M["cfg"]["tmdb"]["remplacer_annee_du_nom"] == "1"
            todo, protected = [], 0
            for i, r in enumerate(M["rows"]):
                if not r.info or r.renamed:
                    continue
                if r.year and r.year_origin in ("name", "manual") and not allow_replace:
                    protected += 1          # jamais ecrase par TMDB
                    continue
                if r.year and not force:
                    continue                # deja renseignee, pas de forcage
                todo.append((i, r))
            if protected:
                log("%d annee(s) protegee(s) (nom de fichier ou saisie manuelle) : "
                    "TMDB ne les touche pas." % protected,
                    "%d protected year(s) (file name or manual entry): TMDB leaves "
                    "them alone." % protected)
            if not todo:
                set_status("Aucune annee a chercher : %d protegee(s), le reste est deja "
                           "renseigne." % protected,
                           "No year to look up: %d protected, the rest is already "
                           "filled in." % protected)
                return
            log("Recherche TMDB pour %d titre(s)." % len(todo),
                "TMDB lookup for %d title(s)." % len(todo))
            found = 0
            for n, (idx, row) in enumerate(todo):
                if M["stop"].is_set():
                    log("Recherche TMDB interrompue.", "TMDB lookup interrupted.")
                    break
                set_progress((n + 1) / float(len(todo)),
                             "TMDB %d/%d" % (n + 1, len(todo)),
                             "TMDB %d/%d" % (n + 1, len(todo)))
                try:
                    year, origin, cands = mkv_tmdb_year_for(key, row.title,
                                                            M["cache"], lang)
                except Exception as exc:
                    log("TMDB (%s) : %s" % (row.title, exc),
                        "TMDB (%s): %s" % (row.title, exc))
                    continue
                row.tmdb_candidates = cands
                if year:
                    found += 1
                    row.year = year
                    row.year_origin = "tmdb"
                    post(sv, "mk_in_year_%d" % row.uid, year)
                    post(sv, "mk_tip_year_%d" % row.uid, year_origin_label(row))
                    post(refresh_row_name, idx)
                    if origin == "tmdb":
                        time.sleep(0.05)          # courtoisie envers l'API
                else:
                    log("Aucun resultat TMDB pour \"%s\"." % row.title,
                        "No TMDB result for \"%s\"." % row.title)
            log("Annees trouvees : %d/%d." % (found, len(todo)),
                "Years found: %d/%d." % (found, len(todo)))
            set_status("Recherche TMDB terminee (%d/%d)." % (found, len(todo)),
                       "TMDB lookup finished (%d/%d)." % (found, len(todo)))
        finally:
            set_progress(0.0)
            if not keep_busy:
                set_busy(False)

    def _rename_worker(targets):
        set_busy(True)
        batch = "%s-%d" % (time.strftime("%Y%m%d-%H%M%S"), os.getpid())
        done_records = []
        ok_count = 0
        try:
            companions = M["cfg"]["format"]["renommer_associes"] == "1"
            claimed = set()
            for n, (idx, row) in enumerate(targets):
                set_progress((n + 1) / float(len(targets)),
                             "Renommage %d/%d" % (n + 1, len(targets)),
                             "Renaming %d/%d" % (n + 1, len(targets)))
                key = os.path.normcase(os.path.join(os.path.dirname(row.path),
                                                    row.new_name))
                if key in claimed:
                    post(set_row_status, idx, "doublon dans le lot",
                         "duplicate within the batch", COL_ERR)
                    log("KO  %s : deux fichiers visent le meme nom" % row.orig_name,
                        "KO  %s: two files target the same name" % row.orig_name)
                    continue
                claimed.add(key)
                ok, msg, recs = mkv_rename_one(row.path, row.new_name, companions)
                if ok and recs:
                    ok_count += 1
                    row.renamed = True
                    row.path = recs[0][1]
                    row.orig_name = os.path.basename(row.path)
                    done_records.extend(recs)
                    extra_fr = " (+%d annexe(s))" % (len(recs) - 1) if len(recs) > 1 else ""
                    extra_en = " (+%d companion(s))" % (len(recs) - 1) if len(recs) > 1 else ""
                    post(set_row_status, idx, "renomme" + extra_fr,
                         "renamed" + extra_en, COL_OK)
                    post(sv, "mk_txt_orig_%d" % row.uid, row.orig_name)
                    post(sv, "mk_cb_row_%d" % row.uid, False)
                    row.selected = False
                    log("OK  %s" % row.new_name, "OK  %s" % row.new_name)
                elif ok:
                    post(set_row_status, idx, msg[0], msg[1], COL_MUTED)
                else:
                    post(set_row_status, idx, msg[0], msg[1], COL_ERR)
                    log("KO  %s : %s" % (row.orig_name, msg[0]),
                        "KO  %s: %s" % (row.orig_name, msg[1]))
            if done_records:
                mkv_journal_append(batch, done_records)
            log("%d fichier(s) renomme(s)." % ok_count,
                "%d file(s) renamed." % ok_count)
            set_status("%d renomme(s). Annulation possible." % ok_count,
                       "%d renamed. Undo available." % ok_count)
        finally:
            set_progress(0.0)
            set_busy(False)

    def _undo_worker():
        set_busy(True)
        try:
            batch, recs = mkv_journal_last_batch()
            if not recs:
                set_status("Aucun lot a annuler.", "No batch to undo.", COL_WARN)
                log("Journal vide.", "Empty journal.")
                return
            restored = 0
            for r in reversed(recs):
                old, new = r.get("old"), r.get("new")
                try:
                    if os.path.exists(new) and not os.path.exists(old):
                        os.rename(new, old)
                        restored += 1
                except Exception as exc:
                    log("Annulation impossible pour %s : %s"
                        % (os.path.basename(new or ""), exc),
                        "Cannot undo %s: %s" % (os.path.basename(new or ""), exc))
            mkv_journal_drop_batch(batch)
            log("Lot %s annule : %d fichier(s) restaure(s)." % (batch, restored),
                "Batch %s undone: %d file(s) restored." % (batch, restored))
            set_status("%d fichier(s) restaure(s). Relance une analyse." % restored,
                       "%d file(s) restored. Run a new analysis." % restored)
        finally:
            set_busy(False)

    # -- bascule de langue ----------------------------------------------
    def set_lang(code):
        M["lang"] = "en" if str(code).upper() == "EN" else "fr"
        i = 1 if M["lang"] == "en" else 0
        for tag, pair in TR.items():
            if not dpg.does_item_exist(tag):
                continue
            try:
                kind = dpg.get_item_type(tag)
                if ("Button" in kind or "Collapsing" in kind or "Checkbox" in kind
                        or "Window" in kind):
                    dpg.configure_item(tag, label=pair[i])
                else:
                    dpg.set_value(tag, pair[i])
            except Exception:
                pass
        for tag, pair in HINTS.items():
            cfgi(tag, hint=pair[i])
        for tag, pair in COLS.items():
            cfgi(tag, label=pair[i])
        for tag, fr, en in TIPS:
            sv(tag, en if i else fr)
        # combos dont un item est traduit : (aucun) / (none)
        cur = gv("mk_cmb_default_source")
        cfgi("mk_cmb_default_source", items=source_choices())
        sv("mk_cmb_default_source",
           none_token() if (not cur or cur in MKV_NONE_TOKENS) else cur)
        # lignes de la table : combos, statuts, infobulles, boutons
        for idx, row in enumerate(M["rows"]):
            cfgi("mk_cmb_lang_%d" % row.uid, items=lang_choices())
            sv("mk_cmb_lang_%d" % row.uid, row.lang or none_token())
            cfgi("mk_cmb_src_%d" % row.uid, items=source_choices())
            sv("mk_cmb_src_%d" % row.uid, row.source or none_token())
            sv("mk_txt_status_%d" % row.uid, mp(row.status))
            sv("mk_tip_year_%d" % row.uid, year_origin_label(row))
            cfgi("mk_btn_det_%d" % row.uid, label=mx("Details", "Details"))
        # textes dynamiques deja affiches
        redraw_log()
        for t in ("mk_status", "mk_status_top"):
            sv(t, mp(M["status"]))
            cfgi(t, color=M.get("status_color", COL_OK))
        cfgi("mk_progress", overlay=mp(M["progress"][1]))
        refresh_tmdb_state()
        render_ffprobe_state()
        if M["detail_row"] is not None:
            sv("mk_detail_text", detail_text(M["detail_row"]))

    # -- construction de l'interface ------------------------------------
    def build_popups():
        with dpg.window(tag="mk_detail_window", label="Details du fichier", show=False,
                        modal=False, width=780, height=540, pos=(140, 110)):
            dpg.add_input_text(tag="mk_detail_text", multiline=True, readonly=True,
                               width=-1, height=-40, default_value="")
            dpg.add_button(label="Fermer", tag="mk_btn_close_detail",
                           callback=cb_close_detail)

    def build_body():
        with dpg.group(horizontal=True):
            dpg.add_text("MKV Renamer", tag="mk_t_title", color=ACCENT)
            dpg.add_text("- renomme les films : Titre.Annee.Langue.Resolution.Source.Codec",
                         tag="mk_t_subtitle", color=COL_MUTED)
        dpg.add_separator()

        with dpg.group(horizontal=True):
            dpg.add_text("Dossier", tag="mk_t_folder")
            dpg.add_input_text(tag="mk_in_folder", width=520, hint=r"D:\Films")
            dpg.add_button(label="Parcourir", tag="mk_btn_browse", callback=cb_browse)
            tip("Choisir le dossier contenant les films a renommer.",
                "Choose the folder holding the movies to rename.")
            dpg.add_checkbox(label="Sous-dossiers", tag="mk_cb_recursive")
            tip("Analyser aussi les sous-dossiers.", "Also analyse subfolders.")
            dpg.add_text("Extensions", tag="mk_t_exts")
            dpg.add_input_text(tag="mk_in_exts", width=130, hint=".mkv, .mp4")
            dpg.add_button(label="Analyser", tag="mk_btn_scan", callback=cb_scan)
            tip("Lit chaque fichier avec ffprobe et propose un nouveau nom.",
                "Reads every file with ffprobe and proposes a new name.")
            dpg.add_button(label="Stop", tag="mk_btn_stop", show=False, callback=cb_stop)

        # etat + statut repetes EN HAUT : le journal du bas peut etre hors ecran
        # sur les petits ecrans quand le bandeau de configuration est deplie.
        with dpg.group(horizontal=True):
            dpg.add_text("Moteur ffprobe :", tag="mk_t_ffstate", color=COL_MUTED)
            dpg.add_text("", tag="mk_ffstate", color=COL_MUTED)
            dpg.add_button(label="Telecharger ffmpeg", tag="mk_btn_getff", show=False,
                           small=True, callback=cb_get_ffmpeg)
            tip("Ouvre la page de telechargement officielle de ffmpeg.",
                "Opens the official ffmpeg download page.")
            dpg.add_button(label="Choisir ffprobe...", tag="mk_btn_browse_ff",
                           show=False, small=True, callback=cb_browse_ffprobe)
            tip("Si ffmpeg est deja installe : designer ffprobe.exe directement.",
                "If ffmpeg is already installed: point to ffprobe.exe directly.")
            dpg.add_spacer(width=12)
            dpg.add_text("", tag="mk_status_top", color=COL_OK)

        with dpg.collapsing_header(label="Options de renommage", tag="mk_opt_header",
                                   default_open=False):
            with dpg.group(horizontal=True):
                dpg.add_text("Cle TMDB commune :", tag="mk_t_tmdb", color=COL_MUTED)
                dpg.add_text("", tag="mk_tmdb_state", color=COL_MUTED)
                tip("La cle TMDB est celle du bandeau de configuration commun, "
                    "partagee avec les autres onglets.",
                    "The TMDB key is the one from the common configuration bar, "
                    "shared with the other tabs.")
                dpg.add_button(label="Tester la cle", tag="mk_btn_test_key",
                               callback=cb_test_key)
                dpg.add_spacer(width=8)
                dpg.add_checkbox(label="Autoriser TMDB a remplacer une annee deja presente",
                                 tag="mk_cb_replace_year")
                dpg.add_spacer(width=8)
                dpg.add_button(label="Vider le cache TMDB", tag="mk_btn_clear_cache",
                               callback=cb_clear_cache)
            dpg.add_text("Par defaut TMDB ne comble que les annees manquantes : une annee "
                         "lue dans le nom de fichier ou saisie a la main n'est jamais "
                         "ecrasee.", tag="mk_t_yearnote", color=COL_MUTED, wrap=900)
            with dpg.group(horizontal=True):
                dpg.add_text("Separateur", tag="mk_t_sep")
                dpg.add_input_text(tag="mk_in_sep", width=40)
                dpg.add_text("Style codec", tag="mk_t_codecstyle")
                dpg.add_combo(list(MKV_CODEC_STYLES.keys()), tag="mk_cmb_codec_style",
                              width=90)
                dpg.add_text("Token VF", tag="mk_t_tokenvf")
                dpg.add_input_text(tag="mk_in_token_vf", width=110)
                tip("Etiquette utilisee pour une version francaise seule "
                    "(VF, TRUEFRENCH, VFF...).",
                    "Label used for a French-only version (VF, TRUEFRENCH, VFF...).")
                dpg.add_text("Source par defaut", tag="mk_t_defsrc")
                dpg.add_combo(source_choices(), tag="mk_cmb_default_source", width=140)
            with dpg.group(horizontal=True):
                dpg.add_checkbox(label="Points dans le titre", tag="mk_cb_dots")
                dpg.add_checkbox(label="Ajouter 10bit", tag="mk_cb_10bit")
                dpg.add_checkbox(label="Ajouter HDR", tag="mk_cb_hdr")
                dpg.add_checkbox(label="Renommer les fichiers associes (srt, nfo...)",
                                 tag="mk_cb_companions")
            with dpg.group(horizontal=True):
                dpg.add_text("ffprobe", tag="mk_t_ffprobe")
                dpg.add_input_text(tag="mk_in_ffprobe", width=480,
                                   hint="laisser vide pour detection automatique")
                dpg.add_button(label="...", width=28, tag="mk_btn_ff_dots",
                               callback=cb_browse_ffprobe)
                dpg.add_button(label="Appliquer et recalculer", tag="mk_btn_apply",
                               callback=cb_recompute)
            dpg.add_text("Donnees : %s" % MKV_DATA_DIR, color=COL_MUTED)

        dpg.add_separator()

        with dpg.group(horizontal=True):
            dpg.add_button(label="Tout cocher", tag="mk_btn_all",
                           callback=cb_select_all, user_data=True)
            dpg.add_button(label="Tout decocher", tag="mk_btn_none",
                           callback=cb_select_all, user_data=False)
            dpg.add_button(label="Recalculer les noms", tag="mk_btn_recompute",
                           callback=cb_recompute)
            dpg.add_button(label="Chercher les annees (TMDB)", tag="mk_btn_years",
                           callback=cb_fetch_years)
            tip("Complete les annees manquantes avec la cle TMDB commune.",
                "Fills in the missing years using the common TMDB key.")
            dpg.add_button(label="RENOMMER LA SELECTION", tag="mk_btn_rename",
                           width=230, callback=cb_rename)
            dpg.add_button(label="Annuler le dernier lot", tag="mk_btn_undo",
                           callback=cb_undo)
            tip("Restaure les noms d'origine du dernier lot renomme.",
                "Restores the original names of the last renamed batch.")

        with dpg.table(tag="mk_table", header_row=True, resizable=True,
                       borders_innerH=True, borders_outerH=True,
                       borders_innerV=True, borders_outerV=True,
                       scrollY=True, scrollX=True, height=-165,
                       policy=dpg.mvTable_SizingFixedFit):
            dpg.add_table_column(label="", tag="mk_col_check",
                                 width_fixed=True, init_width_or_weight=26)
            dpg.add_table_column(label="Fichier actuel", tag="mk_col_orig",
                                 width_fixed=True, init_width_or_weight=195)
            dpg.add_table_column(label="Titre", tag="mk_col_title",
                                 width_fixed=True, init_width_or_weight=150)
            dpg.add_table_column(label="Annee", tag="mk_col_year",
                                 width_fixed=True, init_width_or_weight=54)
            dpg.add_table_column(label="Langue", tag="mk_col_lang",
                                 width_fixed=True, init_width_or_weight=95)
            dpg.add_table_column(label="Res.", tag="mk_col_res",
                                 width_fixed=True, init_width_or_weight=50)
            dpg.add_table_column(label="Source", tag="mk_col_source",
                                 width_fixed=True, init_width_or_weight=115)
            dpg.add_table_column(label="Codec", tag="mk_col_codec",
                                 width_fixed=True, init_width_or_weight=54)
            dpg.add_table_column(label="Nouveau nom", tag="mk_col_new",
                                 width_fixed=True, init_width_or_weight=250)
            dpg.add_table_column(label="Statut", tag="mk_col_status",
                                 width_fixed=True, init_width_or_weight=118)
            dpg.add_table_column(label="", tag="mk_col_det",
                                 width_fixed=True, init_width_or_weight=54)

        dpg.add_progress_bar(tag="mk_progress", default_value=0.0, width=-1, overlay="")
        dpg.add_input_text(tag="mk_log", multiline=True, readonly=True,
                           height=95, width=-1, default_value="")
        dpg.add_text("Pret.", tag="mk_status", color=COL_OK)

        # themes des boutons d'action
        M["themes"]["rename"] = _mkv_btn_theme((28, 122, 62), (38, 158, 82), (20, 96, 48))
        M["themes"]["stop"] = _mkv_btn_theme((160, 48, 48), (196, 62, 62), (128, 38, 38))
        M["themes"]["undo"] = _mkv_btn_theme((150, 106, 26), (186, 134, 38), (120, 84, 20))
        for tg, th in (("mk_btn_rename", "rename"), ("mk_btn_stop", "stop"),
                       ("mk_btn_undo", "undo")):
            try:
                dpg.bind_item_theme(tg, M["themes"][th])
            except Exception:
                pass

        apply_cfg_to_widgets()
        refresh_tmdb_state()
        ff = refresh_ffprobe_state()
        M["log"].append((time.strftime("%H:%M:%S"),
                         "MKV Renamer pret -- donnees : %s" % MKV_DATA_DIR,
                         "MKV Renamer ready -- data: %s" % MKV_DATA_DIR))
        M["log"].append((time.strftime("%H:%M:%S"),
                         "ffprobe : %s" % (ff or "INTROUVABLE (installe ffmpeg)"),
                         "ffprobe: %s" % (ff or "NOT FOUND (install ffmpeg)")))
        redraw_log()

    return {
        "build_popups": build_popups,
        "build_body": build_body,
        "set_lang": set_lang,
        "push_shared": push_shared,
        "save": lambda: (read_widgets_to_cfg(), mkv_save_config(M["cfg"]))[1],
        "state": M,
    }


def _mkv_btn_theme(base, hovered, active, text=(255, 255, 255)):
    """Bouton colore, terne quand il est desactive."""
    with dpg.theme() as theme:
        with dpg.theme_component(dpg.mvButton, enabled_state=True):
            dpg.add_theme_color(dpg.mvThemeCol_Button, base)
            dpg.add_theme_color(dpg.mvThemeCol_ButtonHovered, hovered)
            dpg.add_theme_color(dpg.mvThemeCol_ButtonActive, active)
            dpg.add_theme_color(dpg.mvThemeCol_Text, text)
            dpg.add_theme_style(dpg.mvStyleVar_FrameRounding, 5)
        with dpg.theme_component(dpg.mvButton, enabled_state=False):
            grey = tuple(int(c * 0.35 + 45) for c in base[:3])
            dpg.add_theme_color(dpg.mvThemeCol_Button, grey)
            dpg.add_theme_color(dpg.mvThemeCol_ButtonHovered, grey)
            dpg.add_theme_color(dpg.mvThemeCol_ButtonActive, grey)
            dpg.add_theme_color(dpg.mvThemeCol_Text, (135, 135, 135))
            dpg.add_theme_style(dpg.mvStyleVar_FrameRounding, 5)
    return theme


# ---------------------------------------------------------------------
#  Self-test du moteur MKV (sans GUI, sans reseau) : --mkv-selftest
# ---------------------------------------------------------------------
def _mkv_probe_stub(width=1920, height=1080, vcodec="hevc", audio=None, subs=None,
                    bitrate=5000000, duration=6000.0, tags=None, chapters=0,
                    pix_fmt="yuv420p", filename="film.mkv"):
    audio = audio if audio is not None else [("ac3", "fre", "")]
    subs = subs or []
    streams = [{"codec_type": "video", "width": width, "height": height,
                "codec_name": vcodec, "pix_fmt": pix_fmt, "tags": {}}]
    for codec, lang, title in audio:
        streams.append({"codec_type": "audio", "codec_name": codec,
                        "tags": {"language": lang, "title": title}})
    for codec, lang in subs:
        streams.append({"codec_type": "subtitle", "codec_name": codec,
                        "tags": {"language": lang}})
    return {"streams": streams,
            "chapters": [{} for _ in range(chapters)],
            "format": {"duration": str(duration), "bit_rate": str(bitrate),
                       "size": str(int(bitrate * duration / 8)),
                       "tags": tags or {}, "filename": filename}}


def mkv_selftest():
    fails = []
    n_checks = [0]

    def check(label, got, expected):
        n_checks[0] += 1
        if got != expected:
            fails.append("%s : attendu %r, obtenu %r" % (label, expected, got))

    check("res 1920x1080", mkv_resolution_token(1920, 1080), "1080p")
    check("res scope 1920x800", mkv_resolution_token(1920, 800), "1080p")
    check("res scope 1920x804", mkv_resolution_token(1920, 804), "1080p")
    check("res 3840x2160", mkv_resolution_token(3840, 2160), "2160p")
    check("res scope 3840x1600", mkv_resolution_token(3840, 1600), "2160p")
    check("res 1280x720", mkv_resolution_token(1280, 720), "720p")
    check("res 1280x536", mkv_resolution_token(1280, 536), "720p")
    check("res 720x576", mkv_resolution_token(720, 576), "576p")
    check("res 640x480", mkv_resolution_token(640, 480), "480p")
    check("res vide", mkv_resolution_token(0, 0), "")

    check("codec hevc", mkv_codec_token("hevc", "X265"), "X265")
    check("codec h264", mkv_codec_token("h264", "X265"), "X264")
    check("codec h264 minuscule", mkv_codec_token("h264", "x265"), "x264")
    check("codec point", mkv_codec_token("hevc", "H.265"), "H.265")
    check("codec av1", mkv_codec_token("av1", "X265"), "AV1")

    i = mkv_analyse_probe(_mkv_probe_stub(audio=[("ac3", "fre", ""), ("ac3", "eng", "")]))
    check("MULTi", mkv_language_token(i)[0], "MULTi")
    check("MULTi raison EN", mkv_language_token(i)[1][1], "audio tracks: fr + en")
    i = mkv_analyse_probe(_mkv_probe_stub(audio=[("ac3", "fra", "")]))
    check("VF", mkv_language_token(i)[0], "VF")
    i = mkv_analyse_probe(_mkv_probe_stub(audio=[("ac3", "fre", ""), ("dts", "fre", "")]))
    check("VF multipiste fr", mkv_language_token(i)[0], "VF")
    i = mkv_analyse_probe(_mkv_probe_stub(audio=[("ac3", "eng", "")],
                                          subs=[("subrip", "fre")]))
    check("VOSTFR", mkv_language_token(i)[0], "VOSTFR")
    i = mkv_analyse_probe(_mkv_probe_stub(audio=[("ac3", "eng", "")]))
    check("VO", mkv_language_token(i)[0], "VO")
    i = mkv_analyse_probe(_mkv_probe_stub(audio=[("ac3", "und", "VF"),
                                                 ("ac3", "und", "English")]))
    check("MULTi via libelles", mkv_language_token(i)[0], "MULTi")
    i = mkv_analyse_probe(_mkv_probe_stub(audio=[("ac3", "und", "")]))
    check("langue inconnue", mkv_language_token(i)[0], "")
    i = mkv_analyse_probe(_mkv_probe_stub(audio=[("ac3", "fre", "")]))
    check("token VF personnalise",
          mkv_language_token(i, token_vf="TRUEFRENCH")[0], "TRUEFRENCH")

    i = mkv_analyse_probe(_mkv_probe_stub(vcodec="h264", bitrate=28000000,
                                          audio=[("dts", "fre", ""), ("truehd", "eng", "")],
                                          subs=[("hdmv_pgs_subtitle", "fre")],
                                          chapters=12), "film.mkv")
    src, conf, reasons = mkv_guess_source(i)
    check("source remux BD", src, "BluRay.REMUX")
    check("raisons bilingues", all(isinstance(r, tuple) and len(r) == 2
                                   for r in reasons), True)
    i = mkv_analyse_probe(_mkv_probe_stub(vcodec="hevc", bitrate=4000000,
                                          audio=[("eac3", "fre", ""), ("eac3", "eng", "")],
                                          subs=[("subrip", "fre")],
                                          tags={"ENCODER": "Lavf60.16.100"}), "film.mkv")
    check("source web-dl", mkv_guess_source(i)[0], "WEB-DL")
    i = mkv_analyse_probe(_mkv_probe_stub(vcodec="h264", bitrate=14000000,
                                          audio=[("dts", "fre", "")],
                                          subs=[("hdmv_pgs_subtitle", "fre")]), "film.mkv")
    check("source bluray", mkv_guess_source(i)[0], "BluRay")
    i = mkv_analyse_probe(_mkv_probe_stub(),
                          "Le film 2001 MULTi 1080p WEB-DL x265.mkv")
    check("source depuis le nom", mkv_guess_source(i)[0], "WEB-DL")

    check("titre simple", mkv_clean_title("Max de betises.mkv"), "Max de betises")
    check("titre scene",
          mkv_clean_title("Le.dernier.chateau.2001.MULTi.1080p.WEB-DL.X265-GROUPE.mkv"),
          "Le dernier chateau")
    check("titre parenthese",
          mkv_clean_title("Le Dernier Chateau (2001) [1080p].mkv"), "Le Dernier Chateau")
    check("titre tirets", mkv_clean_title("Mad.Max.Fury.Road.2015.MULTi.2160p.mkv"),
          "Mad Max Fury Road")
    check("annee", mkv_extract_year("Le dernier chateau 2001 MULTi"), "2001")
    check("annee absente", mkv_extract_year("Max de betises"), "")
    check("annee pas 4k", mkv_extract_year("Film 2160p"), "")

    check("nom complet",
          mkv_build_name("Le dernier chateau", "2001", "MULTi", "1080p", "WEB-DL",
                         "X265", ".mkv"),
          "Le dernier chateau.2001.MULTi.1080p.WEB-DL.X265.mkv")
    check("nom points",
          mkv_build_name("Le dernier chateau", "2001", "MULTi", "1080p", "WEB-DL",
                         "X265", ".mkv", dots_in_title=True),
          "Le.dernier.chateau.2001.MULTi.1080p.WEB-DL.X265.mkv")
    check("nom sans annee",
          mkv_build_name("Titre", "", "VF", "720p", "BluRay", "X264", ".mkv"),
          "Titre.VF.720p.BluRay.X264.mkv")
    check("nom hdr",
          mkv_build_name("T", "2020", "MULTi", "2160p", "WEB-DL", "X265", ".mkv",
                         hdr="HDR", tenbit="10bit"),
          "T.2020.MULTi.2160p.WEB-DL.HDR.10bit.X265.mkv")
    check("nom caracteres interdits",
          mkv_build_name('Mission: Impossible / Fallout?', "2018", "MULTi", "1080p",
                         "BluRay", "X265", ".mkv"),
          "Mission Impossible - Fallout.2018.MULTi.1080p.BluRay.X265.mkv")
    check("token (aucun) ignore",
          mkv_build_name("T", "2001", "(aucun)", "1080p", "WEB-DL", "X265", ".mkv"),
          "T.2001.1080p.WEB-DL.X265.mkv")
    check("token (none) ignore",
          mkv_build_name("T", "2001", "(none)", "1080p", "WEB-DL", "X265", ".mkv"),
          "T.2001.1080p.WEB-DL.X265.mkv")

    check("norm fre", mkv_norm_lang("fre"), "fr")
    check("norm fra", mkv_norm_lang("fra"), "fr")
    check("norm fr-CA", mkv_norm_lang("fr-CA"), "fr")
    check("norm eng", mkv_norm_lang("eng"), "en")
    check("norm und", mkv_norm_lang("und"), "")

    print("Self-test MKV Renamer : %d verifications" % n_checks[0])
    if fails:
        print("ECHECS (%d) :" % len(fails))
        for f in fails:
            print("  - " + f)
        return 1
    print("Tout est conforme.")
    return 0


# =====================================================================
#  OUTIL 5 : Sessions  --  qui est connecte, qui lit quoi, en direct
#
#  Interroge GET /Sessions a intervalle regulier (reglable) et affiche
#  pour chaque session : utilisateur, appareil, IP, etat, contenu en
#  cours, progression, mode de lecture (direct / remux / transcodage),
#  inactivite. Ajoute le journal d'activite du serveur
#  (GET /System/ActivityLog/Entries) sous le tableau.
#
#  Actions a distance : pause / reprise / arret et message a l'ecran
#  (POST /Sessions/{id}/Playing/{cmd} et /Sessions/{id}/Command/DisplayMessage).
#
#  URL et cle API viennent du bandeau de configuration commun.
# =====================================================================

SES_INI_PATH = MKV_DATA_DIR / "emby_toolbox_sessions.ini"

SES_DEFAULTS = {
    "affichage": {
        "intervalle": "10",        # secondes entre deux rafraichissements
        "auto": "1",
        "montrer_inactives": "1",
        "journal": "1",
        "journal_lignes": "40",
        "msg_methode": "message",       # message | commande
        "msg_objet_dans_texte": "1",    # recopie l'objet en tete du texte
        "msg_duree": "8",
        "masquer_fantomes": "1",        # cacher les lectures qui n'avancent plus
        "fantome_delai": "120",         # secondes sans progression = fantome
    },
}


def ses_load_config():
    cfg = configparser.ConfigParser()
    cfg.read_dict(SES_DEFAULTS)
    try:
        if SES_INI_PATH.exists():
            cfg.read(SES_INI_PATH, encoding="utf-8")
    except Exception:
        pass
    return cfg


def ses_save_config(cfg):
    try:
        with open(SES_INI_PATH, "w", encoding="utf-8") as fh:
            cfg.write(fh)
        return True
    except Exception:
        return False


def ses_secs(ticks):
    """Ticks Emby (100 ns) -> secondes."""
    try:
        return float(ticks or 0) / 1e7
    except Exception:
        return 0.0


def ses_hms(seconds):
    try:
        s = int(float(seconds))
    except Exception:
        return "?"
    if s < 0:
        s = 0
    h, m, s = s // 3600, (s % 3600) // 60, s % 60
    return "%d:%02d:%02d" % (h, m, s) if h else "%d:%02d" % (m, s)


def ses_parse_date(txt):
    """Date ISO Emby -> horodatage epoch (0 si illisible)."""
    if not txt:
        return 0.0
    t = str(txt).strip().replace("Z", "+00:00")
    m = re.match(r"^(.*\.\d{1,6})\d*([+-]\d{2}:\d{2})?$", t)
    if m:                                   # Emby ecrit 7 chiffres, Python en veut 6
        t = m.group(1) + (m.group(2) or "")
    try:
        import datetime as _dt
        d = _dt.datetime.fromisoformat(t)
        if d.tzinfo is None:
            d = d.replace(tzinfo=_dt.timezone.utc)
        return d.timestamp()
    except Exception:
        return 0.0


def ses_get(base, key, path, params=None, timeout=12):
    """GET Emby avec delai d'attente court : le sondage est frequent, on ne
    veut pas d'un thread bloque 30 s quand le serveur ne repond plus."""
    p = dict(params or {})
    p["api_key"] = key
    url = "%s%s?%s" % (base.rstrip("/"), path, urllib.parse.urlencode(p))
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def ses_post(base, key, path, params=None, body=None, timeout=15):
    """POST Emby avec les parametres dans l'URL.

    Important : les endpoints de session (Message, Playing/...) lisent leurs
    parametres dans la QUERY STRING, pas dans le corps JSON. Un corps JSON
    seul donne une boite de dialogue vide cote client."""
    p = dict(params or {})
    p["api_key"] = key
    url = "%s%s?%s" % (base.rstrip("/"), path, urllib.parse.urlencode(p))
    data = json.dumps(body if body is not None else {}).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="POST", headers={
        "Content-Type": "application/json",
        "Accept": "application/json",
        "X-Emby-Authorization": ('MediaBrowser Client="EmbyToolbox",'
                                 'Device="PC",DeviceId="emby-toolbox-1",'
                                 'Version="1.0",Token="%s"' % key),
    })
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status          # 204 = succes Emby


def ses_item_label(item):
    """Libelle lisible d'un element joue ou consulte."""
    if not isinstance(item, dict):
        return ""
    kind = item.get("Type") or ""
    name = item.get("Name") or ""
    if kind == "Episode":
        serie = item.get("SeriesName") or ""
        sn, ep = item.get("ParentIndexNumber"), item.get("IndexNumber")
        code = ""
        try:
            if sn is not None and ep is not None:
                code = "S%02dE%02d" % (int(sn), int(ep))
            elif ep is not None:
                code = "E%02d" % int(ep)
        except Exception:
            code = ""
        return " - ".join(x for x in (serie, code, name) if x)
    if kind == "Audio":
        art = ", ".join(item.get("Artists") or []) or (item.get("AlbumArtist") or "")
        return " - ".join(x for x in (art, name, item.get("Album") or "") if x)
    year = item.get("ProductionYear")
    return "%s (%s)" % (name, year) if year else name


def _init_sessions():
    COL_OK = (46, 204, 113)
    COL_WARN = (235, 185, 90)
    COL_ERR = (235, 110, 110)
    COL_MUTED = (136, 136, 170)
    COL_INFO = (90, 170, 235)
    COL_GHOST = (170, 120, 120)
    ACCENT = (233, 69, 96)

    S = {
        "lang": "fr",
        "cfg": ses_load_config(),
        "sessions": [],          # dernieres sessions recues
        "activity": [],          # dernieres entrees du journal
        "users": {},             # {user_id: nom}
        "lib_of": {},            # {item_id: nom de mediatheque} (cache)
        "folders": None,         # [(nom, [chemins])] des mediatheques (repli)
        "last_seen": {},         # {session_id: (libelle, mediatheque, horodatage)}
        "progress_seen": {},     # {session_id: (position, horodatage du dernier changement)}
        "ghosts": 0,             # nombre de fantomes au dernier rafraichissement
        "err": None,             # (fr, en) derniere erreur
        "last_ok": 0.0,          # horodatage du dernier succes
        "last_try": 0.0,
        "last_log": 0.0,
        "last_users": 0.0,
        "inflight": False,
        "pending_shown": False,  # « interrogation en cours » deja affiche
        "msg_target": None,      # session visee par la fenetre de message
        "msg_to_name": "",       # nom lisible du destinataire (retour visuel)
        "stop_target": None,
    }

    def mx(fr, en):
        return en if S["lang"] == "en" else fr

    def post(fn, *a, **k):
        ui_post(lambda: fn(*a, **k))

    def sv(tag, value):
        if dpg.does_item_exist(tag):
            try:
                dpg.set_value(tag, value)
            except Exception:
                pass

    def cfgi(tag, **kw):
        if dpg.does_item_exist(tag):
            try:
                dpg.configure_item(tag, **kw)
            except Exception:
                pass

    def gv(tag, default=""):
        return dpg.get_value(tag) if dpg.does_item_exist(tag) else default

    # -- traductions ----------------------------------------------------
    TR = {
        "se_t_title":     ("Sessions Emby", "Emby sessions"),
        "se_t_subtitle":  ("- qui est connecte, qui lit quoi, en direct",
                           "- who is connected, who is watching what, live"),
        "se_btn_refresh": ("Rafraichir maintenant", "Refresh now"),
        "se_cb_auto":     ("Rafraichissement auto", "Auto refresh"),
        "se_t_every":     ("toutes les", "every"),
        "se_t_sec":       ("secondes", "seconds"),
        "se_cb_idle":     ("Montrer les sessions sans lecture",
                           "Show sessions without playback"),
        "se_cb_log":      ("Journal du serveur", "Server log"),
        "se_cb_ghost":    ("Masquer les lectures fantomes", "Hide ghost playbacks"),
        "se_t_ghost_after": ("au-dela de", "after"),
        "se_t_ghost_unit": ("secondes sans progression", "seconds without progress"),
        "se_h_log":       ("Journal d'activite du serveur",
                           "Server activity log"),
        "se_btn_log_refresh": ("Recharger le journal", "Reload the log"),
        "se_col_user":    ("Utilisateur", "User"),
        "se_col_device":  ("Appareil", "Device"),
        "se_col_ip":      ("Adresse IP", "IP address"),
        "se_col_state":   ("Etat", "State"),
        "se_col_lib":     ("Mediatheque", "Library"),
        "se_col_item":    ("Contenu", "Content"),
        "se_col_prog":    ("Progression", "Progress"),
        "se_col_mode":    ("Mode", "Method"),
        "se_col_seen":    ("Activite", "Activity"),
        "se_col_act":     ("Actions", "Actions"),
        "se_lcol_date":   ("Date", "Date"),
        "se_lcol_user":   ("Utilisateur", "User"),
        "se_lcol_event":  ("Evenement", "Event"),
        "se_lcol_detail": ("Detail", "Detail"),
        "se_msg_window":  ("Message a l'ecran", "On-screen message"),
        "se_t_msg_to":    ("Destinataire :", "Recipient:"),
        "se_t_msg_head":  ("Objet", "Subject"),
        "se_t_msg_text":  ("Message", "Message"),
        "se_t_msg_timeout": ("Duree d'affichage (s), 0 = attendre la confirmation",
                             "Display time (s), 0 = wait for confirmation"),
        "se_btn_msg_send": ("Envoyer", "Send"),
        "se_t_msg_method": ("Methode", "Method"),
        "se_cb_msg_prefix": ("Ajouter l'objet en tete du texte",
                             "Prepend the subject to the text"),
        "se_t_msg_note":  ("Emby autorise les clients a ignorer l'objet et la duree "
                           "d'affichage. Si c'est le cas du tien, coche la case "
                           "ci-dessus et/ou essaie l'autre methode.",
                           "Emby allows clients to ignore the subject and the display "
                           "time. If yours does, tick the box above and/or try the "
                           "other method."),
        "se_btn_msg_cancel": ("Annuler", "Cancel"),
        "se_stop_window": ("Arreter la lecture ?", "Stop playback?"),
        "se_btn_stop_yes": ("Arreter", "Stop"),
        "se_btn_stop_no": ("Annuler", "Cancel"),
    }
    HINTS = {
        "se_in_msg_head": ("Emby", "Emby"),
        "se_in_msg_text": ("Extinction du serveur dans 10 minutes...",
                           "Server shutting down in 10 minutes..."),
    }
    TIPS = []
    _tipn = [0]

    def tip(fr, en, wrap=320):
        _tipn[0] += 1
        tag = "se_tip_%d" % _tipn[0]
        with dpg.tooltip(dpg.last_item()):
            dpg.add_text(mx(fr, en), tag=tag, wrap=wrap)
        TIPS.append((tag, fr, en))

    # -- identifiants du bandeau commun ---------------------------------
    def creds():
        base = (gv("sh_url") or "").strip()
        key = (gv("sh_key") or "").strip()
        if not base or not key:
            c = get_shared_creds()
            base = base or (c.get("url") or "").strip()
            key = key or (c.get("api_key") or "").strip()
        return base.rstrip("/"), key

    # -- mise en forme --------------------------------------------------
    def since_label(epoch):
        if not epoch:
            return "?"
        d = max(0, int(time.time() - epoch))
        if d < 60:
            return mx("il y a %d s", "%d s ago") % d
        if d < 3600:
            return mx("il y a %d min", "%d min ago") % (d // 60)
        if d < 86400:
            return mx("il y a %d h", "%d h ago") % (d // 3600)
        return mx("il y a %d j", "%d d ago") % (d // 86400)

    def play_mode(sess):
        """(texte, couleur, detail_infobulle) du mode de lecture."""
        ps = sess.get("PlayState") or {}
        tr = sess.get("TranscodingInfo") or {}
        method = (ps.get("PlayMethod") or "").lower()
        det = []
        if tr:
            vc, ac = tr.get("VideoCodec") or "", tr.get("AudioCodec") or ""
            if vc or ac:
                det.append("%s / %s" % (vc or "?", ac or "?"))
            br = tr.get("Bitrate")
            if br:
                try:
                    det.append("%.1f Mb/s" % (float(br) / 1e6))
                except Exception:
                    pass
            cpu = tr.get("CurrentCpuUsage")
            if cpu:
                try:
                    det.append("CPU %.0f%%" % float(cpu))
                except Exception:
                    pass
            for k in ("TranscodeReasons", "TranscodeReason"):
                if tr.get(k):
                    v = tr[k]
                    det.append(", ".join(v) if isinstance(v, list) else str(v))
                    break
        detail = "  |  ".join(det)
        if method.startswith("transcode"):
            return mx("Transcodage", "Transcoding"), COL_ERR, detail
        if method.startswith("directstream"):
            return mx("Remux", "Remux"), COL_WARN, detail
        if method.startswith("directplay"):
            return mx("Direct", "Direct"), COL_OK, detail
        return (method or "-"), COL_MUTED, detail

    def state_of(sess):
        """(texte, couleur) de l'etat de la session."""
        ps = sess.get("PlayState") or {}
        if sess.get("NowPlayingItem"):
            if ghost_reason(sess):
                return mx("Fantome", "Ghost"), COL_GHOST
            if ps.get("IsPaused"):
                return mx("En pause", "Paused"), COL_WARN
            return mx("Lecture", "Playing"), COL_OK
        if sess.get("NowViewingItem"):
            return mx("Navigation", "Browsing"), COL_INFO
        return mx("Connecte", "Connected"), COL_MUTED

    def ghost_delay():
        try:
            return max(30, min(3600, int(S["cfg"]["affichage"]["fantome_delai"] or 120)))
        except Exception:
            return 120

    def ghost_reason(sess):
        """(fr, en) expliquant pourquoi la session est jugee fantome, ou None.

        Emby conserve une session « en lecture » quand un client disparait sans
        signaler l'arret (frequent sur iOS) : la position n'avance plus."""
        if not sess.get("NowPlayingItem"):
            return None
        now = time.time()
        delay = ghost_delay()
        ps = sess.get("PlayState") or {}
        seen = S["progress_seen"].get(sess.get("Id") or "")
        if not ps.get("IsPaused") and seen:
            frozen = now - seen[1]
            if frozen > delay:
                return ("lecture annoncee, mais la position n'a pas bouge depuis %s"
                        % ses_hms(frozen),
                        "reported as playing, but the position has not moved for %s"
                        % ses_hms(frozen))
        last = ses_parse_date(sess.get("LastActivityDate"))
        if last:
            age = now - last
            if age > max(delay, 300):
                return ("plus aucune activite de cet appareil depuis %s" % ses_hms(age),
                        "no activity from this device for %s" % ses_hms(age))
        return None

    def visible_sessions():
        show_idle = S["cfg"]["affichage"]["montrer_inactives"] == "1"
        hide_ghosts = S["cfg"]["affichage"]["masquer_fantomes"] == "1"
        ghosts = [s for s in S["sessions"] if ghost_reason(s)]
        S["ghosts"] = len(ghosts)
        out = [s for s in S["sessions"] if show_idle or s.get("NowPlayingItem")]
        if hide_ghosts:
            out = [s for s in out if not ghost_reason(s)]
        # lectures reelles en premier, puis navigation, puis inactifs et fantomes
        def rank(s):
            if s.get("NowPlayingItem"):
                return 3 if ghost_reason(s) else 0
            if s.get("NowViewingItem"):
                return 1
            return 2
        return sorted(out, key=lambda s: (rank(s),
                                          -ses_parse_date(s.get("LastActivityDate"))))

    # -- rendu ----------------------------------------------------------
    def render():
        if not dpg.does_item_exist("se_table"):
            return
        try:
            dpg.delete_item("se_table", children_only=True, slot=1)
        except Exception:
            pass
        rows = visible_sessions()

        for s in rows:
            ps = s.get("PlayState") or {}
            item = s.get("NowPlayingItem") or s.get("NowViewingItem") or {}
            st_txt, st_col = state_of(s)
            md_txt, md_col, md_det = play_mode(s)
            sid = s.get("Id") or ""
            with dpg.table_row(parent="se_table"):
                dpg.add_text(s.get("UserName") or mx("(personne)", "(nobody)"))
                dpg.add_text(s.get("DeviceName") or "-")
                cli = "%s %s" % (s.get("Client") or "", s.get("ApplicationVersion") or "")
                with dpg.tooltip(dpg.last_item()):
                    dpg.add_text("%s\n%s" % (cli.strip() or "-",
                                             s.get("DeviceId") or ""), wrap=340)
                dpg.add_text((s.get("RemoteEndPoint") or "-").replace("::ffff:", ""))
                dpg.add_text(st_txt, color=st_col)
                gr = ghost_reason(s)
                if gr:
                    with dpg.tooltip(dpg.last_item()):
                        dpg.add_text(mx("Session fantome : %s.\n\nEmby garde la "
                                        "session quand un client disparait sans "
                                        "signaler l'arret (frequent sur mobile). "
                                        "Elle s'effacera d'elle-meme cote serveur.",
                                        "Ghost session: %s.\n\nEmby keeps the session "
                                        "when a client vanishes without reporting the "
                                        "stop (common on mobile). It will clear itself "
                                        "on the server side.") % mx(gr[0], gr[1]), wrap=380)
                last = S["last_seen"].get(sid)
                lib = S["lib_of"].get(item.get("Id"), "") if item else ""
                if lib:
                    dpg.add_text(lib, color=COL_INFO)
                elif last and last[1]:
                    dpg.add_text(last[1], color=COL_MUTED)
                    with dpg.tooltip(dpg.last_item()):
                        dpg.add_text(mx("Derniere mediatheque observee, %s.",
                                        "Last library seen, %s.") % since_label(last[2]),
                                     wrap=340)
                else:
                    dpg.add_text("-", color=COL_MUTED)
                label = ses_item_label(item)
                if label:
                    dpg.add_text(label)
                    with dpg.tooltip(dpg.last_item()):
                        dpg.add_text(label, wrap=420)
                elif last and last[0]:
                    dpg.add_text(mx("dernier : %s", "last: %s") % last[0], color=COL_MUTED)
                    with dpg.tooltip(dpg.last_item()):
                        dpg.add_text(mx("Emby ne dit pas ce qu'un utilisateur consulte "
                                        "quand il ne lit rien. Voici le dernier contenu "
                                        "que ce programme a vu sur cette session, %s.",
                                        "Emby does not report what a user is browsing "
                                        "when nothing is playing. This is the last "
                                        "content this program saw on this session, %s.")
                                     % since_label(last[2]), wrap=380)
                else:
                    dpg.add_text(mx("(rien en cours)", "(nothing playing)"),
                                 color=COL_MUTED)
                # progression
                if s.get("NowPlayingItem"):
                    total = ses_secs((s.get("NowPlayingItem") or {}).get("RunTimeTicks"))
                    pos = ses_secs(ps.get("PositionTicks"))
                    frac = (pos / total) if total > 0 else 0.0
                    dpg.add_progress_bar(
                        default_value=max(0.0, min(1.0, frac)), width=-1, height=18,
                        overlay="%s / %s" % (ses_hms(pos), ses_hms(total) if total else "?"))
                else:
                    dpg.add_text("-", color=COL_MUTED)
                dpg.add_text(md_txt if s.get("NowPlayingItem") else "-",
                             color=md_col if s.get("NowPlayingItem") else COL_MUTED)
                if md_det:
                    with dpg.tooltip(dpg.last_item()):
                        dpg.add_text(md_det, wrap=420)
                dpg.add_text(since_label(ses_parse_date(s.get("LastActivityDate"))),
                             color=COL_MUTED)
                with dpg.group(horizontal=True):
                    if s.get("NowPlayingItem"):
                        paused = bool(ps.get("IsPaused"))
                        dpg.add_button(
                            label=mx("Reprendre", "Resume") if paused else mx("Pause", "Pause"),
                            small=True, user_data=(sid, "Unpause" if paused else "Pause"),
                            callback=cb_playstate)
                        dpg.add_button(label=mx("Stop", "Stop"), small=True,
                                       user_data=s, callback=cb_ask_stop)
                    dpg.add_button(label=mx("Msg", "Msg"), small=True,
                                   user_data=s, callback=cb_ask_message)

        if not rows:
            with dpg.table_row(parent="se_table"):
                dpg.add_text(mx("Aucune session", "No session"), color=COL_MUTED)
                for _ in range(9):
                    dpg.add_text("")
        render_summary()
        render_activity()

    def flash(fr, en):
        """Retour visuel bref : la prochaine mise a jour reprend la main."""
        sv("se_summary", mx(fr, en))
        cfgi("se_summary", color=COL_OK)

    def render_summary():
        real = [s for s in S["sessions"] if s.get("NowPlayingItem")
                and not ghost_reason(s)]
        n_play = len([s for s in real
                      if not ((s.get("PlayState") or {}).get("IsPaused"))])
        n_pause = len([s for s in real
                       if ((s.get("PlayState") or {}).get("IsPaused"))])
        n_tr = len([s for s in real
                    if str(((s.get("PlayState") or {}).get("PlayMethod") or "")
                           ).lower().startswith("transcode")])
        n_tot = len(S["sessions"])
        if S["err"]:
            sv("se_summary", mx(*S["err"]))
            cfgi("se_summary", color=COL_ERR)
            return
        S["pending_shown"] = False
        stamp = time.strftime("%H:%M:%S", time.localtime(S["last_ok"])) if S["last_ok"] else "-"
        txt = mx("%d session(s) : %d en lecture, %d en pause, %d en transcodage "
                 "  |  mise a jour %s",
                 "%d session(s): %d playing, %d paused, %d transcoding  |  updated %s")
        line = txt % (n_tot, n_play, n_pause, n_tr, stamp)
        if S["ghosts"]:
            hidden = S["cfg"]["affichage"]["masquer_fantomes"] == "1"
            line += mx("  |  %d fantome(s)%s", "  |  %d ghost(s)%s") % (
                S["ghosts"], mx(" masque(s)", " hidden") if hidden else "")
        sv("se_summary", line)
        cfgi("se_summary", color=COL_WARN if S["ghosts"] else
             (COL_OK if n_tot else COL_MUTED))

    def render_activity():
        if not dpg.does_item_exist("se_log_table"):
            return
        try:
            dpg.delete_item("se_log_table", children_only=True, slot=1)
        except Exception:
            pass
        for e in S["activity"]:
            when = ses_parse_date(e.get("Date"))
            sev = str(e.get("Severity") or "").lower()
            col = COL_ERR if sev in ("error", "fatal") else (
                COL_WARN if sev == "warn" else COL_MUTED)
            uid = e.get("UserId") or ""
            with dpg.table_row(parent="se_log_table"):
                dpg.add_text(time.strftime("%d/%m %H:%M:%S", time.localtime(when))
                             if when else "-", color=COL_MUTED)
                dpg.add_text(S["users"].get(uid, "") or ("-" if not uid else uid[:8]))
                dpg.add_text(e.get("Name") or "-", color=col)
                det = e.get("ShortOverview") or e.get("Overview") or ""
                dpg.add_text(det or "-")
                if det:
                    with dpg.tooltip(dpg.last_item()):
                        dpg.add_text(det, wrap=440)
        if not S["activity"]:
            with dpg.table_row(parent="se_log_table"):
                dpg.add_text(mx("(journal vide ou indisponible)",
                                "(log empty or unavailable)"), color=COL_MUTED)
                for _ in range(3):
                    dpg.add_text("")

    # -- reseau ---------------------------------------------------------
    def _refresh_worker(manual):
        try:
            base, key = creds()
            if not base or not key:
                S["err"] = ("URL Emby ou cle API manquante : renseigne-les dans le "
                            "bandeau de configuration commun, en haut.",
                            "Emby URL or API key missing: set them in the common "
                            "configuration bar at the top.")
                S["sessions"] = []
                post(render)
                return
            tmo = max(5, min(20, interval()))
            try:
                data = ses_get(base, key, "/Sessions", {}, tmo)
            except Exception as exc:
                S["err"] = ("Serveur injoignable : %s" % exc,
                            "Server unreachable: %s" % exc)
                post(render)
                return
            S["sessions"] = data if isinstance(data, list) else (data.get("Items") or [])
            S["err"] = None
            S["last_ok"] = time.time()

            now = time.time()
            if now - S["last_users"] > 300 or not S["users"]:
                try:
                    users = ses_get(base, key, "/Users", {}, tmo)
                    S["users"] = {u.get("Id"): u.get("Name") for u in (users or [])
                                  if isinstance(u, dict)}
                    S["last_users"] = now
                except Exception:
                    pass

            # suivi de la progression : une lecture qui n'avance plus est un
            # fantome laisse par un client parti sans signaler l'arret
            now_t = time.time()
            live = set()
            for sess in S["sessions"]:
                sid = sess.get("Id") or ""
                live.add(sid)
                ps = sess.get("PlayState") or {}
                if sess.get("NowPlayingItem"):
                    pos = ps.get("PositionTicks")
                    prev = S["progress_seen"].get(sid)
                    if prev is None or prev[0] != pos:
                        S["progress_seen"][sid] = (pos, now_t)
                else:
                    S["progress_seen"].pop(sid, None)
            for sid in list(S["progress_seen"]):        # purge des sessions parties
                if sid not in live:
                    S["progress_seen"].pop(sid, None)

            # mediatheque de chaque element en cours + memoire du dernier vu
            for sess in S["sessions"]:
                it = sess.get("NowPlayingItem") or sess.get("NowViewingItem")
                if not it:
                    continue
                lib = library_of(base, key, it, sess.get("UserId"), tmo)
                sid = sess.get("Id") or ""
                if sid:
                    S["last_seen"][sid] = (ses_item_label(it), lib, time.time())

            want_log = S["cfg"]["affichage"]["journal"] == "1"
            due = now - S["last_log"] >= max(15, interval())
            if want_log and (manual or due):
                try:
                    lim = int(S["cfg"]["affichage"]["journal_lignes"] or 40)
                except Exception:
                    lim = 40
                for path, params in (("/System/ActivityLog/Entries",
                                      {"StartIndex": 0, "Limit": lim}),
                                     ("/System/ActivityLog/Entries",
                                      {"startIndex": 0, "limit": lim})):
                    try:
                        j = ses_get(base, key, path, params, tmo)
                        S["activity"] = (j.get("Items") if isinstance(j, dict)
                                         else j) or []
                        S["last_log"] = now
                        break
                    except Exception:
                        continue
            post(render)
        finally:
            S["inflight"] = False

    def load_folders(base, key, tmo):
        """Liste (nom, chemins) des mediatheques : repli quand /Ancestors ne
        donne rien mais que l'element expose son chemin."""
        if S["folders"] is not None:
            return S["folders"]
        out = []
        for path in ("/Library/VirtualFolders", "/Library/MediaFolders"):
            try:
                d = ses_get(base, key, path, {}, tmo)
                items = d.get("Items") if isinstance(d, dict) else d
                for f in (items or []):
                    locs = f.get("Locations") or ([f["Path"]] if f.get("Path") else [])
                    if f.get("Name"):
                        out.append((f["Name"], [str(l) for l in locs]))
                if out:
                    break
            except Exception:
                continue
        S["folders"] = out
        return out

    def library_of(base, key, item, user_id, tmo):
        """Mediatheque contenant l'element (mise en cache par identifiant)."""
        iid = (item or {}).get("Id")
        if not iid:
            return ""
        if iid in S["lib_of"]:
            return S["lib_of"][iid]
        name = ""
        try:
            params = {"UserId": user_id} if user_id else {}
            anc = ses_get(base, key, "/Items/%s/Ancestors" % iid, params, tmo)
            if isinstance(anc, dict):
                anc = anc.get("Items") or []
            anc = anc or []
            for a in anc:                       # le dossier de mediatheque
                if str(a.get("Type") or "") == "CollectionFolder" or a.get("CollectionType"):
                    name = a.get("Name") or ""
                    break
            if not name:                        # sinon : le plus haut avant la racine
                cands = [a for a in anc if str(a.get("Type") or "")
                         not in ("UserRootFolder", "AggregateFolder")]
                if cands:
                    name = cands[-1].get("Name") or ""
        except Exception:
            name = ""
        if not name:                            # repli par le chemin du fichier
            p = str((item or {}).get("Path") or "")
            if p:
                low = p.replace("\\", "/").lower()
                for fname, locs in load_folders(base, key, tmo):
                    for loc in locs:
                        l = str(loc).replace("\\", "/").lower().rstrip("/")
                        if l and low.startswith(l):
                            name = fname
                            break
                    if name:
                        break
        S["lib_of"][iid] = name
        return name

    def interval():
        try:
            return max(2, min(3600, int(S["cfg"]["affichage"]["intervalle"] or 10)))
        except Exception:
            return 10

    def start_refresh(manual=False):
        if S["inflight"]:
            return
        S["inflight"] = True
        S["pending_shown"] = False
        S["last_try"] = time.time()
        threading.Thread(target=_refresh_worker, args=(manual,), daemon=True).start()

    def tick():
        """Appele a chaque frame par la boucle principale."""
        if S["inflight"]:
            # le serveur tarde : on le dit, plutot que de laisser un resume fige
            if not S["pending_shown"] and time.time() - S["last_try"] > 2.0:
                S["pending_shown"] = True
                sv("se_summary", mx("Interrogation du serveur en cours...",
                                    "Querying the server..."))
                cfgi("se_summary", color=COL_WARN)
            return
        if S["cfg"]["affichage"]["auto"] != "1":
            return
        if time.time() - S["last_try"] >= interval():
            start_refresh(False)

    # -- commandes a distance -------------------------------------------
    def _command_worker(sid, cmd):
        base, key = creds()
        try:
            ses_post(base, key, "/Sessions/%s/Playing/%s" % (sid, cmd))
            S["last_try"] = 0.0          # force un rafraichissement immediat
        except Exception as exc:
            post(modal_err, mx("Sessions", "Sessions"),
                 mx("Commande refusee par le serveur :\n%s" % exc,
                    "Command refused by the server:\n%s" % exc))

    def cb_playstate(sender=None, app_data=None, user_data=None):
        sid, cmd = user_data
        threading.Thread(target=_command_worker, args=(sid, cmd), daemon=True).start()

    def cb_ask_stop(sender=None, app_data=None, user_data=None):
        S["stop_target"] = user_data
        who = user_data.get("UserName") or "?"
        what = ses_item_label(user_data.get("NowPlayingItem") or {})
        sv("se_stop_text", mx("Arreter la lecture de %s sur %s ?\n\n%s",
                              "Stop playback for %s on %s?\n\n%s")
           % (who, user_data.get("DeviceName") or "?", what))
        cfgi("se_stop_window", show=True)

    def cb_stop_confirm(sender=None, app_data=None, user_data=None):
        s = S["stop_target"]
        cfgi("se_stop_window", show=False)
        if s:
            threading.Thread(target=_command_worker,
                             args=(s.get("Id") or "", "Stop"), daemon=True).start()

    def cb_stop_cancel(sender=None, app_data=None, user_data=None):
        cfgi("se_stop_window", show=False)

    def cb_ask_message(sender=None, app_data=None, user_data=None):
        S["msg_target"] = user_data
        S["msg_to_name"] = "%s (%s)" % (user_data.get("UserName") or "?",
                                        user_data.get("DeviceName") or "?")
        sv("se_msg_to", "%s  --  %s" % (user_data.get("UserName") or "?",
                                        user_data.get("DeviceName") or "?"))
        cfgi("se_msg_window", show=True)

    def _send_message_route(base, key, sid, header, text, timeout_ms):
        """Endpoint dedie : Text / Header / TimeoutMs sont des parametres d'URL."""
        params = {"Text": text, "Header": header}
        if timeout_ms > 0:
            params["TimeoutMs"] = int(timeout_ms)
        return ses_post(base, key, "/Sessions/%s/Message" % sid, params)

    def _send_command_route(base, key, sid, header, text, timeout_ms):
        """Commande generale : certains clients n'implementent que celle-ci.
        Les arguments d'une GeneralCommand sont des chaines."""
        args = {"Header": header, "Text": text}
        if timeout_ms > 0:
            args["TimeoutMs"] = str(int(timeout_ms))
        return ses_post(base, key, "/Sessions/%s/Command" % sid, None,
                        {"Name": "DisplayMessage", "Arguments": args})

    def _message_worker(sid, header, text, timeout_ms):
        base, key = creds()
        # l'objet recopie dans le texte : seul moyen sur les clients qui
        # ignorent le champ Header (Emby les y autorise explicitement)
        if S["cfg"]["affichage"]["msg_objet_dans_texte"] == "1" and header:
            text = "%s\n\n%s" % (header, text)
        first_is_message = S["cfg"]["affichage"]["msg_methode"] != "commande"
        routes = ([("Message", _send_message_route), ("Command", _send_command_route)]
                  if first_is_message else
                  [("Command", _send_command_route), ("Message", _send_message_route)])
        errs = []
        for name, fn in routes:
            try:
                fn(base, key, sid, header, text, timeout_ms)
                post(flash, "Message envoye a %s (%s)."
                     % (S["msg_to_name"] or "?", name),
                     "Message sent to %s (%s)." % (S["msg_to_name"] or "?", name))
                return
            except Exception as exc:
                errs.append("/Sessions/{id}/%s : %s" % (name, exc))
        post(modal_err, mx("Sessions", "Sessions"),
             mx("Message refuse par le serveur.\n\n%s",
                "Message refused by the server.\n\n%s") % "\n".join(errs))

    def cb_msg_send(sender=None, app_data=None, user_data=None):
        s = S["msg_target"]
        if not s:
            return
        text = (gv("se_in_msg_text") or "").strip()
        if not text:
            return
        header = (gv("se_in_msg_head") or "Emby").strip() or "Emby"
        try:
            secs = int(gv("se_in_msg_timeout") or 0)
        except Exception:
            secs = 0
        cfgi("se_msg_window", show=False)
        threading.Thread(target=_message_worker,
                         args=(s.get("Id") or "", header, text, secs * 1000),
                         daemon=True).start()

    def cb_msg_method(sender=None, app_data=None, user_data=None):
        S["cfg"]["affichage"]["msg_methode"] = (
            "commande" if str(app_data).lower().startswith(("commande", "general"))
            else "message")
        save_cfg()

    def cb_msg_prefix(sender=None, app_data=None, user_data=None):
        S["cfg"]["affichage"]["msg_objet_dans_texte"] = "1" if app_data else "0"
        save_cfg()

    def cb_msg_timeout(sender=None, app_data=None, user_data=None):
        try:
            S["cfg"]["affichage"]["msg_duree"] = str(max(0, int(app_data)))
        except Exception:
            pass
        save_cfg()

    def cb_msg_cancel(sender=None, app_data=None, user_data=None):
        cfgi("se_msg_window", show=False)

    # -- reglages -------------------------------------------------------
    def save_cfg():
        ses_save_config(S["cfg"])

    def cb_interval(sender=None, app_data=None, user_data=None):
        try:
            v = max(2, min(3600, int(app_data)))
        except Exception:
            v = 10
        S["cfg"]["affichage"]["intervalle"] = str(v)
        if v != app_data:
            sv("se_interval", v)
        save_cfg()

    def cb_auto(sender=None, app_data=None, user_data=None):
        S["cfg"]["affichage"]["auto"] = "1" if app_data else "0"
        save_cfg()

    def cb_idle(sender=None, app_data=None, user_data=None):
        S["cfg"]["affichage"]["montrer_inactives"] = "1" if app_data else "0"
        save_cfg()
        render()

    def cb_ghost(sender=None, app_data=None, user_data=None):
        S["cfg"]["affichage"]["masquer_fantomes"] = "1" if app_data else "0"
        save_cfg()
        render()

    def cb_ghost_delay(sender=None, app_data=None, user_data=None):
        try:
            v = max(30, min(3600, int(app_data)))
        except Exception:
            v = 120
        S["cfg"]["affichage"]["fantome_delai"] = str(v)
        if v != app_data:
            sv("se_ghost_delay", v)
        save_cfg()
        render()

    def cb_log(sender=None, app_data=None, user_data=None):
        S["cfg"]["affichage"]["journal"] = "1" if app_data else "0"
        save_cfg()
        cfgi("se_h_log", show=bool(app_data))
        if app_data:
            S["last_log"] = 0.0
            start_refresh(True)

    def cb_refresh(sender=None, app_data=None, user_data=None):
        S["last_try"] = 0.0
        start_refresh(True)

    def cb_log_refresh(sender=None, app_data=None, user_data=None):
        S["last_log"] = 0.0
        start_refresh(True)

    # -- bascule de langue ----------------------------------------------
    def set_lang(code):
        S["lang"] = "en" if str(code).upper() == "EN" else "fr"
        i = 1 if S["lang"] == "en" else 0
        for tag, pair in TR.items():
            if not dpg.does_item_exist(tag):
                continue
            try:
                kind = dpg.get_item_type(tag)
                if ("Button" in kind or "Collapsing" in kind or "Checkbox" in kind
                        or "Window" in kind or "Table" in kind):
                    dpg.configure_item(tag, label=pair[i])
                else:
                    dpg.set_value(tag, pair[i])
            except Exception:
                pass
        for tag, pair in HINTS.items():
            cfgi(tag, hint=pair[i])
        for tag, fr, en in TIPS:
            sv(tag, en if i else fr)
        sv("se_tip_method",
           mx("Message : POST /Sessions/{id}/Message\n"
              "Commande generale : POST /Sessions/{id}/Command (DisplayMessage)\n\n"
              "Les clients Emby n'implementent pas tous les deux de la meme facon. "
              "Si l'objet ou la duree ne sont pas respectes, essaie l'autre.",
              "Message: POST /Sessions/{id}/Message\n"
              "General command: POST /Sessions/{id}/Command (DisplayMessage)\n\n"
              "Emby clients do not all implement both the same way. If the subject "
              "or the display time is ignored, try the other one."))
        sync_method_widget()
        render()          # tableaux entierement reconstruits dans la langue

    def sync_method_widget(_=None):
        """Le bouton radio est traduit : on reecrit ses libelles et sa valeur."""
        items = ((mx("Message", "Message"), mx("Commande generale", "General command")))
        cfgi("se_msg_method", items=list(items))
        sv("se_msg_method",
           items[1] if S["cfg"]["affichage"]["msg_methode"] == "commande" else items[0])

    def push_shared(_c=None):
        S["last_try"] = 0.0          # nouvelle URL/cle : on retente tout de suite

    # -- construction ----------------------------------------------------
    def build_popups():
        with dpg.window(tag="se_msg_window", label="Message a l'ecran", show=False,
                        modal=True, width=580, height=470, pos=(180, 110),
                        no_resize=True):
            with dpg.group(horizontal=True):
                dpg.add_text("Destinataire :", tag="se_t_msg_to", color=COL_MUTED)
                dpg.add_text("", tag="se_msg_to", color=COL_OK)
            dpg.add_spacer(height=4)
            dpg.add_text("Objet", tag="se_t_msg_head")
            dpg.add_input_text(tag="se_in_msg_head", width=-1, default_value="Emby",
                               hint="Emby")
            dpg.add_text("Message", tag="se_t_msg_text")
            dpg.add_input_text(tag="se_in_msg_text", width=-1, multiline=True,
                               height=70,
                               hint="Extinction du serveur dans 10 minutes...")
            with dpg.group(horizontal=True):
                dpg.add_text("Duree d'affichage (s), 0 = attendre la confirmation",
                             tag="se_t_msg_timeout")
                dpg.add_input_int(tag="se_in_msg_timeout", width=90, default_value=8,
                                  min_value=0, max_value=600, min_clamped=True,
                                  max_clamped=True, callback=cb_msg_timeout)
            with dpg.group(horizontal=True):
                dpg.add_text("Methode", tag="se_t_msg_method", color=COL_MUTED)
                dpg.add_radio_button(("Message", "Commande generale"),
                                     tag="se_msg_method", horizontal=True,
                                     callback=cb_msg_method)
                with dpg.tooltip("se_msg_method"):
                    dpg.add_text("Message : POST /Sessions/{id}/Message\n"
                                 "Commande generale : POST /Sessions/{id}/Command "
                                 "(DisplayMessage)\n\n"
                                 "Les clients Emby n'implementent pas tous les deux "
                                 "de la meme facon. Si l'objet ou la duree ne sont pas "
                                 "respectes, essaie l'autre.", wrap=380,
                                 tag="se_tip_method")
            dpg.add_checkbox(label="Ajouter l'objet en tete du texte",
                             tag="se_cb_msg_prefix", default_value=True,
                             callback=cb_msg_prefix)
            dpg.add_text("Emby autorise les clients a ignorer l'objet et la duree "
                         "d'affichage. Si c'est le cas du tien, coche la case ci-dessus "
                         "et/ou essaie l'autre methode.",
                         tag="se_t_msg_note", color=COL_MUTED, wrap=530)
            dpg.add_separator()
            with dpg.group(horizontal=True):
                dpg.add_button(label="Envoyer", tag="se_btn_msg_send", width=120,
                               callback=cb_msg_send)
                dpg.add_button(label="Annuler", tag="se_btn_msg_cancel", width=120,
                               callback=cb_msg_cancel)

        with dpg.window(tag="se_stop_window", label="Arreter la lecture ?", show=False,
                        modal=True, width=520, autosize=True, pos=(200, 200),
                        no_resize=True):
            dpg.add_text("", tag="se_stop_text", wrap=490)
            dpg.add_separator()
            with dpg.group(horizontal=True):
                dpg.add_button(label="Arreter", tag="se_btn_stop_yes", width=120,
                               callback=cb_stop_confirm)
                dpg.add_button(label="Annuler", tag="se_btn_stop_no", width=120,
                               callback=cb_stop_cancel)

    def build_body():
        with dpg.group(horizontal=True):
            dpg.add_text("Sessions Emby", tag="se_t_title", color=ACCENT)
            dpg.add_text("- qui est connecte, qui lit quoi, en direct",
                         tag="se_t_subtitle", color=COL_MUTED)
        dpg.add_separator()

        with dpg.group(horizontal=True):
            dpg.add_button(label="Rafraichir maintenant", tag="se_btn_refresh",
                           callback=cb_refresh)
            tip("Interroge le serveur immediatement, sans attendre le prochain cycle.",
                "Queries the server right away, without waiting for the next cycle.")
            dpg.add_checkbox(label="Rafraichissement auto", tag="se_cb_auto",
                             default_value=True, callback=cb_auto)
            dpg.add_text("toutes les", tag="se_t_every", color=COL_MUTED)
            dpg.add_input_int(tag="se_interval", width=90, default_value=10,
                              min_value=2, max_value=3600, min_clamped=True,
                              max_clamped=True, step=1, callback=cb_interval)
            tip("Intervalle entre deux interrogations du serveur, de 2 a 3600 s.\n"
                "Une interrogation est tres legere : une requete /Sessions.",
                "Delay between two server queries, from 2 to 3600 s.\n"
                "A query is very light: one /Sessions request.")
            dpg.add_text("secondes", tag="se_t_sec", color=COL_MUTED)
            dpg.add_spacer(width=10)
            dpg.add_checkbox(label="Journal du serveur", tag="se_cb_log",
                             default_value=True, callback=cb_log)
        with dpg.group(horizontal=True):
            dpg.add_checkbox(label="Montrer les sessions sans lecture",
                             tag="se_cb_idle", default_value=True, callback=cb_idle)
            tip("Affiche aussi les appareils connectes qui ne lisent rien "
                "(navigation dans la mediatheque, application en veille).",
                "Also shows connected devices that play nothing (browsing the "
                "library, app idle).")
            dpg.add_spacer(width=14)
            dpg.add_checkbox(label="Masquer les lectures fantomes",
                             tag="se_cb_ghost", default_value=True, callback=cb_ghost)
            tip("Emby conserve une session « en lecture » quand un client "
                "disparait sans signaler l'arret (frequent sur iPhone / Android) : "
                "la position reste figee, souvent a la fin du film.\n"
                "Ces sessions sont detectees et masquees ; le resume indique "
                "combien il y en a.",
                "Emby keeps a session \"playing\" when a client vanishes without "
                "reporting the stop (common on iPhone / Android): the position "
                "stays frozen, often at the end of the movie.\n"
                "Those sessions are detected and hidden; the summary says how "
                "many there are.")
            dpg.add_text("au-dela de", tag="se_t_ghost_after", color=COL_MUTED)
            dpg.add_input_int(tag="se_ghost_delay", width=118, default_value=120,
                              min_value=30, max_value=3600, min_clamped=True,
                              max_clamped=True, step=30, callback=cb_ghost_delay)
            dpg.add_text("secondes sans progression", tag="se_t_ghost_unit",
                         color=COL_MUTED)
        dpg.add_text("", tag="se_summary", color=COL_MUTED)
        dpg.add_spacer(height=2)

        with dpg.table(tag="se_table", header_row=True, resizable=True,
                       borders_innerH=True, borders_outerH=True,
                       borders_innerV=True, borders_outerV=True,
                       scrollY=True, height=-230):
            dpg.add_table_column(label="Utilisateur", tag="se_col_user",
                                 width_fixed=True, init_width_or_weight=100)
            dpg.add_table_column(label="Appareil", tag="se_col_device",
                                 width_fixed=True, init_width_or_weight=138)
            dpg.add_table_column(label="Adresse IP", tag="se_col_ip",
                                 width_fixed=True, init_width_or_weight=102)
            dpg.add_table_column(label="Etat", tag="se_col_state",
                                 width_fixed=True, init_width_or_weight=82)
            dpg.add_table_column(label="Mediatheque", tag="se_col_lib",
                                 width_fixed=True, init_width_or_weight=118)
            dpg.add_table_column(label="Contenu", tag="se_col_item",
                                 width_stretch=True, init_width_or_weight=0.45)
            dpg.add_table_column(label="Progression", tag="se_col_prog",
                                 width_fixed=True, init_width_or_weight=140)
            dpg.add_table_column(label="Mode", tag="se_col_mode",
                                 width_fixed=True, init_width_or_weight=105)
            dpg.add_table_column(label="Activite", tag="se_col_seen",
                                 width_fixed=True, init_width_or_weight=92)
            dpg.add_table_column(label="Actions", tag="se_col_act",
                                 width_fixed=True, init_width_or_weight=196)

        dpg.add_spacer(height=3)
        with dpg.collapsing_header(label="Journal d'activite du serveur",
                                   tag="se_h_log", default_open=True):
            with dpg.group(horizontal=True):
                dpg.add_button(label="Recharger le journal", tag="se_btn_log_refresh",
                               small=True, callback=cb_log_refresh)
                tip("Recharge immediatement les dernieres entrees du journal Emby.",
                    "Immediately reloads the latest entries of the Emby log.")
            with dpg.table(tag="se_log_table", header_row=True, resizable=True,
                           borders_innerH=True, borders_outerH=True,
                           borders_innerV=True, borders_outerV=True,
                           scrollY=True, height=-1):
                dpg.add_table_column(label="Date", tag="se_lcol_date",
                                     width_fixed=True, init_width_or_weight=110)
                dpg.add_table_column(label="Utilisateur", tag="se_lcol_user",
                                     width_fixed=True, init_width_or_weight=115)
                dpg.add_table_column(label="Evenement", tag="se_lcol_event",
                                     width_stretch=True, init_width_or_weight=0.42)
                dpg.add_table_column(label="Detail", tag="se_lcol_detail",
                                     width_stretch=True, init_width_or_weight=0.58)

        # etat initial depuis le fichier de configuration
        c = S["cfg"]["affichage"]
        sv("se_interval", interval())
        sv("se_cb_auto", c["auto"] == "1")
        sv("se_cb_idle", c["montrer_inactives"] == "1")
        sv("se_cb_log", c["journal"] == "1")
        sv("se_cb_ghost", c["masquer_fantomes"] == "1")
        sv("se_ghost_delay", ghost_delay())
        cfgi("se_h_log", show=c["journal"] == "1")
        sv("se_cb_msg_prefix", c["msg_objet_dans_texte"] == "1")
        try:
            sv("se_in_msg_timeout", int(c["msg_duree"] or 8))
        except Exception:
            pass
        sync_method_widget()
        render_summary()

    return {
        "build_popups": build_popups,
        "build_body": build_body,
        "set_lang": set_lang,
        "push_shared": push_shared,
        "tick": tick,
        "connect": lambda: (S.__setitem__("last_try", 0.0), start_refresh(True))[1],
        "save": save_cfg,
        "state": S,
    }


def main():
    global _render_timer
    # sous-processus du selecteur de dossier de l'onglet MKV Renamer :
    # il ne doit surtout pas relancer toute l'application.
    if "--pick-folder" in sys.argv:
        return mkv_pick_folder_child()
    if "--mkv-selftest" in sys.argv:
        return mkv_selftest()
    dpg.create_context()
    setup_theme()                      # theme sombre global (genres)
    DBL = _init_doublons()             # espace de noms doublons
    MKV = _init_mkv()                  # espace de noms MKV Renamer
    SES = _init_sessions()             # espace de noms Sessions
    app = App()                        # instance RefMatch

    dpg.create_viewport(title="Emby Toolbox  -  Genres / IDFinder / Doublons "
                              "/ MKV / Sessions",
                        width=1320, height=920, min_width=1024, min_height=640)

    # police chargee + liee GLOBALEMENT avant toute UI (sinon le bandeau de
    # config commune, construit hors de l'onglet, garde la police par defaut
    # et affiche « ? » a la place de —, →, …)
    try:
        app._build_font()
    except Exception:
        pass

    # modales / popups (hors onglets, niveau racine)
    build_genres_popups()
    DBL["build_popups"]()
    MKV["build_popups"]()
    SES["build_popups"]()

    # ----- cablage de la configuration commune -----
    def _gv(t):
        return dpg.get_value(t) if dpg.does_item_exist(t) else ""

    def _shared_collect():
        return dict(url=_gv("sh_url"), api_key=_gv("sh_key"), user_id=_gv("sh_uid"),
                    tmdb_key=_gv("sh_tmdb"), omdb_key=_gv("sh_omdb"),
                    nas_prefix=_gv("sh_prefix"), nas_unc=_gv("sh_unc"),
                    player=_gv("sh_player"), lang=_gv("sh_lang") or "FR")

    def _shared_apply():
        save_shared_creds(**_shared_collect())
        push_shared_to_all_tabs()

    # masquage DEFENSIF des blocs de config dupliques dans les onglets :
    # appele apres construction ET a chaque changement de langue, pour qu'aucun
    # chemin (rendu, traduction) ne puisse les faire reapparaitre.
    _DUP_CFG_TAGS = ("g_cfg_conn", "g_cfg_nas", "g_cfg_player",
                     "grp_omdb_key", "grp_tmdb_key", "btn_connect",
                     "h_conn", "dbl_cfg_conn", "dbl_cfg_nas",
                     "dbl_btn_connect", "dbl_btn_lang")

    def _harden_hide():
        for tg in _DUP_CFG_TAGS:
            if dpg.does_item_exist(tg):
                try:
                    dpg.configure_item(tg, show=False)
                except Exception:
                    pass

    _SHBAR_TR = {
        "cfg_header": ("Configuration commune  -  Emby, cles API et langue (partagees par les 5 outils)",
                       "Common configuration  -  Emby, API keys and language (shared by all 5 tools)"),
        "sh_l_url": ("URL Emby", "Emby URL"),
        "sh_l_key": ("Clé API", "API key"),
        "sh_l_uid": ("User ID", "User ID"),
        "sh_l_tmdb": ("Clé TMDB", "TMDB key"),
        "sh_l_omdb": ("Clé OMDB", "OMDB key"),
        "sh_l_lang": ("Langue", "Language"),
        "sh_l_prefix": ("Préfixe NAS", "NAS prefix"),
        "sh_l_player": ("Lecteur", "Player"),
        "sh_connect": ("Connecter", "Connect"),
        "sh_save": ("Enregistrer", "Save"),
    }

    def _apply_shared_bar_lang(code):
        i = 1 if str(code).upper() == "EN" else 0
        for tag, pair in _SHBAR_TR.items():
            if not dpg.does_item_exist(tag):
                continue
            try:
                kind = dpg.get_item_type(tag)
                if "Button" in kind or "Collapsing" in kind:
                    dpg.configure_item(tag, label=pair[i])
                else:
                    dpg.set_value(tag, pair[i])
            except Exception:
                pass

    def _apply_lang(code):
        code = (code or "FR").upper()
        try:
            app.lang = code
            app.apply_language()
        except Exception:
            pass
        try:
            DBL["set_lang"](code)
        except Exception:
            pass
        try:
            MKV["set_lang"](code)          # onglet MKV Renamer (FR/EN)
        except Exception:
            pass
        try:
            SES["set_lang"](code)          # onglet Sessions (FR/EN)
        except Exception:
            pass
        try:
            en = (code == "EN")
            for tag, fr_lbl, en_lbl in (
                    ("tab_lbl_idfinder", "IDFinder", "IDFinder"),
                    ("tab_lbl_doublons", "Doublons", "Duplicates"),
                    ("tab_lbl_genres", "Explorateur de genres", "Genre explorer"),
                    ("tab_lbl_mkv", "MKV Renamer", "MKV Renamer"),
                    ("tab_lbl_sessions", "Sessions", "Sessions")):
                if dpg.does_item_exist(tag):
                    dpg.configure_item(tag, label=en_lbl if en else fr_lbl)
            dpg.set_viewport_title(
                "Emby Toolbox  -  Genres / IDFinder / Duplicates / MKV / Sessions"
                if en else
                "Emby Toolbox  -  Genres / IDFinder / Doublons / MKV / Sessions")
        except Exception:
            pass
        try:
            genres_apply_lang(code)        # onglet Genres (FR/EN)
        except Exception:
            pass
        _apply_shared_bar_lang(code)        # bandeau de config commun
        _harden_hide()

    def _on_shared_lang(sender, app_data, user_data):
        code = (app_data or _gv("sh_lang") or "FR").upper()
        save_shared_creds(lang=code)
        _apply_lang(code)

    def _is_en():
        return (_gv("sh_lang") or "FR").upper() == "EN"

    def _on_shared_save(sender, app_data, user_data):
        _shared_apply()
        # persiste aussi les options propres a chaque outil
        # (mediatheques selectionnees, type Films/Series, tolerance pour RefMatch,
        #  dossier + format de nom pour MKV Renamer)
        for fn in (lambda: app.on_save(None, None, None),
                   lambda: DBL["save"](),
                   lambda: MKV["save"](),
                   lambda: SES["save"]()):
            try:
                fn()
            except Exception:
                pass
        dpg.set_value("sh_status", "Saved" if _is_en() else "Enregistré")

    def _on_shared_connect(sender, app_data, user_data):
        _shared_apply()
        for fn in (lambda: do_connect(),
                   lambda: app.on_connect(None, None, None),
                   lambda: DBL["connect"](),
                   lambda: SES["connect"]()):
            try:
                fn()
            except Exception:
                pass
        dpg.set_value("sh_status", "Connected" if _is_en() else "Connecté")

    def _push_shared_bar(c):
        m = {"sh_url": "url", "sh_key": "api_key", "sh_uid": "user_id",
             "sh_tmdb": "tmdb_key", "sh_omdb": "omdb_key", "sh_prefix": "nas_prefix",
             "sh_unc": "nas_unc", "sh_player": "player", "sh_lang": "lang"}
        for tag, k in m.items():
            if dpg.does_item_exist(tag):
                dpg.set_value(tag, c.get(k, "") or ("FR" if tag == "sh_lang" else ""))

    def _browse_shared_player(sender, app_data, user_data):
        """Ouvre un sélecteur de fichier pour le lecteur vidéo (bouton ...)."""
        try:
            import tkinter as tk
            from tkinter import filedialog
            r = tk.Tk(); r.withdraw(); r.attributes("-topmost", True)
            fp = filedialog.askopenfilename(
                title="Lecteur vidéo",
                filetypes=[("Exécutables", "*.exe"), ("Tous les fichiers", "*.*")],
                initialdir=r"C:\Program Files")
            r.destroy()
            if fp and dpg.does_item_exist("sh_player"):
                dpg.set_value("sh_player", fp)
                save_shared_creds(player=fp)
                push_shared_to_all_tabs()
        except Exception:
            pass

    with dpg.window(tag="host_win", no_title_bar=True, no_move=True,
                    no_resize=True, no_scrollbar=True, no_scroll_with_mouse=True):
        # ===== Configuration commune aux 5 outils =====
        with dpg.collapsing_header(
                tag="cfg_header", default_open=True,
                label="Configuration commune  -  Emby, cles API et langue (partagees par les 5 outils)"):
            with dpg.group(horizontal=True):
                dpg.add_text("URL Emby", tag="sh_l_url")
                dpg.add_input_text(tag="sh_url", width=240)
                dpg.add_text("Clé API", tag="sh_l_key")
                dpg.add_input_text(tag="sh_key", width=240, password=True)
                dpg.add_text("User ID", tag="sh_l_uid")
                dpg.add_input_text(tag="sh_uid", width=110, hint="optionnel")
            with dpg.group(horizontal=True):
                dpg.add_text("Clé TMDB", tag="sh_l_tmdb")
                dpg.add_input_text(tag="sh_tmdb", width=240, password=True,
                                   hint="themoviedb.org")
                dpg.add_button(label="obtenir", width=60, tag="sh_tmdb_link",
                    callback=lambda s,a,u: webbrowser.open(
                        "https://www.themoviedb.org/settings/api"))
                with dpg.tooltip("sh_tmdb_link"):
                    dpg.add_text("Ouvre la page TMDB pour créer/copier votre clé API "
                                 "(gratuite).", wrap=300)
                dpg.add_text("Clé OMDB", tag="sh_l_omdb")
                dpg.add_input_text(tag="sh_omdb", width=240, password=True,
                                   hint="omdbapi.com")
                dpg.add_button(label="obtenir", width=60, tag="sh_omdb_link",
                    callback=lambda s,a,u: webbrowser.open(
                        "https://www.omdbapi.com/apikey.aspx"))
                with dpg.tooltip("sh_omdb_link"):
                    dpg.add_text("Ouvre la page OMDB pour obtenir une clé API "
                                 "gratuite (1000 requêtes/jour).", wrap=300)
                dpg.add_text("Langue", tag="sh_l_lang")
                dpg.add_radio_button(("FR", "EN"), tag="sh_lang", horizontal=True,
                                     default_value="FR", callback=_on_shared_lang)
            with dpg.group(horizontal=True):
                dpg.add_text("Préfixe NAS", tag="sh_l_prefix")
                dpg.add_input_text(tag="sh_prefix", width=150, hint="/volume1")
                dpg.add_text("->")
                dpg.add_input_text(tag="sh_unc", width=220, hint=r"\\192.168.1.x")
                dpg.add_text("Lecteur", tag="sh_l_player")
                dpg.add_input_text(tag="sh_player", width=200, hint=r"C:\...\vlc.exe")
                dpg.add_button(label="...", width=28, tag="sh_player_browse",
                               callback=_browse_shared_player)
            with dpg.group(horizontal=True):
                b = dpg.add_button(label="Connecter", tag="sh_connect",
                                   callback=_on_shared_connect, width=120)
                try:
                    dpg.bind_item_theme(b, "th_btn_ok")
                except Exception:
                    pass
                dpg.add_button(label="Enregistrer", tag="sh_save",
                               callback=_on_shared_save, width=120)
                dpg.add_spacer(width=10)
                dpg.add_text("", tag="sh_status", color=(46, 204, 113))
        dpg.add_separator()

        with dpg.tab_bar():
            with dpg.tab(label="IDFinder", tag="tab_lbl_idfinder"):
                with dpg.child_window(tag="tab_host_refmatch", border=False):
                    app.build_ui()
            with dpg.tab(label="Doublons", tag="tab_lbl_doublons"):
                with dpg.child_window(tag="tab_host_doublons", border=False):
                    DBL["build_body"]()
            with dpg.tab(label="Explorateur de genres", tag="tab_lbl_genres"):
                with dpg.child_window(tag="tab_host_genres", border=False):
                    build_genres_body()
            with dpg.tab(label="MKV Renamer", tag="tab_lbl_mkv"):
                with dpg.child_window(tag="tab_host_mkv", border=False):
                    MKV["build_body"]()
            with dpg.tab(label="Sessions", tag="tab_lbl_sessions"):
                with dpg.child_window(tag="tab_host_sessions", border=False):
                    SES["build_body"]()

    # theme rouge des barres de progression (genres + doublons)
    with dpg.theme() as pb_th:
        with dpg.theme_component(dpg.mvProgressBar):
            dpg.add_theme_color(dpg.mvThemeCol_PlotHistogram, (233, 69, 96))
            dpg.add_theme_color(dpg.mvThemeCol_FrameBg, (40, 20, 30))
    for tg in ("scan_pb", "apply_pb", "dbl_scan_pb", "mk_progress"):
        if dpg.does_item_exist(tg):
            dpg.bind_item_theme(tg, pb_th)

    # theme ORANGE VIF des boutons de recherche / scan (haute visibilite)
    with dpg.theme() as scan_btn_th:
        with dpg.theme_component(dpg.mvButton):
            dpg.add_theme_color(dpg.mvThemeCol_Button, (235, 130, 18))
            dpg.add_theme_color(dpg.mvThemeCol_ButtonHovered, (252, 162, 48))
            dpg.add_theme_color(dpg.mvThemeCol_ButtonActive, (198, 104, 8))
            dpg.add_theme_color(dpg.mvThemeCol_Text, (20, 18, 10))
            dpg.add_theme_style(dpg.mvStyleVar_FrameRounding, 5)
            dpg.add_theme_style(dpg.mvStyleVar_FramePadding, 12, 6)
    for tg in ("btn_scan", "b_scan", "dbl_btn_scan", "mk_btn_scan",
               "se_btn_refresh"):
        if dpg.does_item_exist(tg):
            dpg.bind_item_theme(tg, scan_btn_th)

    # theme TEAL du bouton Enrichir (Genres) : bien visible, distinct du scan
    with dpg.theme() as enrich_btn_th:
        with dpg.theme_component(dpg.mvButton):
            dpg.add_theme_color(dpg.mvThemeCol_Button, (0, 150, 160))
            dpg.add_theme_color(dpg.mvThemeCol_ButtonHovered, (10, 182, 194))
            dpg.add_theme_color(dpg.mvThemeCol_ButtonActive, (0, 118, 128))
            dpg.add_theme_color(dpg.mvThemeCol_Text, (10, 20, 22))
            dpg.add_theme_style(dpg.mvStyleVar_FrameRounding, 5)
            dpg.add_theme_style(dpg.mvStyleVar_FramePadding, 12, 6)
    if dpg.does_item_exist("btn_enrich"):
        dpg.bind_item_theme("btn_enrich", enrich_btn_th)

    # identifiants communs : migration unique + propagation initiale
    register_shared_pusher(_push_genres)
    register_shared_pusher(_push_refmatch)
    register_shared_pusher(_push_doublons)
    register_shared_pusher(MKV["push_shared"])
    register_shared_pusher(SES["push_shared"])
    register_shared_pusher(_push_shared_bar)
    migrate_into_shared(app, DBL)
    push_shared_to_all_tabs()
    _apply_lang(get_shared_creds().get("lang", "FR"))   # langue commune au demarrage
    _harden_hide()                                       # masquage defensif final

    dpg.setup_dearpygui()
    dpg.show_viewport()
    dpg.set_primary_window("host_win", True)

    def _layout():
        vw = dpg.get_viewport_client_width()
        vh = dpg.get_viewport_client_height()
        dpg.set_item_width("host_win", vw)
        dpg.set_item_height("host_win", vh)
        # hauteur du bandeau de config commune (repliable) -> on adapte
        hh = 150
        try:
            rh = dpg.get_item_rect_size("cfg_header")[1]
            if rh and rh > 0:
                hh = rh
        except Exception:
            pass
        top = hh + 24                  # bandeau + separateur
        # hauteur DEFINIE des conteneurs d'onglet : indispensable pour que les
        # zones internes en height=-1 (resultats, candidats) etablissent un
        # ascenseur au lieu de deborder hors ecran (bouton Appliquer cache).
        for tg in ("tab_host_genres", "tab_host_refmatch", "tab_host_doublons"):
            if dpg.does_item_exist(tg):
                dpg.set_item_width(tg, max(320, vw - 16))
                dpg.set_item_height(tg, max(220, vh - top - 44))
        # onglet MKV Renamer : sa barre de statut et son journal sont en bas, donc
        # la hauteur est calee sur la position REELLE du conteneur a l'ecran
        # (get_item_rect_size d'un collapsing_header ne compte pas son contenu
        #  deplie, ce qui poussait le bas de l'onglet hors de la fenetre).
        for host, anchor in (("tab_host_mkv", "mk_t_title"),
                             ("tab_host_sessions", "se_t_title")):
            if not dpg.does_item_exist(host):
                continue
            dpg.set_item_width(host, max(320, vw - 16))
            h = max(220, vh - top - 44)
            try:
                # le 1er element de l'onglet donne le haut REEL du contenu,
                # bandeau de configuration deplie ou replie.
                y = dpg.get_item_rect_min(anchor)[1]
                if y > 0:
                    h = max(220, vh - y - 14)
            except Exception:
                pass
            dpg.set_item_height(host, h)

    dpg.set_viewport_resize_callback(lambda: _layout())
    _layout()                          # dimensionnement initial immediat

    while dpg.is_dearpygui_running():
        drain_ui_queue()
        _layout()                      # s'adapte au repli/depli du bandeau config
        # rendu differe de l'onglet genres
        if _render_timer > 0 and time.time() >= _render_timer:
            _render_timer = 0.0
            render_results()
        # rendu differe de l'onglet doublons
        DBL["tick"]()
        # rafraichissement periodique de l'onglet sessions
        SES["tick"]()
        dpg.render_dearpygui_frame()

    dpg.destroy_context()


if __name__ == "__main__":
    main()
