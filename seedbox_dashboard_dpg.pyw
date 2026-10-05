#!/usr/bin/env pythonw
# -*- coding: utf-8 -*-
"""
Seedbox Dashboard  -  DearPyGui (DirectX 11)
=============================================
Pilote une seedbox depuis le poste Windows :

  - AJOUT AUTOMATIQUE : dossier surveille -> tout .torrent qui y tombe part
    sur la seedbox (puis est archive, supprime ou laisse sur place au choix).
    Ajout manuel de fichiers et de liens magnet egalement.
  - TABLEAU DE BORD : etat des telechargements en temps reel (progression,
    vitesses, ETA, ratio, seeds/peers), rafraichissement automatique,
    actions pause / reprise / verification / suppression.
  - VERIFICATION NAVIGATEUR : un script Tampermonkey interroge l'outil
    (serveur local 127.0.0.1) au survol d'un nom de film ou a l'ouverture
    d'une fiche : deja sur Emby / sur le disque ? et differences visibles
    (VFQ au lieu de VFF, BLURAY au lieu de WEB-DL, 1080p au lieu de 4K...).

L'outil est AGNOSTIQUE du client : il detecte tout seul qBittorrent,
rTorrent (ruTorrent) ou Deluge et parle le protocole qui va bien.

  qBittorrent : Web API v2   (/api/v2/..., cookie de session)
  rTorrent    : XML-RPC      (/plugins/rpc/rpc.php ou /RPC2, auth basic)
  Deluge      : JSON-RPC     (/json, cookie de session)

Aucune dependance en dehors de dearpygui : urllib + http.cookiejar +
xmlrpc.client, tous dans la bibliotheque standard.

Les identifiants sont chiffres (DPAPI sous Windows, Fernet sinon) avec la
meme clef emby_secret.key que les autres outils du dossier.
"""

import dearpygui.dearpygui as dpg
import os, sys, re, json, csv, time, queue, base64, uuid, shutil
import threading, configparser, traceback, unicodedata
from difflib import SequenceMatcher
import urllib.request, urllib.parse, urllib.error
import http.cookiejar, ssl, socket, hashlib, subprocess
import xmlrpc.client
import secrets, webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

APP_TITLE = "Seedbox Dashboard  -  ajout automatique & suivi"
FONT_SIZE = 16

if getattr(sys, "frozen", False):
    APP_DIR = Path(sys.executable).resolve().parent
else:
    APP_DIR = Path(__file__).resolve().parent

LOG_FILE = APP_DIR / "seedbox_dashboard.log"


def log(msg):
    line = "[%s] %s" % (time.strftime("%H:%M:%S"), msg)
    print(line)
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


# =====================================================================
#  Chiffrement partage (meme clef que Emby Toolbox / Torrent Checker)
# =====================================================================
_SECRET_KEYFILE = str(APP_DIR / "emby_secret.key")

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
        if not fn(ctypes.byref(blob_in), None, None, None, None, 0,
                  ctypes.byref(blob_out)):
            raise OSError("DPAPI a echoue")
        try:
            return ctypes.string_at(blob_out.pbData, blob_out.cbData)
        finally:
            ctypes.windll.kernel32.LocalFree(blob_out.pbData)

    def _dpapi_encrypt(d):
        return _dpapi(d, ctypes.windll.crypt32.CryptProtectData)

    def _dpapi_decrypt(d):
        return _dpapi(d, ctypes.windll.crypt32.CryptUnprotectData)


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


def encrypt_secret(plain):
    if not plain or plain.startswith("enc:"):
        return plain or ""
    if _IS_WIN:
        try:
            return "enc:dpapi:" + base64.b64encode(
                _dpapi_encrypt(plain.encode("utf-8"))).decode("ascii")
        except Exception:
            pass
    if _CRYPTO_OK:
        try:
            return "enc:fernet:" + _fernet().encrypt(
                plain.encode("utf-8")).decode("ascii")
        except Exception:
            pass
    return plain


def decrypt_secret(stored):
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
    return stored


# =====================================================================
#  File UI partagee
# =====================================================================
_ui_queue = queue.Queue()


def ui(fn):
    _ui_queue.put(fn)


def drain_ui_queue():
    while True:
        try:
            fn = _ui_queue.get_nowait()
        except queue.Empty:
            break
        try:
            fn()
        except Exception:
            traceback.print_exc()


# =====================================================================
#  Configuration
# =====================================================================
CREDS_FILE = APP_DIR / "seedbox_creds.ini"
CFG_FILE = APP_DIR / "seedbox_dashboard.ini"

_CRED_FIELDS = ["url", "user", "password", "client", "verify_ssl"]
_CRED_SECRET = {"password"}

_CFG_DEFAULTS = {
    "watch_dir": "",
    "watch_enabled": False,
    "watch_interval": 20,
    "after_send": "archiver",      # archiver | supprimer | rien
    "archive_dir": "",
    "category": "",
    "save_path": "",
    "add_paused": False,
    "refresh": 5,
    "sort_key": "state",
    "sort_dir": 1,
    "auto_refresh": True,
    "quota_go": 0,          # capacite totale annoncee par l'offre (0 = inconnue)
    "min_free_go": 10,      # marge de securite a preserver
    "check_space": True,    # refuser un ajout qui ne tiendrait pas
    "count_paused": False,  # reserver aussi l'espace des torrents dormants
    "emby_fuzzy": 80,       # seuil de rapprochement approximatif des titres
    "emby_dup": "demander", # deja present : demander | ignorer | envoyer
    "refus_dir": "",        # ou atterrissent les torrents deja possedes
    "local_dirs": [],       # dossiers compares en plus d'Emby
    "vue": "Tout",          # jeu de colonnes : Tout | Comparaison | Transfert
    "snap_enabled": True,   # sauvegarde automatique de la liste des torrents
    "snap_dir": "",         # vide = sous-dossier seedbox_snapshots
    "snap_interval": 10,    # minutes entre deux sauvegardes de routine
    "snap_keep": 60,        # jours de conservation des instantanes quotidiens
    "browser_enabled": True,  # serveur local interroge par le script navigateur
    "browser_port": 8765,
    "browser_token": "",      # genere au premier lancement
}


def load_creds():
    d = {"url": "", "user": "", "password": "", "client": "auto",
         "verify_ssl": "1"}
    if CREDS_FILE.exists():
        cfg = configparser.ConfigParser()
        try:
            cfg.read(CREDS_FILE, encoding="utf-8")
            if cfg.has_section("seedbox"):
                for k in _CRED_FIELDS:
                    v = cfg["seedbox"].get(k)
                    if v is not None:
                        d[k] = v
        except Exception:
            pass
        for k in _CRED_SECRET:
            d[k] = decrypt_secret(d.get(k, ""))
    return d


def save_creds(**up):
    d = load_creds()
    d.update({k: v for k, v in up.items() if k in _CRED_FIELDS and v is not None})
    out = dict(d)
    for k in _CRED_SECRET:
        out[k] = encrypt_secret(str(d.get(k, "")))
    out = {k: str(v) for k, v in out.items()}
    cfg = configparser.ConfigParser()
    cfg["seedbox"] = out
    try:
        with open(CREDS_FILE, "w", encoding="utf-8") as f:
            cfg.write(f)
    except Exception as exc:
        log("save_creds: %s" % exc)


def load_cfg():
    d = dict(_CFG_DEFAULTS)
    if CFG_FILE.exists():
        try:
            raw = json.loads(CFG_FILE.read_text(encoding="utf-8"))
            for k, v in raw.items():
                if k in d:
                    d[k] = v
        except Exception:
            pass
    return d


def save_cfg(d):
    try:
        CFG_FILE.write_text(json.dumps(d, ensure_ascii=False, indent=2),
                            encoding="utf-8")
    except Exception as exc:
        log("save_cfg: %s" % exc)


# =====================================================================
#  COUCHE HTTP  (cookies + multipart, sans dependance externe)
# =====================================================================
class HttpError(Exception):
    def __init__(self, status, body=""):
        super().__init__("HTTP %s" % status)
        self.status = status
        self.body = body


def multipart_body(fields=None, files=None):
    """Construit un corps multipart/form-data. files = [(champ, nom, octets)]"""
    boundary = "----seedboxdash" + uuid.uuid4().hex
    out = []
    for k, v in (fields or {}).items():
        out.append(("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n"
                    % (boundary, k, v)).encode("utf-8"))
    for field, fname, data in (files or []):
        out.append(("--%s\r\nContent-Disposition: form-data; name=\"%s\"; "
                    "filename=\"%s\"\r\nContent-Type: application/x-bittorrent"
                    "\r\n\r\n" % (boundary, field, fname)).encode("utf-8"))
        out.append(data)
        out.append(b"\r\n")
    out.append(("--%s--\r\n" % boundary).encode("utf-8"))
    return "multipart/form-data; boundary=%s" % boundary, b"".join(out)


class Http:
    """Petit client HTTP a session : cookies conserves, auth basic optionnelle,
    SSL verifiable ou non (certificats auto-signes de certaines seedbox)."""

    def __init__(self, base, user="", password="", verify=True, timeout=25):
        self.base = base.rstrip("/")
        self.user = user
        self.password = password
        self.timeout = timeout
        self.cj = http.cookiejar.CookieJar()
        handlers = [urllib.request.HTTPCookieProcessor(self.cj)]
        if not verify:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            handlers.append(urllib.request.HTTPSHandler(context=ctx))
        self.op = urllib.request.build_opener(*handlers)
        self.basic = ""
        if user:
            tok = base64.b64encode(("%s:%s" % (user, password)).encode()).decode()
            self.basic = "Basic " + tok

    def request(self, path, data=None, headers=None, method=None,
                use_basic=False, timeout=None):
        url = path if path.startswith("http") else self.base + path
        h = {"Accept": "*/*", "User-Agent": "SeedboxDashboard/1.0"}
        if use_basic and self.basic:
            h["Authorization"] = self.basic
        h.update(headers or {})
        req = urllib.request.Request(url, data=data, headers=h, method=method)
        try:
            with self.op.open(req, timeout=timeout or self.timeout) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            body = ""
            try:
                body = e.read().decode("utf-8", "replace")[:400]
            except Exception:
                pass
            raise HttpError(e.code, body)
        except urllib.error.URLError as e:
            raise HttpError(0, str(e.reason))
        except socket.timeout:
            raise HttpError(0, "delai depasse")

    def post_form(self, path, fields, **kw):
        data = urllib.parse.urlencode(fields).encode()
        hd = {"Content-Type": "application/x-www-form-urlencoded",
              "Referer": self.base}
        hd.update(kw.pop("headers", {}))
        return self.request(path, data=data, headers=hd, method="POST", **kw)

    def post_json(self, path, obj, **kw):
        data = json.dumps(obj).encode()
        hd = {"Content-Type": "application/json"}
        hd.update(kw.pop("headers", {}))
        return self.request(path, data=data, headers=hd, method="POST", **kw)

    def get(self, path, **kw):
        return self.request(path, **kw)


# =====================================================================
#  LECTURE DE LA TAILLE D'UN .torrent  (bencode minimal, sans dependance)
# =====================================================================
def _bdec(raw, i):
    c = raw[i:i + 1]
    if c == b"i":
        j = raw.index(b"e", i)
        return int(raw[i + 1:j]), j + 1
    if c == b"l":
        i += 1
        out = []
        while raw[i:i + 1] != b"e":
            if i >= len(raw):
                raise ValueError("liste non terminee")
            v, i = _bdec(raw, i)
            out.append(v)
        return out, i + 1
    if c == b"d":
        i += 1
        out = {}
        while raw[i:i + 1] != b"e":
            if i >= len(raw):
                raise ValueError("dictionnaire non termine")
            k, i = _bdec(raw, i)
            v, i = _bdec(raw, i)
            out[k] = v
        return out, i + 1
    if c.isdigit():
        j = raw.index(b":", i)
        n = int(raw[i:j])
        return raw[j + 1:j + 1 + n], j + 1 + n
    raise ValueError("octet inattendu")


def torrent_info(data, fallback_name=""):
    """(nom interne, taille totale) d'un .torrent. Replie sur le nom de
    fichier si le bencode est illisible."""
    name, size = "", 0
    try:
        meta, _ = _bdec(data, 0)
        info = meta.get(b"info") or {}
        raw = info.get(b"name.utf-8") or info.get(b"name") or b""
        if isinstance(raw, bytes):
            for enc in ("utf-8", "cp1252", "latin-1"):
                try:
                    name = raw.decode(enc)
                    break
                except Exception:
                    continue
        if b"files" in info:
            size = sum(int(f.get(b"length", 0) or 0)
                       for f in info.get(b"files") or [])
        else:
            size = int(info.get(b"length", 0) or 0)
    except Exception:
        pass
    return (name or fallback_name), size


def torrent_size(data):
    return torrent_info(data)[1]


def torrent_infohash(raw):
    """SHA-1 du dictionnaire 'info' brut, sans re-encodage."""
    try:
        if raw[0:1] != b"d":
            return ""
        i = 1
        while raw[i:i + 1] != b"e":
            k, i = _bdec(raw, i)
            start = i
            v, i = _bdec(raw, i)
            if k == b"info":
                return hashlib.sha1(raw[start:i]).hexdigest()
        return ""
    except Exception:
        return ""


_thash_cache = {}


def find_local_torrent(infohash):
    """Cherche dans les dossiers connus un .torrent ayant cet infohash.

    Bien plus fiable qu'un magnet pour un tracker prive : le fichier contient
    l'annonce et les cles necessaires, la ou un magnet dependrait du DHT
    souvent desactive sur ces trackers.
    """
    if not infohash:
        return None
    roots = []
    for d in (CFG.get("watch_dir", ""), CFG.get("archive_dir", "")):
        if d and os.path.isdir(d):
            roots.append(d)
    wd = CFG.get("watch_dir", "")
    if wd and os.path.isdir(os.path.join(wd, "_envoyes")):
        roots.append(os.path.join(wd, "_envoyes"))
    for root in roots:
        for dirpath, _dirs, files in os.walk(root):
            for f in files:
                if not f.lower().endswith(".torrent"):
                    continue
                full = os.path.join(dirpath, f)
                try:
                    st = os.stat(full)
                    sig = (int(st.st_mtime), st.st_size)
                except Exception:
                    continue
                hit = _thash_cache.get(full)
                if not hit or hit[0] != sig:
                    try:
                        raw = open(full, "rb").read()
                    except Exception:
                        continue
                    hit = (sig, torrent_infohash(raw))
                    _thash_cache[full] = hit
                if hit[1] and hit[1].lower() == infohash.lower():
                    try:
                        return open(full, "rb").read()
                    except Exception:
                        return None
    return None


# =====================================================================
#  MODELE COMMUN
# =====================================================================
# etats normalises : downloading, seeding, paused, checking, queued,
#                    stalled, error, completed
ST_COL = {
    "downloading": (90, 190, 255), "seeding": (46, 204, 113),
    "paused": (150, 150, 175), "checking": (235, 190, 20),
    "queued": (170, 150, 220), "stalled": (200, 160, 90),
    "error": (215, 75, 90), "completed": (120, 200, 180),
    "unknown": (190, 130, 190),
}
ST_LBL = {
    "downloading": "Telechargement", "seeding": "Seed", "paused": "En pause",
    "checking": "Verification", "queued": "En file", "stalled": "Bloque",
    "error": "Erreur", "completed": "Termine", "unknown": "Etat inconnu",
}

DONE_STATES = ("seeding", "completed")


def remaining(t):
    return max(0, t["size"] - t["downloaded"])


def is_firm(t):
    """Ce torrent va-t-il REELLEMENT continuer d'ecrire sur le disque ?

    On ne se fie volontairement pas au seul libelle d'etat. Un torrent
    'stalled' est actif au sens du client, mais s'il n'a aucun pair il
    n'ecrira jamais rien : tracker mort, torrent orphelin, release disparue.
    Reserver sa taille bloquerait des imports parfaitement legitimes, ce qui
    est exactement le probleme qu'on cherche a eviter.
    """
    if not remaining(t) or t["state"] in DONE_STATES:
        return False
    if t["state"] in ("downloading", "queued", "checking"):
        return True
    if t["state"] == "stalled":
        return (t["seeds"] + t["peers"]) > 0
    return False        # paused, error, unknown


def is_dormant(t):
    """Incomplet mais qui n'ecrit pas : ne reserve rien, mais reste affiche."""
    return bool(remaining(t)) and t["state"] not in DONE_STATES and not is_firm(t)

_seen_unknown = set()


def map_state(raw, table, fallback="unknown"):
    """Traduit l'etat brut du client. Un etat non cartographie devient
    'unknown' (et non 'queued') : il ne doit pas reserver de l'espace."""
    st = table.get(raw)
    if st is None:
        st = fallback
        if raw and raw not in _seen_unknown:
            _seen_unknown.add(raw)
            log("etat client non cartographie : %r -> traite comme 'inconnu'"
                % raw)
    return st


def blank_torrent():
    return {"hash": "", "name": "", "state": "queued", "raw_state": "",
            "progress": 0.0,
            "size": 0, "downloaded": 0, "uploaded": 0, "dlspeed": 0,
            "upspeed": 0, "eta": 0, "ratio": 0.0, "category": "",
            "save_path": "", "added_on": 0, "seeds": 0, "peers": 0,
            "tracker": "", "msg": "", "emby": None, "emby_state": None,
            "local": None, "local_state": None}


class SeedboxClient:
    """Interface commune. Les implementations levent Exception en cas d'echec ;
    l'appelant affiche le message tel quel (jamais de plantage silencieux)."""

    name = "?"

    def __init__(self, base, user, password, verify=True):
        self.base = base.rstrip("/")
        self.user = user
        self.password = password
        self.verify = verify
        self.http = Http(self.base, user, password, verify)
        self.version = ""

    def connect(self):
        raise NotImplementedError

    def list_torrents(self):
        raise NotImplementedError

    def add_file(self, data, fname, category="", save_path="", paused=False):
        raise NotImplementedError

    def add_magnet(self, uri, category="", save_path="", paused=False):
        raise NotImplementedError

    def pause(self, hashes):
        raise NotImplementedError

    def resume(self, hashes):
        raise NotImplementedError

    def recheck(self, hashes):
        raise NotImplementedError

    def delete(self, hashes, with_data=False):
        raise NotImplementedError

    def disk_info(self):
        """-> (libre, total). total = 0 quand le client ne le publie pas."""
        return 0, 0

    def export_torrent(self, h):
        """Recupere le .torrent depuis le client. None si non supporte."""
        return None

    def can_delete_data(self):
        """Le client sait-il effacer les fichiers telecharges ?"""
        return True


# ---------------------------------------------------------------------
#  qBittorrent  -  Web API v2
# ---------------------------------------------------------------------
_QBT_STATE = {
    "downloading": "downloading", "forcedDL": "downloading",
    "metaDL": "downloading", "forcedMetaDL": "downloading",
    "allocating": "downloading", "downloadingMetadata": "downloading",
    "stalledDL": "stalled", "stalledUP": "seeding",
    "uploading": "seeding", "forcedUP": "seeding",
    "pausedDL": "paused", "pausedUP": "completed",
    "stoppedDL": "paused", "stoppedUP": "completed",
    "queuedDL": "queued", "queuedUP": "queued",
    "checkingDL": "checking", "checkingUP": "checking",
    "checkingResumeData": "checking", "moving": "checking",
    "error": "error", "missingFiles": "error", "unknown": "unknown",
}


class QbtClient(SeedboxClient):
    name = "qBittorrent"

    def connect(self):
        st, body = self.http.post_form("/api/v2/auth/login",
                                       {"username": self.user,
                                        "password": self.password})
        txt = body.decode("utf-8", "replace").strip()
        if txt.lower().startswith("fails"):
            raise Exception("identifiants refuses par qBittorrent")
        st, body = self.http.get("/api/v2/app/version")
        self.version = body.decode("utf-8", "replace").strip()
        return True

    def list_torrents(self):
        st, body = self.http.get("/api/v2/torrents/info")
        out = []
        for it in json.loads(body.decode("utf-8", "replace")):
            t = blank_torrent()
            t.update({
                "hash": (it.get("hash") or "").lower(),
                "name": it.get("name", ""),
                "state": map_state(it.get("state", ""), _QBT_STATE),
                "raw_state": it.get("state", ""),
                "progress": float(it.get("progress", 0) or 0),
                "size": int(it.get("size", 0) or 0),
                "downloaded": int(it.get("completed", 0) or 0),
                "uploaded": int(it.get("uploaded", 0) or 0),
                "dlspeed": int(it.get("dlspeed", 0) or 0),
                "upspeed": int(it.get("upspeed", 0) or 0),
                "eta": int(it.get("eta", 0) or 0),
                "ratio": float(it.get("ratio", 0) or 0),
                "category": it.get("category", "") or "",
                "save_path": it.get("save_path", "") or "",
                "added_on": int(it.get("added_on", 0) or 0),
                "seeds": int(it.get("num_seeds", 0) or 0),
                "peers": int(it.get("num_leechs", 0) or 0),
                "tracker": _host(it.get("tracker", "")),
            })
            if t["progress"] >= 1.0 and t["state"] == "stalled":
                t["state"] = "seeding"
            out.append(t)
        return out

    def add_file(self, data, fname, category="", save_path="", paused=False):
        fields = {"paused": "true" if paused else "false",
                  "stopped": "true" if paused else "false"}
        if category:
            fields["category"] = category
        if save_path:
            fields["savepath"] = save_path
        ctype, body = multipart_body(fields, [("torrents", fname, data)])
        st, resp = self.http.request("/api/v2/torrents/add", data=body,
                                     headers={"Content-Type": ctype,
                                              "Referer": self.base},
                                     method="POST")
        txt = resp.decode("utf-8", "replace").strip()
        if txt and txt.lower() != "ok.":
            raise Exception("qBittorrent a refuse le torrent : %s" % txt[:120])
        return True

    def add_magnet(self, uri, category="", save_path="", paused=False):
        f = {"urls": uri, "paused": "true" if paused else "false",
             "stopped": "true" if paused else "false"}
        if category:
            f["category"] = category
        if save_path:
            f["savepath"] = save_path
        self.http.post_form("/api/v2/torrents/add", f)
        return True

    def _act(self, path, hashes, extra=None):
        f = {"hashes": "|".join(hashes)}
        f.update(extra or {})
        self.http.post_form(path, f)

    def pause(self, hashes):
        # qBittorrent 5.x a renomme pause/resume en stop/start
        try:
            self._act("/api/v2/torrents/stop", hashes)
        except HttpError:
            self._act("/api/v2/torrents/pause", hashes)

    def resume(self, hashes):
        try:
            self._act("/api/v2/torrents/start", hashes)
        except HttpError:
            self._act("/api/v2/torrents/resume", hashes)

    def recheck(self, hashes):
        self._act("/api/v2/torrents/recheck", hashes)

    def delete(self, hashes, with_data=False):
        self._act("/api/v2/torrents/delete", hashes,
                  {"deleteFiles": "true" if with_data else "false"})

    def export_torrent(self, h):
        try:
            st, body = self.http.get("/api/v2/torrents/export?hash=%s" % h)
            return body if body[:1] == b"d" else None
        except Exception:
            return None

    def disk_info(self):
        try:
            st, body = self.http.get("/api/v2/sync/maindata?rid=0")
            d = json.loads(body.decode("utf-8", "replace"))
            ss = d.get("server_state", {})
            return (int(ss.get("free_space_on_disk", 0) or 0), 0)
        except Exception:
            return 0, 0


# ---------------------------------------------------------------------
#  rTorrent / ruTorrent  -  XML-RPC
# ---------------------------------------------------------------------
_RT_PATHS = ["/plugins/rpc/rpc.php", "/RPC2", "/rutorrent/plugins/rpc/rpc.php",
             "/xmlrpc"]


class RtorrentClient(SeedboxClient):
    name = "rTorrent"

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.rpc_url = ""
        self.proxy = None

    def _mk_proxy(self, url):
        ctx = None
        if not self.verify:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
        # xmlrpc.client gere l'auth basic via les identifiants dans l'URL
        if self.user:
            p = urllib.parse.urlsplit(url)
            netloc = "%s:%s@%s" % (urllib.parse.quote(self.user, safe=""),
                                   urllib.parse.quote(self.password, safe=""),
                                   p.netloc)
            url = urllib.parse.urlunsplit((p.scheme, netloc, p.path, p.query, ""))
        kw = {"allow_none": True}
        if url.startswith("https") and ctx is not None:
            kw["context"] = ctx
        return xmlrpc.client.ServerProxy(url, **kw)

    def connect(self):
        errs = []
        cands = ([self.base] if re.search(r"\.php$|/RPC2$|/xmlrpc$", self.base)
                 else [self.base + p for p in _RT_PATHS])
        for url in cands:
            try:
                pr = self._mk_proxy(url)
                self.version = str(pr.system.client_version())
                self.rpc_url = url
                self.proxy = pr
                return True
            except Exception as exc:
                errs.append("%s -> %s" % (url.split("@")[-1], str(exc)[:70]))
        raise Exception("aucun point XML-RPC n'a repondu :\n  " + "\n  ".join(errs))

    def list_torrents(self):
        rows = self.proxy.d.multicall2(
            "", "main", "d.hash=", "d.name=", "d.size_bytes=",
            "d.completed_bytes=", "d.down.rate=", "d.up.rate=", "d.up.total=",
            "d.ratio=", "d.is_active=", "d.is_open=", "d.complete=",
            "d.custom1=", "d.directory=", "d.message=", "d.creation_date=",
            "d.peers_connected=", "d.peers_complete=")
        out = []
        for r in rows:
            (h, name, size, done, dl, up, upt, ratio, active, opened,
             complete, label, directory, msg, created, peers, seeds) = r
            t = blank_torrent()
            size = int(size or 0)
            done = int(done or 0)
            t.update({
                "hash": str(h).lower(), "name": name,
                "size": size, "downloaded": done, "uploaded": int(upt or 0),
                "dlspeed": int(dl or 0), "upspeed": int(up or 0),
                "ratio": float(ratio or 0) / 1000.0,   # d.ratio est en pour-mille
                "progress": (done / size) if size else 0.0,
                "category": label or "", "save_path": directory or "",
                "added_on": int(created or 0), "seeds": int(seeds or 0),
                "peers": int(peers or 0), "msg": msg or "",
            })
            if msg and "Tracker" not in msg:
                t["state"] = "error"
            elif not int(opened or 0) or not int(active or 0):
                t["state"] = "paused" if not int(complete or 0) else "completed"
            elif int(complete or 0):
                t["state"] = "seeding"
            elif t["dlspeed"] > 0:
                t["state"] = "downloading"
            else:
                t["state"] = "stalled"
            t["raw_state"] = "open=%s active=%s complete=%s" % (
                int(opened or 0), int(active or 0), int(complete or 0))
            t["eta"] = int((size - done) / t["dlspeed"]) if t["dlspeed"] else 0
            out.append(t)
        return out

    def add_file(self, data, fname, category="", save_path="", paused=False):
        cmds = []
        if category:
            cmds.append("d.custom1.set=%s" % category)
        if save_path:
            cmds.append("d.directory.set=%s" % save_path)
        blob = xmlrpc.client.Binary(data)
        meth = ["load.raw", "load.raw_start"] if paused else \
               ["load.raw_start", "load.raw"]
        last = None
        for m in meth + ["load_raw_start", "load_raw"]:
            try:
                fn = self.proxy
                for part in m.split("."):
                    fn = getattr(fn, part)
                fn("", blob, *cmds)
                return True
            except Exception as exc:
                last = exc
        raise Exception("rTorrent a refuse le torrent : %s" % last)

    def add_magnet(self, uri, category="", save_path="", paused=False):
        cmds = []
        if category:
            cmds.append("d.custom1.set=%s" % category)
        if save_path:
            cmds.append("d.directory.set=%s" % save_path)
        fn = self.proxy.load.normal if paused else self.proxy.load.start
        fn("", uri, *cmds)
        return True

    def pause(self, hashes):
        for h in hashes:
            self.proxy.d.stop(h)
            self.proxy.d.close(h)

    def resume(self, hashes):
        for h in hashes:
            self.proxy.d.open(h)
            self.proxy.d.start(h)

    def recheck(self, hashes):
        for h in hashes:
            self.proxy.d.check_hash(h)

    def _erasedata(self, h):
        """rTorrent n'a aucune commande native pour effacer les donnees :
        on passe par le greffon erasedata de ruTorrent."""
        for root in ([self.base] + ([self.rpc_url.split("/plugins/")[0]]
                                    if self.rpc_url else [])):
            try:
                self.http.post_form(root.rstrip("/") +
                                    "/plugins/erasedata/action.php",
                                    {"hash": h.upper()}, use_basic=True)
                return True
            except Exception:
                continue
        return False

    def can_delete_data(self):
        return self._probe_erasedata()

    # Codes signifiant que le point d'entree n'existe pas a cette adresse.
    _ABSENT = (404, 405, 501)

    def _probe_erasedata(self):
        """Le greffon erasedata est-il installe ? On sonde par un POST avec un
        hash inexistant : sans effet de bord, et c'est exactement la requete
        que fera la vraie suppression."""
        if "erasedata" in G.get("_caps", {}):
            return G["_caps"]["erasedata"]
        ok = False
        for root in ([self.base] + ([self.rpc_url.split("/plugins/")[0]]
                                    if self.rpc_url else [])):
            try:
                self.http.post_form(root.rstrip("/") +
                                    "/plugins/erasedata/action.php",
                                    {"hash": "0" * 40}, use_basic=True)
                ok = True
                break
            except HttpError as e:
                if e.status and e.status not in self._ABSENT:
                    ok = True     # 401/403/500 : le greffon repond, il existe
                    break
            except Exception:
                continue
        G.setdefault("_caps", {})["erasedata"] = ok
        return ok

    def delete(self, hashes, with_data=False):
        for h in hashes:
            if with_data:
                self._erasedata(h)
            self.proxy.d.stop(h)
            self.proxy.d.erase(h)

    def disk_info(self):
        # rTorrent n'expose rien : on interroge le greffon diskspace de
        # ruTorrent, qui lui donne le total en plus du libre.
        roots = [self.base]
        if self.rpc_url:
            roots.append(self.rpc_url.split("/plugins/")[0].split("@")[-1])
        for root in roots:
            if not root.startswith("http"):
                root = "https://" + root
            try:
                st, body = self.http.get(root.rstrip("/") +
                                         "/plugins/diskspace/action.php",
                                         use_basic=True)
                d = json.loads(body.decode("utf-8", "replace"))
                return int(d.get("free", 0) or 0), int(d.get("total", 0) or 0)
            except Exception:
                continue
        return 0, 0


# ---------------------------------------------------------------------
#  Deluge  -  JSON-RPC (deluge-web)
# ---------------------------------------------------------------------
_DEL_FIELDS = ["name", "progress", "state", "total_size", "total_wanted",
               "total_done",
               "total_uploaded", "download_payload_rate",
               "upload_payload_rate", "ratio", "eta", "save_path",
               "time_added", "num_seeds", "num_peers", "tracker_host",
               "label", "message"]
_DEL_STATE = {"Downloading": "downloading", "Seeding": "seeding",
              "Paused": "paused", "Checking": "checking",
              "Queued": "queued", "Error": "error", "Allocating": "checking",
              "Moving": "checking"}


class DelugeClient(SeedboxClient):
    name = "Deluge"

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self._id = 0

    def _call(self, method, params):
        self._id += 1
        st, body = self.http.post_json("/json", {"method": method,
                                                 "params": params,
                                                 "id": self._id})
        d = json.loads(body.decode("utf-8", "replace"))
        if d.get("error"):
            raise Exception("Deluge : %s" % (d["error"].get("message")
                                             or d["error"]))
        return d.get("result")

    def connect(self):
        if not self._call("auth.login", [self.password]):
            raise Exception("mot de passe refuse par Deluge")
        if not self._call("web.connected", []):
            hosts = self._call("web.get_hosts", []) or []
            if not hosts:
                raise Exception("deluge-web n'est relie a aucun demon")
            self._call("web.connect_to_daemon", [hosts[0][0]])
        try:
            self.version = str(self._call("daemon.info", []) or "")
        except Exception:
            self.version = ""
        return True

    def list_torrents(self):
        res = self._call("core.get_torrents_status", [{}, _DEL_FIELDS]) or {}
        out = []
        for h, it in res.items():
            t = blank_torrent()
            # total_wanted exclut les fichiers deselectionnes : c'est lui qui
            # determine l'espace reellement consomme.
            size = int(it.get("total_wanted", 0) or it.get("total_size", 0) or 0)
            t.update({
                "hash": str(h).lower(), "name": it.get("name", ""),
                "state": map_state(it.get("state", ""), _DEL_STATE),
                "raw_state": it.get("state", ""),
                "progress": float(it.get("progress", 0) or 0) / 100.0,
                "size": size, "downloaded": int(it.get("total_done", 0) or 0),
                "uploaded": int(it.get("total_uploaded", 0) or 0),
                "dlspeed": int(it.get("download_payload_rate", 0) or 0),
                "upspeed": int(it.get("upload_payload_rate", 0) or 0),
                "eta": int(it.get("eta", 0) or 0),
                "ratio": max(0.0, float(it.get("ratio", 0) or 0)),
                "category": it.get("label", "") or "",
                "save_path": it.get("save_path", "") or "",
                "added_on": int(it.get("time_added", 0) or 0),
                "seeds": int(it.get("num_seeds", 0) or 0),
                "peers": int(it.get("num_peers", 0) or 0),
                "tracker": _host(it.get("tracker_host", "")),
                "msg": it.get("message", "") or "",
            })
            if t["state"] == "downloading" and not t["dlspeed"]:
                t["state"] = "stalled"
            out.append(t)
        return out

    def add_file(self, data, fname, category="", save_path="", paused=False):
        opts = {"add_paused": bool(paused)}
        if save_path:
            opts["download_location"] = save_path
        h = self._call("core.add_torrent_file",
                       [fname, base64.b64encode(data).decode("ascii"), opts])
        if category and h:
            try:
                self._call("label.set_torrent", [h, category])
            except Exception:
                pass          # greffon label absent : non bloquant
        return True

    def add_magnet(self, uri, category="", save_path="", paused=False):
        opts = {"add_paused": bool(paused)}
        if save_path:
            opts["download_location"] = save_path
        h = self._call("core.add_torrent_magnet", [uri, opts])
        if category and h:
            try:
                self._call("label.set_torrent", [h, category])
            except Exception:
                pass
        return True

    def pause(self, hashes):
        self._call("core.pause_torrent", [list(hashes)])

    def resume(self, hashes):
        self._call("core.resume_torrent", [list(hashes)])

    def recheck(self, hashes):
        self._call("core.force_recheck", [list(hashes)])

    def delete(self, hashes, with_data=False):
        for h in hashes:
            self._call("core.remove_torrent", [h, bool(with_data)])

    def disk_info(self):
        try:
            return int(self._call("core.get_free_space", []) or 0), 0
        except Exception:
            return 0, 0


def _host(url):
    if not url:
        return ""
    if "://" not in url:
        return url.split(":")[0]
    try:
        return urllib.parse.urlparse(url).netloc.split(":")[0]
    except Exception:
        return url[:40]


CLIENTS = {"qbittorrent": QbtClient, "rtorrent": RtorrentClient,
           "deluge": DelugeClient}


def make_client(kind, base, user, pwd, verify=True):
    """kind = 'auto' | 'qbittorrent' | 'rtorrent' | 'deluge'."""
    if kind != "auto":
        c = CLIENTS[kind](base, user, pwd, verify)
        c.connect()
        return c
    errs = []
    for key in ("qbittorrent", "deluge", "rtorrent"):
        try:
            c = CLIENTS[key](base, user, pwd, verify)
            c.connect()
            log("auto-detection : %s (%s)" % (c.name, c.version))
            return c
        except Exception as exc:
            errs.append("%-12s : %s" % (CLIENTS[key].name, str(exc)[:150]))
    raise Exception("Aucun client reconnu a cette adresse.\n\n" + "\n".join(errs))


# =====================================================================
#  EMBY  -  identifiants partages avec Emby Toolbox / Torrent Checker
# =====================================================================
EMBY_CREDS_FILE = APP_DIR / "emby_toolbox_creds.ini"


def load_emby_creds():
    d = {"url": "", "api_key": "", "user_id": ""}
    if EMBY_CREDS_FILE.exists():
        cfg = configparser.ConfigParser()
        try:
            cfg.read(EMBY_CREDS_FILE, encoding="utf-8")
            if cfg.has_section("emby"):
                for k in d:
                    d[k] = cfg["emby"].get(k, "") or ""
        except Exception:
            pass
        d["api_key"] = decrypt_secret(d["api_key"])
    return d


def save_emby_creds(url, key, uid):
    raw = {}
    if EMBY_CREDS_FILE.exists():
        cfg = configparser.ConfigParser()
        try:
            cfg.read(EMBY_CREDS_FILE, encoding="utf-8")
            if cfg.has_section("emby"):
                raw = dict(cfg["emby"])
        except Exception:
            raw = {}
    raw.update({"url": url, "api_key": encrypt_secret(key), "user_id": uid})
    cfg = configparser.ConfigParser()
    cfg["emby"] = raw
    try:
        with open(EMBY_CREDS_FILE, "w", encoding="utf-8") as f:
            cfg.write(f)
    except Exception as exc:
        log("save_emby_creds: %s" % exc)


_NET_HINTS = {
    "10013": ("Windows a refuse la socket (WSAEACCES). La requete n'est jamais "
              "sortie de la machine : ce n'est pas un probleme de serveur.\n"
              "Causes habituelles, dans l'ordre :\n"
              "  - un antivirus ou pare-feu bloque pythonw.exe (les regles "
              "visent souvent python.exe, pas pythonw.exe) ;\n"
              "  - le port est reserve par Hyper-V / WSL / Docker : verifie "
              "avec\n      netsh int ipv4 show excludedportrange protocol=tcp\n"
              "  - un VPN ou un filtrage reseau intercepte la connexion."),
    "10061": ("Connexion refusee : rien n'ecoute sur ce port. Verifie l'adresse "
              "et le port, et que le service tourne."),
    "10060": ("Delai depasse : la machine n'est pas joignable (routage, "
              "pare-feu distant, ou mauvaise adresse)."),
    "11001": ("Nom d'hote introuvable : erreur de DNS ou faute de frappe dans "
              "l'adresse."),
    "10054": "La connexion a ete coupee par le serveur distant.",
}


def friendly_net_error(exc, url):
    """Traduit une erreur reseau en message exploitable, URL comprise."""
    raw = str(exc)
    out = ["Adresse tentee : %s" % url, "", raw]
    m = re.search(r"WinError (\d+)", raw)
    if m and m.group(1) in _NET_HINTS:
        out += ["", _NET_HINTS[m.group(1)]]
    elif "CERTIFICATE_VERIFY_FAILED" in raw or "certificate" in raw.lower():
        out += ["", "Certificat TLS refuse. Decoche 'Verifier le certificat "
                    "SSL' si le serveur utilise un certificat auto-signe."]
    elif "401" in raw or "403" in raw:
        out += ["", "Le serveur a repondu mais a refuse l'authentification : "
                    "verifie la cle API ou les identifiants."]
    elif "unknown url type" in raw or "No scheme" in raw:
        out += ["", "L'adresse doit commencer par http:// ou https://."]
    return "\n".join(out)


def emby_get(base, key, path, params=None):
    p = dict(params or {})
    p["api_key"] = key
    url = "%s%s?%s" % (base.rstrip("/"), path, urllib.parse.urlencode(p))
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


# ---------------------------------------------------------------------
#  Normalisation des titres (identique a Torrent Checker 2)
# ---------------------------------------------------------------------
_TAGS = (r"2160p|1080[pi]|720p|576p|480p|4k|uhd|fhd|hdlight|hdlite|"
         r"blu ?ray|bluray|bdrip|brrip|bd ?remux|remux|bdmv|webrip|web ?dl|web|"
         r"hdtv|dvdrip|dvdscr|dvd ?[95]|dvd|hd ?dvd|vhsrip|tvrip|telesync|"
         r"x ?26[45]|h ?26[45]|hevc|avc|av1|xvid|divx|vp9|mpeg ?2|"
         r"aac|ac3|e ?ac3|eac3|dts ?hd ?ma|dts ?hd|dts|true ?hd|atmos|flac|opus|"
         r"ddp? ?5 1|dd ?5 1|ddp|dd|5 1|7 1|2 0|"
         r"10 ?bits?|8 ?bits?|hdr10|hdr|dovi|dolby ?vision|dolby|sdr|imax|"
         r"multi ?3|multi|vff|vfq|vfi|vf2|vf|vostfr|vost|subfrench|subforced|"
         r"french|truefrench|english|"
         r"repack|proper|rerip|internal|limited|extended|unrated|uncut|"
         r"remastered|remasterise|integrale|complete|final cut|"
         r"directors? cut|director s cut|theatrical|redux|special edition|"
         r"anniversary|criterion|amzn|netflix|dsnp|hulu|hmax|atvp|pcok|"
         r"3d|sbs|hsbs|mvc|rarbg|yify|yts|fgt|sparks|nogrp")
_TAG_RE = re.compile(r"\b(?:%s)\b" % _TAGS, re.I)
_YEAR_ANY = re.compile(r"(?<!\d)((?:19|20)\d{2})(?!\d)")
_RES_RE = re.compile(r"\b(2160p|4k|uhd|1080p|1080i|720p|576p|480p|dvdrip|"
                     r"bdrip|hdtv|cam)\b", re.I)
_ARTICLES = ("le ", "la ", "les ", "l ", "un ", "une ", "des ", "du ",
             "the ", "a ", "an ")
_MAX_YEAR = time.localtime().tm_year + 2
TIER_LBL = {4: "4K/UHD", 3: "1080p", 2: "720p", 1: "SD", 0: "?"}
VIDEO_EXT = {".mkv", ".mp4", ".avi", ".m2ts", ".ts", ".mov", ".wmv", ".mpg",
             ".mpeg", ".m4v", ".iso", ".img", ".vob", ".ogm", ".rmvb"}


def strip_accents(x):
    return "".join(c for c in unicodedata.normalize("NFKD", x)
                   if not unicodedata.combining(c))


def clean_tokens(x):
    x = strip_accents(str(x or "")).lower().replace("&", " and ")
    x = re.sub(r"[\._\-\[\]\(\)\{\}:,;!?'\"`~+/\\|#@*]", " ", x)
    return re.sub(r"\s+", " ", x).strip()


def split_title_year(raw):
    x = clean_tokens(raw)
    year, cut = None, None
    for m in _YEAR_ANY.finditer(x):
        y = int(m.group(1))
        if 1888 <= y <= _MAX_YEAR:
            year, cut = y, m.start()
    title = (x[:cut] if cut is not None else x).strip()
    stripped = re.sub(r"\s+", " ", _TAG_RE.sub(" ", title)).strip()
    if stripped:
        title = stripped
    if not title and cut is not None:
        rest = re.sub(r"\s+", " ", _TAG_RE.sub(" ", x[cut + 4:])).strip()
        title = rest or x[cut + 4:].strip()
    for art in _ARTICLES:
        if title.startswith(art):
            title = title[len(art):]
            break
    return title.strip(), year


def norm_key(x):
    return re.sub(r"[^a-z0-9]", "", strip_accents(str(x or "")).lower())


def tier_from_name(name):
    m = _RES_RE.search(name or "")
    if not m:
        return 0
    v = m.group(1).lower()
    return (4 if v in ("2160p", "4k", "uhd") else
            3 if v in ("1080p", "1080i") else 2 if v == "720p" else 1)


def fuzzy(a, b):
    if not a or not b:
        return 0
    return int(SequenceMatcher(None, " ".join(sorted(a.split())),
                               " ".join(sorted(b.split()))).ratio() * 100)


_STOP = {"the", "a", "an", "le", "la", "les", "de", "du", "des", "et", "and",
         "of", "in", "on", "el", "il", "un", "une", "part", "vol"}


def _toks(n):
    return [t for t in n.split() if len(t) >= 3 and t not in _STOP]


# ---------------------------------------------------------------------
#  Caracteristiques visibles d'une release (langue, source, codec...)
# ---------------------------------------------------------------------
# Chaque regle : (valeur affichee, motif cherche dans le nom nettoye par
# clean_tokens, donc "WEB-DL" y devient "web dl"). La premiere qui
# correspond gagne : l'ordre compte (REMUX avant BLURAY, WEBRIP avant WEB).
_SRC_RULES = (
    ("REMUX", r"(?:bd|uhd|blu ?ray) ?remux|remux|bdmv|complete blu ?ray|"
              r"full blu ?ray"),
    ("HDLIGHT", r"hdlight|hdlite|mhd"),
    ("BDRIP", r"bdrip|brrip|bd ?rip"),
    ("BLURAY", r"blu ?ray|bd|uhd ?bd"),
    ("WEBRIP", r"webrip|web ?rip"),
    ("WEB-DL", r"web ?dl|webdl|web|amzn|nf|netflix|dsnp|hmax|atvp|pcok"),
    ("HDTV", r"hdtv|tvrip|dsr|pdtv"),
    ("DVDRIP", r"dvdrip|dvd ?rip"),
    ("DVD", r"dvd ?[59]|dvd|dvdr"),
    ("CAM/TS", r"cam|hdcam|ts|telesync|hdts|tc|telecine|dvdscr|screener"),
)
# Rang de qualite pour dire qui a "mieux" (plus grand = meilleur).
SRC_RANK = {"REMUX": 8, "BLURAY": 7, "WEB-DL": 6, "BDRIP": 5, "HDLIGHT": 5,
            "WEBRIP": 4, "HDTV": 3, "DVD": 3, "DVDRIP": 2, "CAM/TS": 0}
_CODEC_RULES = (("AV1", r"av1"), ("x265/HEVC", r"x ?265|h ?265|hevc"),
                ("x264/AVC", r"x ?264|h ?264|avc"), ("XviD", r"xvid|divx"),
                ("MPEG-2", r"mpeg ?2"), ("VC-1", r"vc ?1"))
_HDR_RULES = (("DV", r"dolby ?vision|dovi|dv"), ("HDR10+", r"hdr10 ?plus|hdr10p"),
              ("HDR", r"hdr10|hdr"))
_AUDIO_RULES = (("Atmos", r"atmos"), ("TrueHD", r"true ?hd"),
                ("DTS-HD MA", r"dts ?hd ?ma|dts ?ma"), ("DTS-HD", r"dts ?hd|dts ?x"),
                ("DTS", r"dts"), ("E-AC3", r"e ?ac3|eac3|ddp|dd ?plus"),
                ("AC3", r"ac3|dd ?5 1|dd"), ("FLAC", r"flac"), ("AAC", r"aac"),
                ("Opus", r"opus"), ("MP3", r"mp3"))
AUDIO_RANK = {"Atmos": 9, "TrueHD": 8, "DTS-HD MA": 8, "DTS-HD": 7, "FLAC": 7,
              "DTS": 6, "E-AC3": 5, "AC3": 4, "Opus": 3, "AAC": 3, "MP3": 1}
_EDITION_RULES = (("Extended", r"extended|version longue"),
                  ("Director's Cut", r"directors? cut|director s cut"),
                  ("Unrated", r"unrated|uncut"), ("Remastered", r"remaster(?:ed|ise)?"),
                  ("IMAX", r"imax"), ("Final Cut", r"final cut"),
                  ("Theatrical", r"theatrical"), ("Criterion", r"criterion"),
                  ("3D", r"3d|sbs|hsbs|mvc"))
_RULE_CACHE = {}


def _rx(pat):
    r = _RULE_CACHE.get(pat)
    if r is None:
        r = _RULE_CACHE[pat] = re.compile(r"(?<![a-z0-9])(?:%s)(?![a-z0-9])"
                                          % pat)
    return r


def _first(rules, x):
    for lbl, pat in rules:
        if _rx(pat).search(x):
            return lbl
    return ""


def _lang_from_text(x):
    """Langue annoncee dans un nom deja nettoye. "" si rien n'est dit."""
    has = lambda pat: bool(_rx(pat).search(x))
    vff = has(r"vff|truefrench|true french|vfi")
    vfq = has(r"vfq|quebec|qc|french canadian|canadien")
    if has(r"vf2"):
        variant = "VF2"
    elif vff and vfq:
        variant = "VF2"
    elif vfq:
        variant = "VFQ"
    elif vff:
        variant = "VFI" if has(r"vfi") and not has(r"vff|truefrench") else "VFF"
    else:
        variant = ""
    if has(r"multi ?\d?|multilang|dual"):
        return "MULTI " + variant if variant else "MULTI"
    if variant:
        return variant
    if has(r"vostfr|subfrench|vost|stfr"):
        return "VOSTFR"
    if has(r"french|vf|fr"):
        return "FRENCH"
    if has(r"vo|english|eng"):
        return "VO"
    return ""


def _release_group(raw):
    """Equipe de release : le "-XXX" qui termine le nom (avant l'extension)."""
    stem = str(raw or "").strip()
    ext = Path(stem).suffix.lower()
    if ext in VIDEO_EXT or ext == ".torrent":
        stem = stem[:-len(ext)]
    m = re.search(r"-([A-Za-z0-9]{2,15})\s*(?:\[[^\]]*\])?\s*$", stem)
    if not m or re.fullmatch(r"(?:19|20)\d{2}|\d+p|x26[45]|dl", m.group(1), re.I):
        return ""
    return m.group(1)


def _tags_part(raw):
    """Partie "technique" d'un nom : ce qui suit l'annee, ou a defaut ce qui
    suit le premier marqueur connu (1080p, MULTI...). Le titre lui-meme n'est
    jamais analyse : "French Connection" ou "Cam" ne sont pas des tags."""
    x = clean_tokens(raw)
    cut = None
    for m in _YEAR_ANY.finditer(x):
        if 1888 <= int(m.group(1)) <= _MAX_YEAR:
            cut = m.end()
    if cut is None:
        m = _TAG_RE.search(x)
        cut = m.start() if m else len(x)
    return x[cut:].strip()


def release_attrs(raw):
    """Caracteristiques lisibles dans un nom de release ou de fichier.

    Toutes les clefs sont presentes ; "" = non precise dans le nom.
    """
    x = _tags_part(raw)
    tier = tier_from_name(x)
    ed = [lbl for lbl, pat in _EDITION_RULES if _rx(pat).search(x)]
    return {"lang": _lang_from_text(x), "source": _first(_SRC_RULES, x),
            "res": TIER_LBL[tier] if tier else "", "tier": tier,
            "codec": _first(_CODEC_RULES, x), "hdr": _first(_HDR_RULES, x),
            "audio": _first(_AUDIO_RULES, x), "edition": ", ".join(ed),
            "group": _release_group(raw)}


_VCODEC = {"hevc": "x265/HEVC", "h265": "x265/HEVC", "h264": "x264/AVC",
           "avc": "x264/AVC", "av1": "AV1", "mpeg4": "XviD", "msmpeg4v3": "XviD",
           "mpeg2video": "MPEG-2", "vc1": "VC-1"}
_ACODEC = {"truehd": "TrueHD", "dts": "DTS", "eac3": "E-AC3", "ac3": "AC3",
           "aac": "AAC", "flac": "FLAC", "opus": "Opus", "mp3": "MP3"}
_FR = {"fre", "fra", "fr", "french"}


def emby_source_attrs(path, streams, tier):
    """Caracteristiques d'une version Emby : le nom du fichier (et de son
    dossier) d'abord, completes et corriges par les pistes reellement lues
    par Emby (resolution, codec, HDR, audio, langues)."""
    p = Path(path) if path else None
    a = release_attrs("%s %s" % (p.parent.name, p.name) if p else "")
    if tier:
        a["tier"], a["res"] = tier, TIER_LBL[tier]
    vid = next((s for s in streams if s.get("Type") == "Video"), None)
    if vid:
        a["codec"] = _VCODEC.get(str(vid.get("Codec", "")).lower(), a["codec"])
        rng = "%s %s" % (vid.get("VideoRange", ""), vid.get("VideoRangeType", ""))
        rng = rng.lower()
        if "dovi" in rng or "dolby" in rng:
            a["hdr"] = "DV"
        elif "hdr10+" in rng or "hdr10plus" in rng:
            a["hdr"] = "HDR10+"
        elif "hdr" in rng or "hlg" in rng:
            a["hdr"] = "HDR"
        elif "sdr" in rng:
            a["hdr"] = ""
    auds = [s for s in streams if s.get("Type") == "Audio"]
    if auds:
        best, rank = "", -1
        for s in auds:
            c = _ACODEC.get(str(s.get("Codec", "")).lower(), "")
            t = clean_tokens("%s %s" % (s.get("DisplayTitle", ""), s.get("Title", "")))
            if "atmos" in t:
                c = "Atmos"
            elif c == "DTS" and ("ma" in t.split() or "dts hd" in t):
                c = "DTS-HD MA"
            if AUDIO_RANK.get(c, 0) > rank:
                best, rank = c, AUDIO_RANK.get(c, 0)
        a["audio"] = best or a["audio"]
        # Langues des pistes : n'ecrase pas ce que le nom precise (VFQ...),
        # mais complete un nom muet, ou un "MULTI" sans variante.
        langs = [str(s.get("Language", "")).lower() for s in auds]
        titles = " ".join(clean_tokens("%s %s" % (s.get("DisplayTitle", ""),
                                                   s.get("Title", "")))
                          for s in auds)
        fr = sum(1 for l in langs if l in _FR)
        other = sum(1 for l in langs if l and l not in _FR)
        variant = _lang_from_text(titles)
        variant = variant.replace("MULTI", "").strip() \
            if variant not in ("FRENCH", "VO", "VOSTFR") else ""
        if not a["lang"]:
            if fr and other:
                a["lang"] = ("MULTI " + variant).strip()
            elif fr:
                a["lang"] = variant or "FRENCH"
            elif other:
                subs = [s for s in streams if s.get("Type") == "Subtitle" and
                        str(s.get("Language", "")).lower() in _FR]
                a["lang"] = "VOSTFR" if subs else "VO"
        elif a["lang"] in ("MULTI", "FRENCH") and variant:
            a["lang"] = ("MULTI " + variant) if a["lang"] == "MULTI" else variant
    return a


ATTR_LABELS = (("lang", "Langue"), ("source", "Source"), ("res", "Resolution"),
               ("codec", "Codec"), ("hdr", "HDR"), ("audio", "Audio"),
               ("edition", "Edition"), ("group", "Equipe"))


def _rank(key, a):
    if key == "res":
        return a.get("tier") or None
    if key == "source":
        return SRC_RANK.get(a.get("source"))
    if key == "audio":
        return AUDIO_RANK.get(a.get("audio"))
    if key == "hdr":
        return {"": 0, "HDR": 1, "HDR10+": 2, "DV": 3}.get(a.get("hdr"))
    return None


def attrs_diff(page, have):
    """Differences visibles entre la release vue dans le navigateur et une
    version deja possedee. -> liste de {key, label, page, have, cmp}
    cmp : "mieux" (la page a mieux), "moins" (la page a moins bien), "autre".

    Une caracteristique que le nom de la page ne precise pas n'est pas une
    difference (sauf le HDR : le posseder est un vrai plus a signaler).
    """
    out = []
    for key, lbl in ATTR_LABELS:
        pv, hv = page.get(key, ""), have.get(key, "")
        if pv == hv or (not pv and key != "hdr") or (not hv and key == "group"):
            continue
        if key == "hdr" and not page.get("res"):
            continue            # simple titre de film : rien a comparer
        if key == "lang" and pv and hv and (pv in hv or hv in pv) \
                and "FRENCH" in (pv, hv):
            continue            # "FRENCH" generique contre "VFF" : pas d'info
        rp, rh = _rank(key, page), _rank(key, have)
        cmp = "autre"
        if rp is not None and rh is not None and rp != rh:
            cmp = "mieux" if rp > rh else "moins"
        none = "aucun" if key == "hdr" else "-"
        out.append({"key": key, "label": lbl, "page": pv or none,
                    "have": hv or none, "cmp": cmp})
    return out


# =====================================================================
#  ETAT GLOBAL
# =====================================================================
CFG = load_cfg()
CRD = load_creds()
EMB = load_emby_creds()

G = {
    "client": None,
    "torrents": [],
    "order": [],            # hashes dans l'ordre affiche (detecte les rebuilds)
    "rows": {},             # hash -> {tag_col: tag} pour maj sur place
    "sel": set(),
    "queue": [],            # fichiers en attente d'envoi
    "sent": {},             # chemin -> horodatage (anti-doublon du dossier surveille)
    "deferred": set(),      # reportes faute d'espace (re-essayes au cycle suivant)
    "emby_skip": set(),     # refuses par l'utilisateur : deja sur Emby
    "emby_asked": set(),    # deja soumis a confirmation (pas de harcelement)
    "dup_queue": [],        # doublons en attente de decision, un par un
    "dup_open": False,      # une fenetre de decision est-elle affichee ?
    "local_idx": None,      # index des dossiers locaux
    "local_dirs": list(CFG.get("local_dirs") or []),
    "colmap": {},           # id de colonne -> clef de tri
    "free": 0, "total": 0,
    "last_err": "",
    "connected": False,
}
_stop_refresh = threading.Event()
_stop_watch = threading.Event()
_busy = threading.Lock()
_mid = 0

STATE_FILTERS = ("Tous", "Telechargement", "Seed", "En pause", "Erreur",
                 "Termine")


def fmt_size(b):
    if not b:
        return "-"
    b = float(b)
    for u in ("o", "Ko", "Mo", "Go", "To"):
        if b < 1024:
            return "%.1f %s" % (b, u)
        b /= 1024
    return "%.1f Po" % b


def fmt_speed(b):
    return "-" if not b else fmt_size(b) + "/s"


def fmt_eta(s):
    s = int(s or 0)
    if s <= 0 or s >= 8640000:      # 8640000 = infini chez qBittorrent
        return "-"
    d, r = divmod(s, 86400)
    h, r = divmod(r, 3600)
    m, sec = divmod(r, 60)
    if d:
        return "%dj %02dh" % (d, h)
    if h:
        return "%dh%02d" % (h, m)
    return "%dm%02d" % (m, sec)


def gv(tag, default=""):
    try:
        return dpg.get_value(tag)
    except Exception:
        return default


def set_status(msg, color=(150, 200, 240)):
    if dpg.does_item_exist("sb_status"):
        dpg.set_value("sb_status", msg)
        dpg.configure_item("sb_status", color=color)


def add_log(msg, color=(200, 200, 215)):
    """Journal visible dans l'onglet Ajout (le plus recent en haut)."""
    log(msg)
    if not dpg.does_item_exist("sb_logarea"):
        return
    kids = dpg.get_item_children("sb_logarea", 1) or []
    # before=0 signifie "a la fin" ; passer None leve une erreur DearPyGui,
    # ce qui arrivait sur la toute premiere ligne du journal.
    dpg.add_text("[%s] %s" % (time.strftime("%H:%M:%S"), msg),
                 parent="sb_logarea", color=color, wrap=1100,
                 before=kids[0] if kids else 0)
    kids = dpg.get_item_children("sb_logarea", 1) or []
    for extra in kids[400:]:
        dpg.delete_item(extra)


_deferred = {}


def defer(key, fn, delay=0.30):
    """Reporte un traitement lourd apres la frame en cours.

    Indispensable pour les curseurs et les champs de saisie : DearPyGui
    rappelle leur callback A CHAQUE FRAME pendant la manipulation. Reconstruire
    le tableau depuis ce callback revient a detruire des elements qu'ImGui est
    en train de parcourir -> plantage natif, sans trace Python.
    Le travail est donc execute par la boucle principale, entre deux frames,
    et une seule fois quand la valeur s'est stabilisee.
    """
    _deferred[key] = (fn, time.time() + delay)


def run_deferred():
    if not _deferred:
        return
    now = time.time()
    for key in [k for k, (_, when) in _deferred.items() if now >= when]:
        fn, _ = _deferred.pop(key)
        try:
            fn()
        except Exception:
            traceback.print_exc()


def pb_set(frac, text=""):
    """Progression. Appelable depuis un thread : passe par la file UI."""
    ui(lambda: (dpg.configure_item("sb_pb", show=True, overlay=text[:80]),
                dpg.set_value("sb_pb", max(0.0, min(1.0, frac)))))


def pb_hide():
    ui(lambda: dpg.configure_item("sb_pb", show=False, overlay=""))


def modal(title, msg, wide=560):
    global _mid
    _mid += 1
    tag = "sb_modal_%d" % _mid
    with dpg.window(label=title, tag=tag, modal=True, width=wide,
                    autosize=True, pos=[180, 200], no_resize=True):
        dpg.add_text(msg, wrap=wide - 30)
        dpg.add_separator()
        dpg.add_button(label="OK", width=-1, user_data=tag,
                       callback=lambda s, a, u: dpg.delete_item(u))


def persist():
    CFG.update({
        "watch_dir": (gv("sb_watchdir", CFG["watch_dir"]) or "").strip(),
        "watch_enabled": bool(gv("sb_watch_on", CFG["watch_enabled"])),
        "watch_interval": int(gv("sb_watch_int", CFG["watch_interval"]) or 20),
        "after_send": gv("sb_after", CFG["after_send"]) or "archiver",
        "archive_dir": (gv("sb_archdir", CFG["archive_dir"]) or "").strip(),
        "category": (gv("sb_cat", CFG["category"]) or "").strip(),
        "save_path": (gv("sb_savepath", CFG["save_path"]) or "").strip(),
        "add_paused": bool(gv("sb_paused", CFG["add_paused"])),
        "refresh": int(gv("sb_refresh", CFG["refresh"]) or 5),
        "auto_refresh": bool(gv("sb_autoref", CFG["auto_refresh"])),
        "quota_go": int(gv("sb_quota", CFG["quota_go"]) or 0),
        "min_free_go": int(gv("sb_minfree", CFG["min_free_go"]) or 0),
        "check_space": bool(gv("sb_checkspace", CFG["check_space"])),
        "count_paused": bool(gv("sb_countpaused", CFG["count_paused"])),
        "emby_fuzzy": int(gv("sb_embyfuzzy", CFG["emby_fuzzy"]) or 80),
        "emby_dup": gv("sb_embydup", CFG["emby_dup"]) or "demander",
        "refus_dir": (gv("sb_refusdir", CFG["refus_dir"]) or "").strip(),
        "snap_enabled": bool(gv("sb_snapon", CFG["snap_enabled"])),
        "snap_dir": (gv("sb_snapdir", CFG["snap_dir"]) or "").strip(),
        "snap_interval": int(gv("sb_snapint", CFG["snap_interval"]) or 10),
        "snap_keep": int(gv("sb_snapkeep", CFG["snap_keep"]) or 0),
    })
    save_cfg(CFG)


def disk_summary():
    """Photographie de l'espace, engagements compris.

      'pending' = ce que les torrents qui ecrivent vraiment vont encore ecrire.
                  Seule valeur deduite de l'espace libre.
      'dormant' = ce qu'ecriraient les torrents en pause, en erreur, d'etat
                  inconnu ou bloques sans aucun pair. Hypothetique : affiche,
                  deduit seulement si l'utilisateur le demande.
    """
    lst = G["torrents"]
    quota = int(CFG.get("quota_go", 0) or 0) * 1024 ** 3
    total = G.get("total", 0) or quota
    free = G.get("free", 0)
    used_tor = sum(t["downloaded"] for t in lst)
    firm = [t for t in lst if is_firm(t)]
    dorm = [t for t in lst if is_dormant(t)]
    pending = sum(remaining(t) for t in firm)
    dormant = sum(remaining(t) for t in dorm)
    engaged = pending + (dormant if CFG.get("count_paused", False) else 0)
    projected = (free - engaged) if free else 0
    used = (total - free) if (total and free) else used_tor
    return {"total": total, "free": free, "used": used, "used_tor": used_tor,
            "pending": pending, "dormant": dormant, "n_dormant": len(dorm),
            "firm": firm, "dorm": dorm, "engaged": engaged,
            "projected": projected, "pct": (used / total) if total else 0.0,
            "min_free": int(CFG.get("min_free_go", 10) or 0) * 1024 ** 3}


def space_detail_text(d, limit=12):
    """Qui reserve la place, nommement. Sans ca, un refus est incomprehensible."""
    out = []
    if d["firm"]:
        out.append("RESERVENT DE LA PLACE (%d torrents, %s)"
                   % (len(d["firm"]), fmt_size(d["pending"])))
        for t in sorted(d["firm"], key=lambda x: -remaining(x))[:limit]:
            out.append("   %9s  %-52s [%s]"
                       % (fmt_size(remaining(t)), t["name"][:52],
                          ST_LBL.get(t["state"], t["state"])))
        if len(d["firm"]) > limit:
            out.append("   ... et %d autre(s)" % (len(d["firm"]) - limit))
    else:
        out.append("Aucun torrent ne reserve de place.")
    if d["dorm"]:
        out.append("")
        out.append("NE RESERVENT RIEN (%d torrents, %s)%s"
                   % (len(d["dorm"]), fmt_size(d["dormant"]),
                      "  -  comptes car tu l'as demande"
                      if CFG.get("count_paused") else ""))
        for t in sorted(d["dorm"], key=lambda x: -remaining(x))[:limit]:
            why = ST_LBL.get(t["state"], t["state"])
            if t["state"] == "stalled":
                why = "bloque, aucun pair"
            out.append("   %9s  %-52s [%s]"
                       % (fmt_size(remaining(t)), t["name"][:52], why))
        if len(d["dorm"]) > limit:
            out.append("   ... et %d autre(s)" % (len(d["dorm"]) - limit))
    return "\n".join(out)


def show_space_detail(sender=None, app_data=None, user_data=None):
    global _mid
    d = disk_summary()
    _mid += 1
    tag = "sb_spc_%d" % _mid
    with dpg.window(label="Detail de l'espace reserve", tag=tag, modal=True,
                    width=860, height=560, pos=[140, 120]):
        dpg.add_text("Libre %s   -   reserve %s   -   libre a la fin %s   "
                     "(marge exigee %s)"
                     % (fmt_size(d["free"]), fmt_size(d["engaged"]),
                        fmt_size(d["projected"]) if d["projected"] > 0 else "0 o",
                        fmt_size(d["min_free"])), color=(150, 200, 240))
        dpg.add_separator()
        with dpg.child_window(height=-40, border=False, horizontal_scrollbar=True):
            for line in space_detail_text(d, limit=200).splitlines():
                col = ((235, 140, 20) if line.startswith("RESERVENT")
                       else (46, 204, 113) if line.startswith("NE RESERVENT")
                       else (200, 200, 215))
                dpg.add_text(line or " ", color=col)
        dpg.add_separator()
        dpg.add_button(label="Fermer", width=-1, user_data=tag,
                       callback=lambda s, a, u: dpg.delete_item(u))


def fits(size, already=0):
    """Le torrent tient-il, une fois les engagements en cours honores ?"""
    d = disk_summary()
    if not CFG.get("check_space", True) or not d["free"]:
        return True, d
    return (d["projected"] - already - size) >= d["min_free"], d


# =====================================================================
#  CONNEXION
# =====================================================================
def do_connect(sender=None, app_data=None, user_data=None):
    url = (gv("sb_url") or "").strip().rstrip("/")
    user = (gv("sb_user") or "").strip()
    pwd = gv("sb_pass") or ""
    kind = (gv("sb_kind") or "auto").lower()
    verify = bool(gv("sb_verify", True))
    if not url:
        modal("Adresse manquante",
              "Indique l'URL de ton client, par exemple :\n\n"
              "  https://xxx.seedbox.io          (qBittorrent / Deluge)\n"
              "  https://xxx.seedbox.io/rutorrent (ruTorrent)")
        return
    if not url.startswith("http"):
        url = "https://" + url
        dpg.set_value("sb_url", url)
    save_creds(url=url, user=user, password=pwd, client=kind,
               verify_ssl="1" if verify else "0")
    set_status("Connexion a %s..." % url, (235, 140, 20))
    dpg.configure_item("sb_btn_connect", enabled=False)

    def worker():
        try:
            c = make_client(kind, url, user, pwd, verify)

            def done():
                G["client"] = c
                G["connected"] = True
                set_status("Connecte  -  %s %s" % (c.name, c.version),
                           (46, 204, 113))
                dpg.set_value("sb_clientlbl", "%s %s" % (c.name, c.version))
                dpg.configure_item("sb_btn_connect", enabled=True)
                add_log("Connecte a %s (%s) sur %s" % (c.name, c.version, url),
                        (46, 204, 113))
                refresh_now()
                start_refresh_thread()
                if CFG["watch_enabled"]:
                    start_watch_thread()
            ui(done)
        except Exception as exc:
            msg = friendly_net_error(exc, url) if "WinError" in str(exc) \
                else str(exc)
            log("connexion: %s" % msg.replace("\n", " | "))

            def fail():
                G["connected"] = False
                set_status("Echec de connexion.", (215, 75, 90))
                dpg.configure_item("sb_btn_connect", enabled=True)
                modal("Connexion impossible", msg, wide=760)
            ui(fail)

    threading.Thread(target=worker, daemon=True).start()


# =====================================================================
#  AJOUT DE TORRENTS
# =====================================================================
def _opts():
    return ((gv("sb_cat") or "").strip(), (gv("sb_savepath") or "").strip(),
            bool(gv("sb_paused", False)))


def send_files(paths, origin="manuel", force=False, force_emby=False):
    """Envoie une liste de .torrent. Toujours appele depuis un thread.

    Deux controles avant tout envoi :
      1. le film est-il deja sur Emby ? (inutile de retelecharger)
      2. tiendra-t-il sur le disque une fois les telechargements en cours
         termines ? 'already' cumule les tailles acceptees pendant ce lot.
    """
    c = G["client"]
    if not c:
        ui(lambda: modal("Non connecte", "Connecte-toi d'abord a la seedbox."))
        return 0, 0
    cat, sp, paused = _opts()
    idx = G.get("emby_idx")
    dup_mode = CFG.get("emby_dup", "demander")
    ok, ko, already = 0, 0, 0
    refuses, doublons = [], []
    for p in paths:
        try:
            with open(p, "rb") as fh:
                data = fh.read()
            if not data:
                raise Exception("fichier vide")
            name, size = torrent_info(data, Path(p).stem)
            st, ent = emby_verdict(name, size, idx)
            lst, lent = emby_verdict(name, size, G.get("local_idx"))
            src = "Emby"
            if st not in (EM_SAME, EM_OK) and lst in (EM_SAME, EM_OK):
                st, ent, src = lst, lent, "dossier local"

            # --- controle de presence (Emby + dossiers) ---
            if st in (EM_SAME, EM_OK) and not force_emby and dup_mode != "envoyer":
                if p in G["emby_skip"]:
                    continue
                if dup_mode == "ignorer":
                    _refuse(p, "deja present (%s)" % src)
                    ui(lambda n=os.path.basename(p), e=ent, sr=src: add_log(
                        "IGNORE, deja present (%s) : %s  ->  %s (%s, %s)"
                        % (sr, n, e.get("fname") or e["name"], e["res"],
                           fmt_size(e["size"])), (46, 204, 113)))
                    continue
                if p not in G["emby_asked"]:
                    doublons.append((p, name, size, st, ent, src))
                continue

            # --- controle d'espace ---
            good, d = fits(size, already)
            if not good and not force:
                refuses.append((p, size, d))
                if origin != "manuel" and p not in G["deferred"]:
                    G["deferred"].add(p)
                    top = ", ".join(
                        "%s (%s)" % (t["name"][:34], fmt_size(remaining(t)))
                        for t in sorted(d["firm"],
                                        key=lambda x: -remaining(x))[:2])
                    ui(lambda n=os.path.basename(p), sz=size, dd=d, tp=top:
                       add_log("REPORTE (espace) : %s demande %s, il reste %s "
                               "projetes (marge %s). Place reservee par : %s. "
                               "Le fichier reste dans le dossier surveille."
                               % (n, fmt_size(sz),
                                  fmt_size(dd["projected"]) if dd["projected"] > 0
                                  else "0 o", fmt_size(dd["min_free"]), tp
                                  or "aucun torrent actif"), (235, 140, 20)))
                continue

            with _busy:
                c.add_file(data, os.path.basename(p), cat, sp, paused)
            ok += 1
            already += size
            G["sent"][p] = time.time()
            G["deferred"].discard(p)
            if not idx and not G.get("local_idx"):
                note = "aucune source de comparaison chargee"
            elif st == EM_BETTER:
                note = "UPGRADE : %s n'a que du %s" % (src, ent["res"])
            elif st in (EM_SAME, EM_OK):
                note = "deja present (%s), envoye sur demande" % src
            else:
                note = "absent d'Emby et des dossiers"
            ui(lambda n=os.path.basename(p), sz=size, nt=note: add_log(
                "Envoye (%s) : %s  (%s)  -  %s" % (origin, n, fmt_size(sz), nt),
                (46, 204, 113)))
            _after_send(p)
        except Exception as exc:
            ko += 1
            ui(lambda n=os.path.basename(p), e=str(exc): add_log(
                "ECHEC : %s  ->  %s" % (n, e), (215, 75, 90)))
    if doublons:
        for d0 in doublons:
            G["emby_asked"].add(d0[0])
        ui(lambda: ask_emby_dup(doublons, origin))
    if refuses and origin == "manuel":
        ui(lambda: ask_force(refuses))
    return ok, ko


def ask_emby_dup(doublons, origin):
    """Met les doublons en file. Chaque torrent est soumis SEPAREMENT :
    une fenetre, un fichier, une decision."""
    G["dup_queue"].extend((d, origin) for d in doublons)
    if not G.get("dup_open"):
        _ask_next_dup()


def _close_dup(tag):
    G["dup_open"] = False
    if dpg.does_item_exist(tag):
        dpg.delete_item(tag)


def _dup_choice(choix, entry, tag):
    """choix : 'oui' telecharger, 'non' ecarter, 'plus_tard' laisser en place."""
    (pth, name, size, st, ent, src), origin = entry
    _close_dup(tag)
    if choix == "oui":
        threading.Thread(
            target=lambda: send_files([pth], origin, force_emby=True),
            daemon=True).start()
    elif choix == "non":
        _refuse(pth, "refus manuel")
    else:
        G["emby_asked"].discard(pth)   # il sera represente au prochain cycle
    _ask_next_dup()


def _dup_stop():
    """Interrompt la file sans rien decider : les fichiers restent en place."""
    n = len(G["dup_queue"])
    for (pth, *_r), _o in G["dup_queue"]:
        G["emby_asked"].discard(pth)
    G["dup_queue"].clear()
    if n:
        add_log("%d torrent(s) laisse(s) de cote : ils seront represente(s) au "
                "prochain cycle." % n, (150, 150, 175))


def _ask_next_dup():
    """Affiche la fenetre du prochain torrent en attente."""
    global _mid
    while G["dup_queue"]:
        entry = G["dup_queue"].pop(0)
        (pth, name, size, st, ent, src), origin = entry
        if not os.path.exists(pth):
            continue                    # deja traite entre-temps
        reste = len(G["dup_queue"])
        G["dup_open"] = True
        _mid += 1
        tag = "sb_dup_%d" % _mid
        with dpg.window(label="Deja present  -  %s" % os.path.basename(pth)[:48],
                        tag=tag, modal=True, width=820, autosize=True,
                        pos=[160, 170], no_close=True):
            dpg.add_text(os.path.basename(pth), color=(220, 200, 120))
            dpg.add_text("Ce film est deja chez toi. Le telecharger "
                         "consommerait %s pour rien." % fmt_size(size),
                         wrap=790, color=(235, 140, 20))
            dpg.add_separator()
            dpg.add_text("A TELECHARGER", color=(120, 200, 255))
            dpg.add_text("   %s" % name[:88])
            dpg.add_text("   %s  -  %s" % (TIER_LBL[tier_from_name(name)],
                                           fmt_size(size)))
            dpg.add_spacer(height=4)
            dpg.add_text("DEJA PRESENT  (%s)" % src, color=(120, 200, 255))
            dpg.add_text("   %s (%s)" % (ent["name"], ent["year"] or "?"),
                         color=EM_COL[st])
            dpg.add_text("   fichier : %s" % (ent.get("fname") or ent["name"]))
            dpg.add_text("   %s  -  %s        [%s]"
                         % (ent["res"], fmt_size(ent["size"]), EM_LBL[st]),
                         color=EM_COL[st])
            if ent.get("path"):
                dpg.add_text("   %s" % ent["path"], wrap=790,
                             color=(150, 150, 175))
            dpg.add_text("   rapprochement : %s" % ent["method"],
                         color=(150, 150, 175))
            dpg.add_separator()
            with dpg.group(horizontal=True):
                b = dpg.add_button(label="Telecharger quand meme", width=250,
                                   user_data=(entry, tag),
                                   callback=lambda s, a, u: _dup_choice(
                                       "oui", u[0], u[1]))
                dpg.bind_item_theme(b, "sb_th_danger")
                b2 = dpg.add_button(label="Ne pas telecharger", width=250,
                                    user_data=(entry, tag),
                                    callback=lambda s, a, u: _dup_choice(
                                        "non", u[0], u[1]))
                dpg.bind_item_theme(b2, "sb_th_ok")
                dpg.add_button(label="Decider plus tard", width=250,
                               user_data=(entry, tag),
                               callback=lambda s, a, u: _dup_choice(
                                   "plus_tard", u[0], u[1]))
            with dpg.group(horizontal=True):
                dpg.add_text("%d autre(s) en attente" % reste if reste
                             else "dernier de la file",
                             color=(150, 150, 175))
                if reste:
                    dpg.add_button(label="Arreter les questions", width=200,
                                   user_data=tag,
                                   callback=lambda s, a, u: (_close_dup(u),
                                                             _dup_stop()))
        return
    G["dup_open"] = False


def _skip_dups(paths):
    dest = refus_dir_for(paths[0]) if paths else ""
    n = sum(1 for p in paths if _refuse(p, "refus manuel"))
    set_status("%d torrent(s) deplace(s) vers %s." % (n, dest), (46, 204, 113))


def ask_force(refuses):
    """Envoi manuel refuse faute de place : on laisse le dernier mot a l'humain."""
    global _mid
    _mid += 1
    tag = "sb_force_%d" % _mid
    d = refuses[0][2]
    total = sum(r[1] for r in refuses)
    with dpg.window(label="Espace insuffisant", tag=tag, modal=True, width=680,
                    autosize=True, pos=[170, 190], no_resize=True):
        dpg.add_text("%d torrent(s) representant %s n'ont pas ete envoyes."
                     % (len(refuses), fmt_size(total)), wrap=650)
        dpg.add_spacer(height=4)
        dpg.add_text("Libre maintenant : %s\n"
                     "Deja engage par les telechargements en cours : %s\n"
                     "Libre une fois ceux-ci termines : %s\n"
                     "Marge de securite exigee : %s%s"
                     % (fmt_size(d["free"]), fmt_size(d["pending"]),
                        fmt_size(d["projected"]) if d["projected"] > 0 else "0 o",
                        fmt_size(d["min_free"]),
                        "\nTorrents en pause / en erreur comptes : %s"
                        % fmt_size(d["dormant"]) if d.get("dormant")
                        and CFG.get("count_paused") else ""),
                     wrap=650, color=(235, 140, 20))
        dpg.add_spacer(height=4)
        with dpg.child_window(height=150, border=True, horizontal_scrollbar=True):
            for line in space_detail_text(d).splitlines():
                dpg.add_text(line or " ",
                             color=(235, 140, 20) if line.startswith("RESERVENT")
                             else (46, 204, 113) if line.startswith("NE RESERVENT")
                             else (200, 200, 215))
        dpg.add_separator()
        for pth, sz, _ in refuses[:8]:
            dpg.add_text("  - %s  (%s)" % (os.path.basename(pth)[:70],
                                           fmt_size(sz)),
                         color=(200, 200, 215))
        if len(refuses) > 8:
            dpg.add_text("  ... et %d autre(s)" % (len(refuses) - 8),
                         color=(150, 150, 175))
        dpg.add_separator()
        with dpg.group(horizontal=True):
            b = dpg.add_button(label="Envoyer quand meme", width=320,
                               user_data=([r[0] for r in refuses], tag),
                               callback=lambda s, a, u: (
                                   dpg.delete_item(u[1]),
                                   threading.Thread(
                                       target=lambda: send_files(u[0], "force",
                                                                 force=True),
                                       daemon=True).start()))
            dpg.bind_item_theme(b, "sb_th_danger")
            dpg.add_button(label="Annuler", width=320, user_data=tag,
                           callback=lambda s, a, u: dpg.delete_item(u))


def move_aside(path, dest_dir):
    """Deplace un .torrent dans un sous-dossier. -> chemin final ou None."""
    os.makedirs(dest_dir, exist_ok=True)
    target = os.path.join(dest_dir, os.path.basename(path))
    if os.path.exists(target):     # collision : suffixe horodate
        stem, ext = os.path.splitext(os.path.basename(path))
        target = os.path.join(dest_dir, "%s_%s%s"
                              % (stem, time.strftime("%H%M%S"), ext))
    shutil.move(path, target)
    return target


def refus_dir_for(path):
    """Dossier ou atterrissent les torrents refuses parce que deja possedes."""
    d = (CFG.get("refus_dir") or "").strip()
    if d:
        return d
    base = CFG.get("watch_dir") or os.path.dirname(path)
    return os.path.join(base, "_deja_presents")


def _after_send(path):
    """Archive ou supprime le .torrent local une fois pousse sur la seedbox."""
    mode = CFG.get("after_send", "archiver")
    if mode == "rien":
        return
    try:
        if mode == "supprimer":
            os.remove(path)
            return
        move_aside(path, CFG.get("archive_dir")
                   or os.path.join(os.path.dirname(path), "_envoyes"))
    except Exception as exc:
        ui(lambda e=str(exc): add_log("Post-traitement impossible : %s" % e,
                                      (235, 140, 20)))


def _refuse(path, motif=""):
    """Ecarte un .torrent : il quitte le dossier surveille pour le dossier
    des refus, ce qui l'empeche de revenir a chaque cycle."""
    G["emby_skip"].add(path)
    dest = refus_dir_for(path)
    if not os.path.exists(path):
        return None
    try:
        final = move_aside(path, dest)
        ui(lambda n=os.path.basename(path), d=dest, m=motif: add_log(
            "Ecarte -> %s : %s%s" % (d, n, "  (%s)" % m if m else ""),
            (46, 204, 113)))
        return final
    except Exception as exc:
        ui(lambda n=os.path.basename(path), e=str(exc): add_log(
            "Deplacement impossible : %s  ->  %s" % (n, e), (235, 140, 20)))
        return None


def pick_files(sender=None, app_data=None, user_data=None):
    try:
        import tkinter as tk
        from tkinter import filedialog
        r = tk.Tk()
        r.withdraw()
        r.attributes("-topmost", True)
        files = filedialog.askopenfilenames(
            title="Fichiers .torrent a envoyer",
            filetypes=[("Torrents", "*.torrent"), ("Tous", "*.*")])
        r.destroy()
    except Exception as exc:
        modal("Selecteur indisponible", str(exc))
        return
    files = [f for f in files or []]
    if not files:
        return
    threading.Thread(target=lambda: send_files(files, "manuel"),
                     daemon=True).start()
    set_status("Envoi de %d fichier(s)..." % len(files), (235, 140, 20))


def send_magnets(sender=None, app_data=None, user_data=None):
    raw = gv("sb_magnets") or ""
    uris = [l.strip() for l in raw.splitlines() if l.strip()]
    uris = [u for u in uris if u.startswith("magnet:") or len(u) == 40]
    if not uris:
        modal("Aucun lien", "Colle un ou plusieurs liens magnet, un par ligne.")
        return
    c = G["client"]
    if not c:
        modal("Non connecte", "Connecte-toi d'abord a la seedbox.")
        return
    cat, sp, paused = _opts()

    def worker():
        ok = 0
        for u in uris:
            if len(u) == 40 and not u.startswith("magnet:"):
                u = "magnet:?xt=urn:btih:" + u
            try:
                with _busy:
                    c.add_magnet(u, cat, sp, paused)
                ok += 1
                ui(lambda x=u: add_log("Magnet envoye : %s" % x[:80],
                                       (46, 204, 113)))
            except Exception as exc:
                ui(lambda e=str(exc): add_log("ECHEC magnet : %s" % e,
                                              (215, 75, 90)))
        ui(lambda: (dpg.set_value("sb_magnets", ""),
                    set_status("%d/%d magnet(s) envoye(s)." % (ok, len(uris)),
                               (46, 204, 113)), refresh_now()))
    threading.Thread(target=worker, daemon=True).start()


# =====================================================================
#  DOSSIER SURVEILLE
# =====================================================================
def _watch_scan(folder):
    """Retourne les .torrent stables (taille figee) jamais envoyes."""
    out = []
    try:
        names = os.listdir(folder)
    except Exception:
        return out
    for n in names:
        if not n.lower().endswith(".torrent"):
            continue
        p = os.path.join(folder, n)
        if p in G["sent"]:
            continue
        try:
            st = os.stat(p)
            # on ignore un fichier encore en cours d'ecriture (< 2 s)
            if time.time() - st.st_mtime < 2:
                continue
            if st.st_size == 0:
                continue
        except Exception:
            continue
        out.append(p)
    return out


def _watch_loop():
    add_log("Surveillance demarree : %s" % CFG["watch_dir"], (120, 200, 255))
    while not _stop_watch.is_set():
        folder = CFG.get("watch_dir", "")
        if folder and os.path.isdir(folder) and G["client"]:
            found = _watch_scan(folder)
            if found:
                ok, ko = send_files(found, "surveille")
                if ok:
                    ui(refresh_now)
        _stop_watch.wait(max(3, int(CFG.get("watch_interval", 20))))
    add_log("Surveillance arretee.", (150, 150, 175))


def start_watch_thread():
    if not CFG.get("watch_dir"):
        return
    _stop_watch.clear()
    t = threading.Thread(target=_watch_loop, daemon=True)
    G["watch_thread"] = t
    t.start()


def toggle_watch(sender=None, app_data=None, user_data=None):
    persist()
    if CFG["watch_enabled"]:
        if not CFG["watch_dir"] or not os.path.isdir(CFG["watch_dir"]):
            dpg.set_value("sb_watch_on", False)
            CFG["watch_enabled"] = False
            save_cfg(CFG)
            modal("Dossier invalide",
                  "Indique un dossier surveille existant avant d'activer.")
            return
        start_watch_thread()
        set_status("Surveillance active sur %s" % CFG["watch_dir"],
                   (46, 204, 113))
    else:
        _stop_watch.set()
        set_status("Surveillance arretee.", (150, 150, 175))


def browse_dir(target_tag):
    try:
        import tkinter as tk
        from tkinter import filedialog
        r = tk.Tk()
        r.withdraw()
        r.attributes("-topmost", True)
        d = filedialog.askdirectory(title="Choisir un dossier")
        r.destroy()
        if d:
            dpg.set_value(target_tag, os.path.normpath(d))
            persist()
    except Exception as exc:
        modal("Selecteur indisponible", str(exc))


# =====================================================================
#  APPARIEMENT SEEDBOX <-> EMBY
# =====================================================================
EM_SAME = "same"     # meme fichier (taille identique) : deja rapatrie
EM_OK = "ok"         # present sur Emby en qualite egale ou meilleure
EM_BETTER = "better" # present sur Emby mais la seedbox a mieux : NE PAS effacer
EM_NONE = None

EM_COL = {EM_SAME: (46, 204, 113), EM_OK: (110, 200, 150),
          EM_BETTER: (235, 140, 20)}
EM_LBL = {EM_SAME: "Sur Emby (identique)", EM_OK: "Sur Emby",
          EM_BETTER: "Emby a moins bien"}
# Libelles neutres, pour la source disque ou tout contexte hors Emby.
EM_LBL_N = {EM_SAME: "fichier identique", EM_OK: "deja present",
            EM_BETTER: "la seedbox a mieux"}


def fetch_emby_movies(base, key, uid, cb=None):
    """Lit la bibliotheque page par page. cb(recuperes, total) a chaque page :
    Emby renvoie TotalRecordCount des la premiere reponse, la progression est
    donc exacte et non simulee."""
    params = {"Recursive": "true", "IncludeItemTypes": "Movie",
              "Fields": "Path,MediaSources,ProductionYear,OriginalTitle,"
                        "ProviderIds",
              "Limit": 300, "StartIndex": 0}
    if uid:
        params["UserId"] = uid
    items = []
    while True:
        data = emby_get(base, key, "/Items", dict(params))
        got = data.get("Items", []) or []
        items.extend(got)
        total = max(int(data.get("TotalRecordCount", 0) or 0), len(items))
        if cb:
            cb(len(items), total)
        if len(got) < params["Limit"] or len(items) >= total or not got:
            break
        params["StartIndex"] += len(got)
    return items


def build_index(raw):
    """Index commun (nom de fichier, titre+annee, jetons) sur une liste
    d'entrees {name, year, norm, norm_orig, stem_key, size, tier, res, path}."""
    idx = {"entries": raw, "by_stem": {}, "by_ty": {}, "tok": {}}
    for i, e in enumerate(raw):
        if e["stem_key"]:
            idx["by_stem"].setdefault(e["stem_key"], []).append(i)
        if e["norm"]:
            idx["by_ty"].setdefault((e["norm"], e["year"]), []).append(i)
            if e["norm_orig"]:
                idx["by_ty"].setdefault((e["norm_orig"], e["year"]), []).append(i)
            for tk in set(_toks(e["norm"]) + _toks(e["norm_orig"])):
                idx["tok"].setdefault(tk, set()).add(i)
    return idx


def build_emby_index(movies):
    """Un film peut avoir plusieurs versions : une entree par fichier."""
    entries = []
    for it in movies:
        name = it.get("Name", "") or ""
        year = int(it.get("ProductionYear", 0) or 0)
        orig = it.get("OriginalTitle", "") or ""
        nt, _ = split_title_year("%s %s" % (name, year or ""))
        no, _ = split_title_year(orig) if orig else ("", None)
        pids = {str(k).lower(): str(v) for k, v in
                (it.get("ProviderIds") or {}).items()}
        srcs = it.get("MediaSources") or [{"Path": it.get("Path", ""),
                                           "Size": 0}]
        for src in srcs:
            path = src.get("Path") or it.get("Path") or ""
            streams = src.get("MediaStreams") or []
            vid = next((x for x in streams if x.get("Type") == "Video"), None)
            tier = 0
            if vid:
                mx = max(int(vid.get("Width", 0) or 0),
                         int(vid.get("Height", 0) or 0))
                tier = (4 if mx >= 2000 else 3 if mx >= 1400 else
                        2 if mx >= 1000 else 1 if mx else 0)
            if not tier and path:
                tier = tier_from_name(Path(path).name)
            entries.append({
                "name": name, "year": year, "norm": nt or clean_tokens(name),
                "norm_orig": no, "path": path,
                "stem_key": norm_key(Path(path).stem) if path else "",
                "size": int(src.get("Size", 0) or 0),
                "tier": tier, "res": TIER_LBL[tier],
                "imdb": pids.get("imdb", "").lower(),
                "tmdb": pids.get("tmdb", ""),
                "attrs": emby_source_attrs(path, streams, tier)})
    idx = build_index(entries)
    # Acces direct par identifiant : pages IMDb / TMDB ouvertes dans le
    # navigateur, sans passer par le rapprochement des titres.
    idx["by_id"] = {}
    for i, e in enumerate(entries):
        if e["imdb"]:
            idx["by_id"].setdefault("imdb:" + e["imdb"], []).append(i)
        if e["tmdb"]:
            idx["by_id"].setdefault("tmdb:" + e["tmdb"], []).append(i)
    return idx


def scan_local_dirs(dirs, cb=None):
    """Parcourt des dossiers et indexe les fichiers video qui s'y trouvent.

    Complement d'Emby : un film rapatrie mais pas encore indexe dans la
    mediatheque (dossier 'a trier', partage non scanne) est bien present sur
    le disque, et rien ne sert de le retelecharger.
    """
    entries = []
    seen = 0
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for root, _dirs, files in os.walk(d):
            for f in files:
                if Path(f).suffix.lower() not in VIDEO_EXT:
                    continue
                full = os.path.join(root, f)
                try:
                    size = os.path.getsize(full)
                except Exception:
                    size = 0
                stem = Path(f).stem
                nt, ny = split_title_year(stem)
                if not nt:                       # nom de fichier inexploitable
                    nt, ny2 = split_title_year(Path(root).name)
                    ny = ny or ny2
                tier = tier_from_name(f) or tier_from_name(root)
                entries.append({
                    "name": f, "year": ny or 0, "norm": nt,
                    "norm_orig": "", "path": full,
                    "stem_key": norm_key(stem), "size": size,
                    "tier": tier, "res": TIER_LBL[tier],
                    "attrs": release_attrs("%s %s"
                                           % (Path(root).name, f))})
                seen += 1
                if cb and seen % 200 == 0:
                    cb(seen, d)
    if cb:
        cb(seen, "")
    return entries


def match_torrent_to_emby(t, idx, threshold=80):
    """Cherche le film Emby correspondant a un torrent. -> (entree, score, methode)"""
    name = t["name"] or ""
    ext = Path(name).suffix.lower()
    stem = name[:-len(ext)] if ext in VIDEO_EXT else name
    hit = idx["by_stem"].get(norm_key(stem))
    if hit:
        return idx["entries"][hit[0]], 100, "Fichier"
    nt, ny = split_title_year(name)
    if nt and ny:
        hit = idx["by_ty"].get((nt, ny))
        if hit:
            return idx["entries"][hit[0]], 96, "Titre+annee"
    toks = _toks(nt)
    if not toks:
        return None, 0, ""
    toks.sort(key=lambda x: len(idx["tok"].get(x, ())))
    cand = set()
    for tk in toks[:3]:
        cand |= idx["tok"].get(tk, set())
        if len(cand) > 900:
            break
    floor = threshold if ny else min(97, threshold + 8)
    best, best_e = 0, None
    for i in cand:
        e = idx["entries"][i]
        if ny and e["year"] and abs(e["year"] - ny) > 1:
            continue
        sc = max(fuzzy(nt, e["norm"]),
                 fuzzy(nt, e["norm_orig"]) if e["norm_orig"] else 0)
        if ny and e["year"] and ny != e["year"]:
            sc -= 6
        if sc > best:
            best, best_e = sc, e
    if best_e is not None and best >= floor:
        return best_e, int(best), "Fuzzy %d%%" % best
    return None, 0, ""


def emby_verdict(name, size, idx, tol_pct=2.0):
    """-> (etat, entree | None). Fonctionne pour l'index Emby comme pour
    l'index des dossiers locaux : meme structure, meme logique."""
    if not idx:
        return EM_NONE, None
    e, score, method = match_torrent_to_emby({"name": name, "size": size}, idx,
                                             int(CFG.get("emby_fuzzy", 80)))
    if not e:
        return EM_NONE, None
    ttier = tier_from_name(name)
    same = bool(e["size"] and size and
                abs(size - e["size"]) * 100.0 / max(size, e["size"]) <= tol_pct)
    if same:
        st = EM_SAME
    elif ttier and e["tier"] and ttier > e["tier"]:
        st = EM_BETTER
    else:
        st = EM_OK
    return st, {"name": e["name"], "year": e["year"], "res": e["res"],
                "size": e["size"], "score": score, "method": method,
                "path": e["path"], "tier": e["tier"],
                "fname": Path(e["path"]).name if e["path"] else ""}


def apply_emby_status(t, idx, tol_pct=2.0):
    """Renseigne t['emby'] et t['local'] : entrees trouvees + verdicts."""
    st, info = emby_verdict(t["name"], t["size"], idx, tol_pct)
    t["emby_state"] = st
    t["emby"] = info
    lst, linfo = emby_verdict(t["name"], t["size"], G.get("local_idx"), tol_pct)
    t["local_state"] = lst
    t["local"] = linfo


def present_state(t):
    """Meilleur verdict toutes sources confondues (Emby ou dossiers)."""
    order = {EM_SAME: 0, EM_OK: 1, EM_BETTER: 2, EM_NONE: 3}
    a, b = t.get("emby_state"), t.get("local_state")
    return a if order.get(a, 3) <= order.get(b, 3) else b


def present_entry(t):
    return t.get("emby") if present_state(t) == t.get("emby_state") \
        and t.get("emby") else (t.get("local") or t.get("emby"))


def apply_emby_seuil():
    """Applique le nouveau seuil une fois le curseur relache."""
    val = int(gv("sb_embyfuzzy", CFG["emby_fuzzy"]) or 80)
    if val == int(CFG.get("emby_fuzzy", 80)) and G.get("_seuil_fait"):
        return
    G["_seuil_fait"] = True
    persist()
    if not G.get("emby_idx"):
        return
    refresh_emby_status()
    render_dashboard()
    on_emby = sum(1 for t in G["torrents"] if t.get("emby"))
    set_status("Seuil de rapprochement : %d%%  -  %d torrent(s) trouves sur "
               "Emby" % (val, on_emby), (46, 204, 113))


def refresh_emby_status(cb=None):
    idx = G.get("emby_idx")
    lst = list(G["torrents"])
    for i, t in enumerate(lst):
        apply_emby_status(t, idx)
        if cb and (i % 20 == 0 or i == len(lst) - 1):
            cb(i + 1, len(lst))


def rebuild_local_dirs():
    dpg.delete_item("sb_dirlist", children_only=True)
    if not G["local_dirs"]:
        dpg.add_text("Aucun dossier. Colle un chemin (UNC accepte) puis "
                     "Ajouter.", parent="sb_dirlist", color=(150, 150, 175))
        return
    for i, d in enumerate(G["local_dirs"]):
        with dpg.group(horizontal=True, parent="sb_dirlist"):
            dpg.add_button(label="X", width=26, user_data=i,
                           callback=lambda s, a, u: del_local_dir(u))
            ok = os.path.isdir(d)
            dpg.add_text(d, color=(220, 200, 120) if ok else (215, 75, 90))
            if not ok:
                dpg.add_text("(inaccessible)", color=(215, 75, 90))


def add_local_dir(sender=None, app_data=None, user_data=None):
    p = (gv("sb_dirin") or "").strip().strip('"')
    if not p:
        return
    if p in G["local_dirs"]:
        set_status("Dossier deja present.", (235, 140, 20))
        return
    if not os.path.isdir(p):
        set_status("Dossier inaccessible : %s" % p, (215, 75, 90))
        return
    G["local_dirs"].append(p)
    dpg.set_value("sb_dirin", "")
    rebuild_local_dirs()
    CFG["local_dirs"] = list(G["local_dirs"])
    save_cfg(CFG)


def del_local_dir(i):
    try:
        G["local_dirs"].pop(i)
    except Exception:
        return
    rebuild_local_dirs()
    CFG["local_dirs"] = list(G["local_dirs"])
    save_cfg(CFG)


def do_scan_local(sender=None, app_data=None, user_data=None):
    dirs = list(G["local_dirs"])
    if not dirs:
        modal("Aucun dossier",
              "Ajoute au moins un dossier a comparer (le partage de ton NAS, "
              "un dossier 'a trier'...).")
        return
    dpg.configure_item("sb_dirscan", enabled=False)
    set_status("Analyse des dossiers...", (235, 140, 20))

    def worker():
        try:
            pb_set(0.05, "Parcours des dossiers...")

            def on_file(n, d):
                pb_set(0.05 + min(0.75, n / 20000.0),
                       "Parcours : %d fichiers video  %s" % (n, Path(d).name))

            entries = scan_local_dirs(dirs, on_file)
            pb_set(0.85, "Indexation de %d fichiers..." % len(entries))
            idx = build_index(entries)
            G["local_idx"] = idx
            pb_set(0.92, "Rapprochement des torrents...")
            refresh_emby_status()
            pb_set(1.0, "Termine")

            def done():
                pb_hide()
                n_on = sum(1 for t in G["torrents"] if t.get("local"))
                dpg.set_value("sb_dirlbl", "%d fichiers indexes" % len(entries))
                dpg.configure_item("sb_dirscan", enabled=True)
                set_status("Dossiers : %d fichiers video  -  %d torrent(s) "
                           "deja sur disque" % (len(entries), n_on),
                           (46, 204, 113))
                add_log("Dossiers analyses : %d fichiers video dans %d dossier(s). "
                        "%d torrent(s) correspondent a un fichier existant."
                        % (len(entries), len(dirs), n_on), (46, 204, 113))
                render_dashboard(force=True)
            ui(done)
        except Exception as exc:
            msg = str(exc)
            pb_hide()
            ui(lambda: (dpg.configure_item("sb_dirscan", enabled=True),
                        set_status("Analyse des dossiers : echec", (215, 75, 90)),
                        modal("Analyse impossible", msg)))

    threading.Thread(target=worker, daemon=True).start()


def do_load_emby(sender=None, app_data=None, user_data=None):
    url = (gv("sb_emby_url") or "").strip().rstrip("/")
    key = (gv("sb_emby_key") or "").strip()
    uid = (gv("sb_emby_uid") or "").strip()
    if not url or not key:
        modal("Parametres Emby manquants",
              "Renseigne l'URL du serveur Emby et la cle API.")
        return
    if not url.startswith("http"):
        url = "http://" + url
        dpg.set_value("sb_emby_url", url)
    save_emby_creds(url, key, uid)
    set_status("Lecture de la bibliotheque Emby...", (235, 140, 20))
    dpg.configure_item("sb_emby_btn", enabled=False)

    def worker():
        try:
            pb_set(0.02, "Connexion au serveur Emby...")

            def on_page(got, total):
                pb_set(0.02 + 0.78 * (got / max(1, total)),
                       "Lecture de la bibliotheque Emby : %d / %d films"
                       % (got, total))

            movies = fetch_emby_movies(url, key, uid, on_page)
            pb_set(0.82, "Indexation de %d films..." % len(movies))
            idx = build_emby_index(movies)
            G["emby_idx"] = idx
            pb_set(0.88, "Rapprochement des torrents...")
            refresh_emby_status(
                lambda i, n: pb_set(0.88 + 0.12 * (i / max(1, n)),
                                    "Rapprochement : %d / %d torrents" % (i, n)))
            pb_set(1.0, "Termine")

            def done():
                n = len(idx["entries"])
                pb_hide()
                dpg.set_value("sb_emby_lbl", "%d fichiers Emby" % n)
                dpg.configure_item("sb_emby_btn", enabled=True)
                on_emby = sum(1 for t in G["torrents"] if t.get("emby"))
                set_status("Emby : %d films (%d fichiers)  -  %d torrent(s) "
                           "deja presents" % (len(movies), n, on_emby),
                           (46, 204, 113))
                add_log("Emby charge : %d films, %d fichiers. %d torrent(s) de "
                        "la seedbox sont deja sur Emby."
                        % (len(movies), n, on_emby), (46, 204, 113))
                render_dashboard(force=True)
            ui(done)
        except Exception as exc:
            detail = friendly_net_error(exc, url + "/Items")
            short = str(exc)[:90]
            log("Emby: %s" % detail.replace("\n", " | "))
            pb_hide()

            def fail():
                dpg.configure_item("sb_emby_btn", enabled=True)
                set_status("Emby : echec (%s)" % short, (215, 75, 90))
                modal("Connexion Emby impossible", detail, wide=760)
            ui(fail)

    threading.Thread(target=worker, daemon=True).start()


# =====================================================================
#  SAUVEGARDE AUTOMATIQUE DE LA LISTE DES TORRENTS
# =====================================================================
# Deux fichiers complementaires :
#   - un instantane quotidien : la photo exacte de la seedbox ce jour-la ;
#   - un INVENTAIRE cumulatif : tout ce qui a ete vu au moins une fois, meme
#     supprime depuis. C'est lui qui permet de reconstituer une seedbox perdue.
INV_FILE = APP_DIR / "seedbox_inventaire.json"

SNAP_FIELDS = ["hash", "name", "size", "category", "save_path", "tracker",
               "state", "progress", "ratio", "uploaded", "added_on",
               "first_seen", "last_seen", "present", "emby_name", "emby_path"]


def snap_dir():
    d = (CFG.get("snap_dir") or "").strip()
    return Path(d) if d else (APP_DIR / "seedbox_snapshots")


def load_inventory():
    if G.get("inv") is not None:
        return G["inv"]
    inv = {}
    if INV_FILE.exists():
        try:
            inv = json.loads(INV_FILE.read_text(encoding="utf-8"))
        except Exception as exc:
            log("inventaire illisible : %s" % exc)
            inv = {}
    G["inv"] = inv
    return inv


def save_inventory(inv):
    try:
        tmp = INV_FILE.with_suffix(".tmp")
        tmp.write_text(json.dumps(inv, ensure_ascii=False, indent=1),
                       encoding="utf-8")
        tmp.replace(INV_FILE)          # ecriture atomique : jamais de fichier tronque
        return True
    except Exception as exc:
        log("ecriture inventaire : %s" % exc)
        return False


def _snap_row(t, now):
    e = t.get("emby") or {}
    return {"hash": t["hash"], "name": t["name"], "size": t["size"],
            "category": t["category"], "save_path": t["save_path"],
            "tracker": t["tracker"], "state": t["state"],
            "progress": round(t["progress"], 4), "ratio": round(t["ratio"], 3),
            "uploaded": t["uploaded"], "added_on": t["added_on"],
            "first_seen": now, "last_seen": now, "present": True,
            "emby_name": e.get("fname", ""), "emby_path": e.get("path", "")}


def write_snapshot(lst, force=False):
    """Ecrit l'instantane du jour et met a jour l'inventaire cumulatif."""
    now = time.strftime("%Y-%m-%d %H:%M:%S")
    d = snap_dir()
    try:
        d.mkdir(parents=True, exist_ok=True)
    except Exception as exc:
        log("dossier de sauvegarde : %s" % exc)
        return None

    rows = [_snap_row(t, now) for t in lst]
    day = time.strftime("%Y-%m-%d")
    try:
        (d / ("seedbox_%s.json" % day)).write_text(
            json.dumps({"date": now, "torrents": rows}, ensure_ascii=False,
                       indent=1), encoding="utf-8")
        with open(d / ("seedbox_%s.csv" % day), "w", newline="",
                  encoding="utf-8-sig") as f:
            w = csv.DictWriter(f, fieldnames=SNAP_FIELDS, delimiter=";",
                               extrasaction="ignore")
            w.writeheader()
            for r in rows:
                w.writerow(r)
    except Exception as exc:
        log("ecriture instantane : %s" % exc)

    inv = load_inventory()
    vus = set()
    for r in rows:
        h = r["hash"]
        vus.add(h)
        if h in inv:
            anc = inv[h]
            r["first_seen"] = anc.get("first_seen", now)
            # on ne perd jamais un rapprochement Emby deja connu
            r["emby_name"] = r["emby_name"] or anc.get("emby_name", "")
            r["emby_path"] = r["emby_path"] or anc.get("emby_path", "")
        inv[h] = r
    for h, r in inv.items():
        if h not in vus and r.get("present"):
            r["present"] = False       # disparu : on conserve son dernier etat
    save_inventory(inv)
    prune_snapshots()
    G["_snap_at"] = time.time()
    G["_snap_sig"] = frozenset(vus)
    return len(rows), sum(1 for r in inv.values() if not r.get("present"))


def prune_snapshots():
    keep = int(CFG.get("snap_keep", 60) or 0)
    if keep <= 0:
        return
    limite = time.time() - keep * 86400
    try:
        for f in snap_dir().glob("seedbox_*.*"):
            if f.stat().st_mtime < limite:
                f.unlink()
    except Exception:
        pass


def snapshot_maybe(lst):
    """Appele a chaque rafraichissement. Ecrit si la composition a change ou
    si le delai de routine est ecoule : on ne reecrit pas 12 fois par minute."""
    if not CFG.get("snap_enabled", True):
        return
    sig = frozenset(t["hash"] for t in lst)
    change = sig != G.get("_snap_sig")
    echu = (time.time() - G.get("_snap_at", 0)) >= \
        max(1, int(CFG.get("snap_interval", 10) or 10)) * 60
    if not (change or echu):
        return
    res = write_snapshot(lst)
    if res and change and G.get("_snap_sig_init"):
        ui(lambda: add_log("Liste sauvegardee : %d torrent(s) presents, "
                           "%d disparu(s) conserve(s) dans l'inventaire."
                           % res, (120, 200, 255)))
    G["_snap_sig_init"] = True


# =====================================================================
#  TABLEAU DE BORD
# =====================================================================
_SORT_KEY = {
    "name": lambda t: t["name"].lower(),
    "state": lambda t: (_ST_ORDER.get(t["state"], 9), -t["dlspeed"],
                        t["name"].lower()),
    "progress": lambda t: t["progress"],
    "size": lambda t: t["size"],
    "dl": lambda t: t["dlspeed"],
    "ul": lambda t: t["upspeed"],
    "eta": lambda t: t["eta"] if 0 < t["eta"] < 8640000 else 1 << 40,
    "ratio": lambda t: t["ratio"],
    "peers": lambda t: t["seeds"],
    "cat": lambda t: t["category"].lower(),
    "tracker": lambda t: t["tracker"].lower(),
    "emby": lambda t: ({EM_SAME: 0, EM_OK: 1, EM_BETTER: 2}.get(
        t.get("emby_state"), 3), t["name"].lower()),
    "local": lambda t: ({EM_SAME: 0, EM_OK: 1, EM_BETTER: 2}.get(
        t.get("local_state"), 3), t["name"].lower()),
    "tq": lambda t: -tier_from_name(t["name"]),
    "eq": lambda t: -(t["emby"]["tier"] if t.get("emby") else -1),
    "esize": lambda t: -(t["emby"]["size"] if t.get("emby") else 0),
    "ediff": lambda t: -abs(_size_delta(t) or 0),
}


def _size_delta(t):
    """Ecart relatif entre la taille du torrent et celle du fichier Emby."""
    e = t.get("emby")
    if not e or not e["size"] or not t["size"]:
        return None
    return (t["size"] - e["size"]) * 100.0 / max(t["size"], e["size"])
_ST_ORDER = {"downloading": 0, "stalled": 1, "checking": 2, "queued": 3,
             "seeding": 4, "completed": 5, "paused": 6, "unknown": 7,
             "error": -1}
_FILTER_MAP = {"Telechargement": ("downloading", "stalled", "queued", "checking"),
               "Seed": ("seeding",), "En pause": ("paused",),
               "Erreur": ("error", "unknown"),
               "Termine": ("completed", "seeding")}
# Ordre des colonnes : (clef de tri, libelle, largeur, groupe).
# La comparaison est placee JUSTE APRES la taille du torrent, avant les
# colonnes de transfert : c'est l'information qu'on vient chercher, elle ne
# doit pas se trouver a 1500 px de scroll horizontal.
# groupe : "id" toujours visible, "cmp" comparaison, "tr" transfert.
_COLS = [(None, "", 28, "id"), ("name", "Nom", 330, "id"),
         ("state", "Etat", 105, "id"),
         ("progress", "Progression", 110, "tr"), ("progress", "%", 55, "tr"),
         ("size", "Taille", 85, "id"),
         ("tq", "Qual. torrent", 95, "cmp"),
         ("emby", "Sur Emby ?", 165, "cmp"), ("eq", "Qual. Emby", 90, "cmp"),
         ("esize", "Taille Emby", 95, "cmp"), ("ediff", "Ecart", 80, "cmp"),
         ("local", "Sur disque ?", 240, "cmp"),
         ("dl", "DL", 85, "tr"), ("ul", "UL", 85, "tr"),
         ("eta", "ETA", 75, "tr"), ("ratio", "Ratio", 60, "tr"),
         ("peers", "S / P", 65, "tr"), ("cat", "Categorie", 105, "tr"),
         ("tracker", "Tracker", 140, "tr")]

VUES = ("Tout", "Comparaison", "Transfert")


def col_visible(groupe, vue):
    if vue == "Comparaison":
        return groupe in ("id", "cmp")
    if vue == "Transfert":
        return groupe in ("id", "tr")
    return True


def change_vue(sender=None, app_data=None, user_data=None):
    """Change le jeu de colonnes affichees (reconstruit le tableau)."""
    CFG["vue"] = gv("sb_vue", "Tout") or "Tout"
    save_cfg(CFG)
    defer("vue", lambda: (drop_table(), render_dashboard(force=True)), 0.05)


def drop_table():
    if dpg.does_item_exist("sb_table"):
        dpg.delete_item("sb_table")
    G["rows"] = {}
    G["order"] = []


def on_sort(sender, sort_specs):
    """Clic sur un en-tete de colonne. sort_specs = [[col_id, sens]]."""
    if not sort_specs:
        return
    col_id, direction = sort_specs[0][0], sort_specs[0][1]
    key = G["colmap"].get(col_id)
    if not key:
        return
    CFG["sort_key"] = key
    CFG["sort_dir"] = 1 if direction > 0 else -1
    save_cfg(CFG)
    # hors de la frame de clic : l'en-tete de tri appartient au tableau qu'on
    # s'apprete a reconstruire.
    defer("tri", render_dashboard, 0.05)


EMBY_FILTERS = ("Tous", "Deja sur Emby", "Deja sur disque",
                "Present quelque part", "Absent partout", "Seedbox a mieux")


def visible_torrents():
    ft = (gv("sb_filter") or "").lower().strip()
    sf = gv("sb_statefilter") or "Tous"
    ef = gv("sb_embyfilter") or "Tous"
    out = []
    for t in G["torrents"]:
        if ft and ft not in t["name"].lower() and ft not in t["category"].lower():
            continue
        if sf != "Tous" and t["state"] not in _FILTER_MAP.get(sf, ()):
            continue
        est, lst = t.get("emby_state"), t.get("local_state")
        pst = present_state(t)
        if ef == "Deja sur Emby" and est not in (EM_SAME, EM_OK):
            continue
        if ef == "Deja sur disque" and lst not in (EM_SAME, EM_OK):
            continue
        if ef == "Present quelque part" and pst not in (EM_SAME, EM_OK):
            continue
        if ef == "Absent partout" and (est is not None or lst is not None):
            continue
        if ef == "Seedbox a mieux" and EM_BETTER not in (est, lst):
            continue
        out.append(t)
    key = _SORT_KEY.get(CFG.get("sort_key", "state"), _SORT_KEY["state"])
    out.sort(key=key, reverse=(int(CFG.get("sort_dir", 1)) < 0))
    return out


def _cells(t):
    """Valeurs de chaque colonne, dans l'ordre du tableau."""
    return {
        "name": t["name"][:78],
        "state": ST_LBL.get(t["state"], t["state"]),
        "pct": "%.1f%%" % (t["progress"] * 100),
        "size": fmt_size(t["size"]),
        "dl": fmt_speed(t["dlspeed"]),
        "ul": fmt_speed(t["upspeed"]),
        "eta": fmt_eta(t["eta"]),
        "ratio": "%.2f" % t["ratio"],
        "peers": "%d / %d" % (t["seeds"], t["peers"]),
        "cat": (t["category"] or "-")[:18],
        "tracker": (t["tracker"] or "-")[:24],
        "emby": _emby_cell(t),
        "tq": TIER_LBL[tier_from_name(t["name"])],
        "eq": t["emby"]["res"] if t.get("emby") else "-",
        "esize": fmt_size(t["emby"]["size"]) if t.get("emby") else "-",
        "ediff": _diff_cell(t),
        "local": _local_cell(t),
    }


def _diff_cell(t):
    d = _size_delta(t)
    if d is None:
        return "-"
    if abs(d) < 0.05:
        return "identique"
    return "%s%.1f%%" % ("+" if d > 0 else "-", abs(d))


def _local_cell(t):
    if not G.get("local_idx"):
        return "?"
    e = t.get("local")
    if not e:
        return "absent"
    return "%s  (%s, %s)" % (EM_LBL_N[t["local_state"]], e["res"],
                             fmt_size(e["size"]))


def _emby_cell(t):
    if not G.get("emby_idx"):
        return "?"
    if not t.get("emby"):
        return "absent"
    return EM_LBL[t["emby_state"]]


def update_header():
    lst = G["torrents"]
    dl = sum(t["dlspeed"] for t in lst)
    ul = sum(t["upspeed"] for t in lst)
    act = sum(1 for t in lst if t["state"] in ("downloading", "stalled"))
    seed = sum(1 for t in lst if t["state"] == "seeding")
    err = sum(1 for t in lst if t["state"] == "error")
    if dpg.does_item_exist("sb_headline"):
        dpg.set_value("sb_headline",
                      "%d torrents   -   %d en telechargement, %d en seed%s"
                      "      DL %s   UL %s"
                      % (len(lst), act, seed,
                         ", %d en erreur" % err if err else "",
                         fmt_speed(dl), fmt_speed(ul)))
        dpg.configure_item("sb_headline",
                           color=(215, 75, 90) if err else (150, 200, 240))
    update_disk()


def update_disk():
    """Bandeau d'occupation : etat actuel ET etat projete."""
    if not dpg.does_item_exist("sb_diskline"):
        return
    d = disk_summary()
    if not d["free"] and not d["total"]:
        dpg.set_value("sb_diskbar", 0.0)
        dpg.configure_item("sb_diskbar", overlay="espace disque inconnu")
        dpg.set_value("sb_diskline",
                      "Ce client ne publie pas l'espace disque. Renseigne le "
                      "quota de ton offre ci-dessus pour activer le suivi.")
        dpg.configure_item("sb_diskline", color=(150, 150, 175))
        return

    if d["total"]:
        dpg.set_value("sb_diskbar", max(0.0, min(1.0, d["pct"])))
        dpg.configure_item("sb_diskbar", overlay="%.0f%%  -  %s / %s utilises"
                           % (d["pct"] * 100, fmt_size(d["used"]),
                              fmt_size(d["total"])))
    else:
        dpg.set_value("sb_diskbar", 0.0)
        dpg.configure_item("sb_diskbar",
                           overlay="%s libres  (quota non renseigne)"
                           % fmt_size(d["free"]))

    txt = "Libre : %s" % fmt_size(d["free"])
    if d["pending"]:
        txt += "      En cours d'ecriture : %s" % fmt_size(d["pending"])
        txt += ("      -->  Libre a la fin : %s"
                % (fmt_size(d["projected"]) if d["projected"] > 0 else "0 o"))
    txt += "      Occupe par les torrents : %s" % fmt_size(d["used_tor"])
    if d["dormant"]:
        txt += ("      [%d dormants (pause/erreur/sans pair) : %s %s]"
                % (d["n_dormant"], fmt_size(d["dormant"]),
                   "deduits" if CFG.get("count_paused", False)
                   else "NON deduits"))

    col = (150, 200, 240)
    if d["projected"] < 0:
        col = (215, 75, 90)
        txt += "      SATURATION : il manque %s" % fmt_size(-d["projected"])
    elif d["projected"] < d["min_free"]:
        col = (235, 140, 20)
        txt += "      (sous la marge de %s)" % fmt_size(d["min_free"])
    dpg.set_value("sb_diskline", txt)
    dpg.configure_item("sb_diskline", color=col)
    if dpg.does_item_exist("sb_th_bar_ok"):
        dpg.bind_item_theme("sb_diskbar",
                            "sb_th_bar_bad" if d["pct"] > 0.9 or d["projected"] < 0
                            else "sb_th_bar_warn" if d["pct"] > 0.75
                            or d["projected"] < d["min_free"]
                            else "sb_th_bar_ok")


def _row_tag(h, col):
    return "sb_c_%s_%s" % (h, col)


def update_rows(lst):
    for t in lst:
        h = t["hash"]
        if h not in G["rows"]:
            return False
        vals = _cells(t)
        for col, v in vals.items():
            tag = _row_tag(h, col)
            if dpg.does_item_exist(tag):
                dpg.set_value(tag, v)
        tag = _row_tag(h, "bar")
        if dpg.does_item_exist(tag):
            dpg.set_value(tag, t["progress"])
        tag = _row_tag(h, "state")
        if dpg.does_item_exist(tag):
            dpg.configure_item(tag, color=ST_COL.get(t["state"], (200, 200, 200)))
        tag = _row_tag(h, "tip")
        if dpg.does_item_exist(tag):
            dpg.set_value(tag, name_tooltip_text(t, h))
        col = EM_COL.get(t.get("emby_state"))
        pcol = EM_COL.get(present_state(t))
        for c, cc, dft in (("emby", col, (150, 150, 175)),
                           ("eq", col, (150, 150, 175)),
                           ("local", EM_COL.get(t.get("local_state")),
                            (150, 150, 175)),
                           ("name", pcol, (228, 230, 236))):
            tag = _row_tag(h, c)
            if dpg.does_item_exist(tag):
                dpg.configure_item(tag, color=cc or dft)
    return True


def ensure_table():
    """Le tableau est cree UNE fois et conserve : ImGui garde ainsi l'etat de
    tri (colonne active et fleche). Seules les lignes sont reconstruites."""
    if dpg.does_item_exist("sb_table"):
        return
    dpg.delete_item("sb_table_area", children_only=True)
    dpg.add_text("", tag="sb_empty", parent="sb_table_area",
                 color=(150, 150, 175), show=False)
    G["colmap"] = {}
    with dpg.table(tag="sb_table", parent="sb_table_area", header_row=True,
                   resizable=True, sortable=True, hideable=True,
                   reorderable=True, callback=lambda s, a, u: on_sort(s, a),
                   borders_innerH=True, borders_innerV=True, row_background=True,
                   policy=dpg.mvTable_SizingFixedFit, scrollX=True, scrollY=True,
                   height=-1, freeze_rows=1):
        marked = False
        vue = CFG.get("vue", "Tout")
        for key, label, w, groupe in _COLS:
            if not col_visible(groupe, vue):
                continue
            first = key == CFG.get("sort_key", "state") and not marked
            cid = dpg.add_table_column(
                label=label, init_width_or_weight=w, no_sort=(key is None),
                no_hide=(key is None),
                default_sort=first,
                prefer_sort_descending=(first and int(CFG.get("sort_dir", 1)) < 0))
            if first:
                marked = True
            if key:
                G["colmap"][cid] = key


def name_tooltip_text(t, h):
    """Fiche complete d'un torrent, montree au survol de son nom.

    Volontairement autonome : en vue Transfert les colonnes de comparaison
    sont masquees, cette infobulle reste alors le seul acces aux details
    Emby et disque.
    """
    L = [t["name"], ""]
    L.append("SEEDBOX   %s  -  %s"
             % (TIER_LBL[tier_from_name(t["name"])], fmt_size(t["size"])))
    L.append("   %s" % (t["save_path"] or "(dossier inconnu)"))
    L.append("   hash %s   -   etat client : %s" % (h, t["raw_state"] or "?"))
    if t["msg"]:
        L.append("   message : %s" % t["msg"])

    L.append("")
    e = t.get("emby")
    if not G.get("emby_idx"):
        L.append("EMBY      bibliotheque non chargee")
    elif not e:
        L.append("EMBY      aucun film correspondant")
    else:
        L.append("EMBY      %s (%s)" % (e["name"], e["year"] or "?"))
        L.append("   fichier : %s" % (e["fname"] or "(nom inconnu)"))
        L.append("   %s  -  %s        [%s]"
                 % (e["res"], fmt_size(e["size"]), EM_LBL[t["emby_state"]]))
        L.append("   %s" % (e["path"] or "(chemin non renseigne)"))
        d = _size_delta(t)
        L.append("   rapprochement : %s%s"
                 % (e["method"],
                    "   -   ecart de taille %s" % _diff_cell(t)
                    if d is not None else ""))

    L.append("")
    le = t.get("local")
    if not G.get("local_idx"):
        L.append("DISQUE    aucun dossier analyse")
    elif not le:
        L.append("DISQUE    aucun fichier correspondant")
    else:
        L.append("DISQUE    %s" % Path(le["path"]).name)
        L.append("   %s  -  %s        [%s]"
                 % (le["res"], fmt_size(le["size"]),
                    EM_LBL_N[t["local_state"]]))
        L.append("   %s" % le["path"])
        L.append("   rapprochement : %s" % le["method"])
    return "\n".join(L)


def _row_cells(t, h, v, show):
    """Construit les cellules d'une ligne, en respectant la vue active.

    Les colonnes masquees ne sont pas creees du tout : update_rows teste
    l'existence de chaque tag, elle s'en accommode sans modification.
    """
    dpg.add_checkbox(default_value=h in G["sel"], user_data=h,
                     callback=lambda s, val, u: (
                         G["sel"].add(u) if val else G["sel"].discard(u)))
    # Une infobulle creee directement dans une table_row en devient un enfant
    # au meme titre qu'une cellule, ce qui decale toutes les colonnes suivantes.
    # On l'enferme donc dans un groupe : la ligne ne voit qu'un enfant par
    # colonne, quoi qu'il arrive.
    with dpg.group():
        dpg.add_text(v["name"], tag=_row_tag(h, "name"),
                     color=EM_COL.get(present_state(t), (228, 230, 236)))
        with dpg.tooltip(dpg.last_item()):
            dpg.add_text(name_tooltip_text(t, h), tag=_row_tag(h, "tip"),
                         wrap=700)
    dpg.add_text(v["state"], tag=_row_tag(h, "state"),
                 color=ST_COL.get(t["state"], (200, 200, 200)))
    if show["tr"]:
        dpg.add_progress_bar(default_value=t["progress"], width=-1,
                             tag=_row_tag(h, "bar"))
        dpg.add_text(v["pct"], tag=_row_tag(h, "pct"))
    dpg.add_text(v["size"], tag=_row_tag(h, "size"))
    if show["cmp"]:
        dpg.add_text(v["tq"], tag=_row_tag(h, "tq"))
        e = t.get("emby")
        with dpg.group():
            dpg.add_text(v["emby"], tag=_row_tag(h, "emby"),
                         color=EM_COL.get(t.get("emby_state"), (150, 150, 175)))
            if e:
                with dpg.tooltip(dpg.last_item()):
                    dpg.add_text("%s (%s)\n\nfichier Emby : %s\n%s\n\n"
                                 "Emby    : %s  -  %s\n"
                                 "Seedbox : %s  -  %s\n\nRapprochement : %s"
                                 % (e["name"], e["year"] or "?",
                                    e["fname"] or "?", e["path"],
                                    e["res"], fmt_size(e["size"]),
                                    TIER_LBL[tier_from_name(t["name"])],
                                    fmt_size(t["size"]), e["method"]), wrap=620)
        dpg.add_text(v["eq"], tag=_row_tag(h, "eq"),
                     color=EM_COL.get(t.get("emby_state"), (150, 150, 175)))
        dpg.add_text(v["esize"], tag=_row_tag(h, "esize"))
        d = _size_delta(t)
        dpg.add_text(v["ediff"], tag=_row_tag(h, "ediff"),
                     color=((46, 204, 113) if d is not None and abs(d) <= 2
                            else (235, 140, 20) if d is not None and abs(d) <= 20
                            else (200, 90, 100) if d is not None
                            else (150, 150, 175)))
        le = t.get("local")
        with dpg.group():
            dpg.add_text(v["local"], tag=_row_tag(h, "local"),
                         color=EM_COL.get(t.get("local_state"), (150, 150, 175)))
            if le:
                with dpg.tooltip(dpg.last_item()):
                    dpg.add_text("%s\n\n%s\n\nsur disque : %s  -  %s\n"
                                 "torrent    : %s  -  %s\n\nRapprochement : %s"
                                 % (Path(le["path"]).name, le["path"],
                                    le["res"], fmt_size(le["size"]),
                                    TIER_LBL[tier_from_name(t["name"])],
                                    fmt_size(t["size"]), le["method"]), wrap=620)
    if show["tr"]:
        dpg.add_text(v["dl"], tag=_row_tag(h, "dl"))
        dpg.add_text(v["ul"], tag=_row_tag(h, "ul"))
        dpg.add_text(v["eta"], tag=_row_tag(h, "eta"))
        dpg.add_text(v["ratio"], tag=_row_tag(h, "ratio"))
        dpg.add_text(v["peers"], tag=_row_tag(h, "peers"))
        dpg.add_text(v["cat"], tag=_row_tag(h, "cat"))
        dpg.add_text(v["tracker"], tag=_row_tag(h, "tracker"))


def render_dashboard(force=False):
    lst = visible_torrents()
    order = [t["hash"] for t in lst]
    # maj sur place tant que la composition et l'ordre ne bougent pas :
    # reconstruire 200 lignes toutes les 5 s ferait clignoter l'affichage.
    if not force and order == G["order"] and G["rows"]:
        if update_rows(lst):
            update_header()
            return
    G["order"] = order
    G["rows"] = {}
    ensure_table()
    dpg.delete_item("sb_table", children_only=True, slot=1)   # lignes seules
    update_header()

    msg = ""
    if not G["connected"]:
        msg = "Non connecte. Renseigne l'adresse de ton client puis Connecter."
    elif not lst:
        msg = ("Aucun torrent avec ces filtres." if G["torrents"]
               else "Aucun torrent sur la seedbox.")
    dpg.configure_item("sb_empty", show=bool(msg))
    dpg.set_value("sb_empty", msg)
    dpg.configure_item("sb_table", show=not msg)
    if msg:
        return

    vue = CFG.get("vue", "Tout")
    show = {g: col_visible(g, vue) for g in ("id", "cmp", "tr")}
    for t in lst:
        h = t["hash"]
        v = _cells(t)
        with dpg.table_row(parent="sb_table"):
            _row_cells(t, h, v, show)
        G["rows"][h] = True


# --- rafraichissement -------------------------------------------------
def _pull():
    c = G["client"]
    if not c:
        return
    with _busy:
        lst = c.list_torrents()
        free, total = c.disk_info()
    try:
        for t in lst:                  # statut Emby avant sauvegarde
            apply_emby_status(t, G.get("emby_idx"))
        snapshot_maybe(lst)
    except Exception as exc:
        log("sauvegarde : %s" % exc)   # ne doit jamais casser le rafraichissement

    def apply():
        G["torrents"] = lst
        G["free"] = free
        G["total"] = total
        refresh_emby_status()
        live = {t["hash"] for t in lst}
        G["sel"] &= live          # un torrent supprime ne reste pas selectionne
        render_dashboard()
    ui(apply)


def refresh_now(sender=None, app_data=None, user_data=None):
    if not G["client"]:
        return

    def worker():
        try:
            _pull()
        except Exception as exc:
            msg = str(exc)
            ui(lambda: set_status("Rafraichissement impossible : %s" % msg,
                                  (215, 75, 90)))
    threading.Thread(target=worker, daemon=True).start()


def _refresh_loop():
    fails = 0
    while not _stop_refresh.is_set():
        delay = max(2, int(CFG.get("refresh", 5)))
        _stop_refresh.wait(delay)
        if _stop_refresh.is_set():
            break
        if not G["client"] or not CFG.get("auto_refresh", True):
            continue
        try:
            _pull()
            if fails:
                fails = 0
                ui(lambda: set_status("Connexion retablie.", (46, 204, 113)))
        except Exception as exc:
            fails += 1
            msg = str(exc)
            if fails in (1, 5, 20):     # on n'inonde pas le journal
                ui(lambda m=msg, f=fails: set_status(
                    "Perte de contact (%d essais) : %s" % (f, m), (235, 140, 20)))


def start_refresh_thread():
    if G.get("refresh_thread"):
        return
    _stop_refresh.clear()
    t = threading.Thread(target=_refresh_loop, daemon=True)
    G["refresh_thread"] = t
    t.start()


# --- actions sur la selection ----------------------------------------
def _sel_or_warn():
    sel = sorted(G["sel"])
    if not sel:
        modal("Aucune selection", "Coche au moins un torrent dans le tableau.")
        return None
    if not G["client"]:
        modal("Non connecte", "Connecte-toi d'abord.")
        return None
    return sel


def _run_action(fn, label, sel):
    def worker():
        try:
            with _busy:
                fn(sel)
            ui(lambda: (add_log("%s : %d torrent(s)" % (label, len(sel)),
                                (46, 204, 113)),
                        set_status("%s applique." % label, (46, 204, 113))))
            time.sleep(0.6)
            _pull()
        except Exception as exc:
            msg = str(exc)
            ui(lambda: (add_log("ECHEC %s : %s" % (label, msg), (215, 75, 90)),
                        modal("Action impossible", msg)))
    threading.Thread(target=worker, daemon=True).start()


def act_pause(sender=None, app_data=None, user_data=None):
    sel = _sel_or_warn()
    if sel:
        _run_action(G["client"].pause, "Mise en pause", sel)


def act_resume(sender=None, app_data=None, user_data=None):
    sel = _sel_or_warn()
    if sel:
        _run_action(G["client"].resume, "Reprise", sel)


def act_recheck(sender=None, app_data=None, user_data=None):
    sel = _sel_or_warn()
    if sel:
        _run_action(G["client"].recheck, "Verification", sel)


def act_delete(sender=None, app_data=None, user_data=None):
    global _mid
    sel = _sel_or_warn()
    if not sel:
        return
    picked = [t for t in G["torrents"] if t["hash"] in sel]
    on_emby = [t for t in picked if t.get("emby_state") in (EM_SAME, EM_OK)]
    better = [t for t in picked if t.get("emby_state") == EM_BETTER]
    absent = [t for t in picked if t.get("emby_state") is None]
    seeding = [t for t in picked if t["state"] == "seeding"]
    low_ratio = [t for t in seeding if t["ratio"] < 1.0]
    freed = sum(t["downloaded"] for t in picked)
    _mid += 1
    tag = "sb_del_%d" % _mid
    with dpg.window(label="Retirer de la seedbox", tag=tag, modal=True,
                    width=720, autosize=True, pos=[160, 150]):
        dpg.add_text("Retirer %d torrent(s) du client ?  Place liberee sur la "
                     "seedbox si tu supprimes aussi les fichiers : %s"
                     % (len(sel), fmt_size(freed)), wrap=690)
        dpg.add_separator()
        if G.get("emby_idx"):
            if on_emby:
                dpg.add_text("%d deja presents sur Emby : suppression sans "
                             "risque." % len(on_emby), color=(46, 204, 113),
                             wrap=690)
            if better:
                dpg.add_text("ATTENTION : %d torrent(s) sont de MEILLEURE "
                             "qualite que la version presente sur Emby. Les "
                             "supprimer te ferait perdre l'upgrade."
                             % len(better), color=(235, 140, 20), wrap=690)
                for t in better[:5]:
                    e = t["emby"]
                    dpg.add_text("   %s  -  seedbox %s / Emby %s"
                                 % (t["name"][:52],
                                    TIER_LBL[tier_from_name(t["name"])],
                                    e["res"]), color=(235, 140, 20))
            if absent:
                dpg.add_text("ATTENTION : %d torrent(s) ne sont PAS sur Emby. "
                             "Rien ne les sauvegarde ailleurs." % len(absent),
                             color=(215, 75, 90), wrap=690)
                for t in absent[:5]:
                    dpg.add_text("   %s" % t["name"][:66], color=(215, 75, 90))
        else:
            dpg.add_text("Emby n'est pas charge : impossible de dire si ces "
                         "films existent ailleurs.", color=(150, 150, 175),
                         wrap=690)
        if seeding:
            dpg.add_text("%d sont en seed%s. Les retirer arrete le partage."
                         % (len(seeding),
                            ", dont %d sous un ratio de 1.00" % len(low_ratio)
                            if low_ratio else ""),
                         color=(235, 140, 20) if low_ratio else (150, 150, 200),
                         wrap=690)
        dpg.add_separator()
        with dpg.child_window(height=110, border=True):
            for t in picked[:40]:
                dpg.add_text("  %s" % t["name"][:88],
                             color=EM_COL.get(t.get("emby_state"),
                                              (200, 200, 215)))
            if len(picked) > 40:
                dpg.add_text("  ... et %d autre(s)" % (len(picked) - 40),
                             color=(150, 150, 175))
        dpg.add_separator()
        dpg.add_checkbox(label="Supprimer AUSSI les fichiers telecharges sur "
                         "la seedbox", tag="sb_delfiles", default_value=False)
        dpg.add_text("Sans cette case, seule l'entree du client est retiree : "
                     "les fichiers restent sur le disque de la seedbox.",
                     wrap=690, color=(150, 150, 175))
        dpg.add_separator()
        with dpg.group(horizontal=True):
            b = dpg.add_button(label="Retirer", width=340, user_data=(sel, tag),
                               callback=lambda s, a, u: _do_delete(u[0], u[1]))
            dpg.bind_item_theme(b, "sb_th_danger")
            dpg.add_button(label="Annuler", width=340, user_data=tag,
                           callback=lambda s, a, u: dpg.delete_item(u))


def _do_delete(sel, tag):
    with_data = bool(gv("sb_delfiles", False))
    dpg.delete_item(tag)
    G["sel"] = set()
    _run_action(lambda hs: G["client"].delete(hs, with_data),
                "Suppression%s" % (" + donnees" if with_data else ""), sel)


# ---------------------------------------------------------------------
#  REINITIALISATION : effacer les donnees et retelecharger de zero
# ---------------------------------------------------------------------
def _reset_plan(t):
    """Comment ce torrent sera re-ajoute apres effacement. -> (mode, donnees)"""
    data = find_local_torrent(t["hash"])
    if data:
        return "fichier .torrent local", data
    data = G["client"].export_torrent(t["hash"])
    if data:
        return "export du client", data
    if t["hash"]:
        return "magnet", None
    return "impossible", None


def act_reset(sender=None, app_data=None, user_data=None):
    sel = _sel_or_warn()
    if not sel:
        return
    if not G["client"].can_delete_data():
        modal("Reinitialisation indisponible",
              "rTorrent n'a aucune commande pour effacer les fichiers "
              "telecharges, et le greffon erasedata de ruTorrent ne repond "
              "pas.\n\nSans effacement des donnees, une reinitialisation ne "
              "servirait a rien : le client revérifierait et retrouverait tout "
              "complet. Supprime les fichiers depuis ruTorrent, puis utilise "
              "Verifier.", wide=680)
        return
    picked = [t for t in G["torrents"] if t["hash"] in sel]
    set_status("Preparation de la reinitialisation...", (235, 140, 20))

    def worker():
        plans = []
        for t in picked:
            mode, data = _reset_plan(t)
            plans.append((t, mode, data))
        ui(lambda: _ask_reset(plans))

    threading.Thread(target=worker, daemon=True).start()


def _ask_reset(plans):
    global _mid
    _mid += 1
    tag = "sb_reset_%d" % _mid
    perdu = sum(t["downloaded"] for t, _, _ in plans)
    magnets = [p for p in plans if p[1] == "magnet"]
    impossibles = [p for p in plans if p[1] == "impossible"]
    seeds = [t for t, _, _ in plans if t["state"] == "seeding"]
    with dpg.window(label="Reinitialiser et retelecharger", tag=tag, modal=True,
                    width=800, autosize=True, pos=[150, 130]):
        dpg.add_text("%d torrent(s) vont etre effaces de la seedbox PUIS "
                     "re-ajoutes a zero." % len(plans), wrap=770)
        dpg.add_text("%s de donnees deja telechargees seront perdues et "
                     "retelechargees." % fmt_size(perdu), wrap=770,
                     color=(235, 140, 20))
        if seeds:
            dpg.add_text("%d sont en seed : le partage s'arrete et le ratio "
                         "de ces torrents repart de zero." % len(seeds),
                         wrap=770, color=(235, 140, 20))
        dpg.add_separator()
        with dpg.child_window(height=min(240, 40 + 22 * len(plans)), border=True):
            for t, mode, _d in plans:
                col = ((215, 75, 90) if mode == "impossible"
                       else (235, 140, 20) if mode == "magnet"
                       else (46, 204, 113))
                dpg.add_text("  %-58s  %s  ->  re-ajout par %s"
                             % (t["name"][:58], fmt_size(t["downloaded"]), mode),
                             color=col)
        if magnets:
            dpg.add_text("%d seront re-ajoutes par magnet : sur un tracker "
                         "prive sans DHT, la recuperation des metadonnees peut "
                         "echouer. Depose le .torrent d'origine dans ton "
                         "dossier surveille pour que je le reutilise tel quel."
                         % len(magnets), wrap=770, color=(235, 140, 20))
        if impossibles:
            dpg.add_text("%d n'ont ni fichier .torrent ni infohash exploitable "
                         "et seront ignores." % len(impossibles), wrap=770,
                         color=(215, 75, 90))
        dpg.add_separator()
        with dpg.group(horizontal=True):
            b = dpg.add_button(label="Effacer et retelecharger", width=380,
                               user_data=(plans, tag),
                               callback=lambda s, a, u: (dpg.delete_item(u[1]),
                                                         _do_reset(u[0])))
            dpg.bind_item_theme(b, "sb_th_danger")
            dpg.add_button(label="Annuler", width=380, user_data=tag,
                           callback=lambda s, a, u: dpg.delete_item(u))


def _do_reset(plans):
    c = G["client"]
    G["sel"] = set()

    def worker():
        ok, ko = 0, 0
        for t, mode, data in plans:
            if mode == "impossible":
                ko += 1
                continue
            name, h = t["name"], t["hash"]
            sp, cat = t["save_path"], t["category"]
            try:
                with _busy:
                    c.delete([h], with_data=True)
                time.sleep(1.0)          # laisser le client liberer les fichiers
                with _busy:
                    if data:
                        c.add_file(data, (name[:60] or h) + ".torrent",
                                   cat, sp, False)
                    else:
                        uri = "magnet:?xt=urn:btih:%s&dn=%s" % (
                            h, urllib.parse.quote(name))
                        if t.get("tracker"):
                            uri += "&tr=" + urllib.parse.quote(
                                "https://%s/announce" % t["tracker"])
                        c.add_magnet(uri, cat, sp, False)
                ok += 1
                ui(lambda n=name, m=mode: add_log(
                    "Reinitialise (%s) : %s" % (m, n[:70]), (46, 204, 113)))
            except Exception as exc:
                ko += 1
                ui(lambda n=name, e=str(exc): add_log(
                    "ECHEC reinitialisation : %s  ->  %s" % (n[:60], e),
                    (215, 75, 90)))
        ui(lambda: set_status("Reinitialisation : %d reussie(s), %d echec(s)."
                              % (ok, ko),
                              (46, 204, 113) if not ko else (235, 140, 20)))
        time.sleep(1.0)
        _pull()

    threading.Thread(target=worker, daemon=True).start()


def sel_all(sender=None, app_data=None, user_data=None):
    G["sel"] = {t["hash"] for t in visible_torrents()}
    render_dashboard(force=True)


def sel_emby(sender=None, app_data=None, user_data=None):
    """Coche les torrents deja presents (Emby ou dossiers) en qualite au moins
    equivalente. Ceux ou la seedbox a mieux sont volontairement exclus."""
    if not G.get("emby_idx") and not G.get("local_idx"):
        modal("Aucune source chargee",
              "Charge la bibliotheque Emby ou analyse des dossiers avant de "
              "faire le tri.")
        return
    G["sel"] = {t["hash"] for t in visible_torrents()
                if present_state(t) in (EM_SAME, EM_OK)}
    better = sum(1 for t in visible_torrents()
                 if present_state(t) == EM_BETTER)
    render_dashboard(force=True)
    msg = "%d torrent(s) coche(s) : deja presents." % len(G["sel"])
    if better:
        msg += (" %d exclu(s) car la seedbox a une meilleure qualite que ce "
                "que tu possedes." % better)
    set_status(msg, (46, 204, 113))


def sel_none(sender=None, app_data=None, user_data=None):
    G["sel"] = set()
    render_dashboard(force=True)


# =====================================================================
#  ONGLET INVENTAIRE
# =====================================================================
def inv_rows():
    inv = load_inventory()
    ft = (gv("sb_invfilter") or "").lower().strip()
    mode = gv("sb_invmode") or "Tout"
    out = []
    for r in inv.values():
        if mode == "Presents" and not r.get("present"):
            continue
        if mode == "Disparus" and r.get("present"):
            continue
        if ft and ft not in (r.get("name", "") + r.get("category", "")).lower():
            continue
        out.append(r)
    out.sort(key=lambda r: (r.get("present", False), r.get("last_seen", "")),
             reverse=True)
    return out


def render_inventory(sender=None, app_data=None, user_data=None):
    if not dpg.does_item_exist("sb_inv_area"):
        return
    dpg.delete_item("sb_inv_area", children_only=True)
    inv = load_inventory()
    presents = sum(1 for r in inv.values() if r.get("present"))
    disparus = len(inv) - presents
    taille = sum(r.get("size", 0) for r in inv.values() if not r.get("present"))
    dpg.set_value("sb_invlbl",
                  "%d torrent(s) connus  -  %d presents  -  %d disparus (%s)"
                  % (len(inv), presents, disparus, fmt_size(taille)))
    rows = inv_rows()
    if not rows:
        dpg.add_text("Aucun enregistrement. La sauvegarde se declenche au "
                     "premier rafraichissement apres connexion.",
                     parent="sb_inv_area", color=(150, 150, 175))
        return
    with dpg.table(parent="sb_inv_area", header_row=True, resizable=True,
                   borders_innerH=True, borders_innerV=True, row_background=True,
                   policy=dpg.mvTable_SizingFixedFit, scrollX=True, scrollY=True,
                   height=-1, freeze_rows=1):
        for lbl, w in (("Etat", 90), ("Nom", 420), ("Taille", 90),
                       ("Categorie", 100), ("Ratio", 60), ("Vu la 1re fois", 140),
                       ("Vu la derniere fois", 150), ("Fichier sur Emby", 300),
                       ("Dossier", 220)):
            dpg.add_table_column(label=lbl, init_width_or_weight=w)
        for r in rows[:2000]:
            with dpg.table_row():
                pres = r.get("present")
                dpg.add_text("present" if pres else "DISPARU",
                             color=(46, 204, 113) if pres else (235, 140, 20))
                with dpg.group():
                    dpg.add_text(r.get("name", "")[:88])
                    with dpg.tooltip(dpg.last_item()):
                        dpg.add_text("%s\n\ninfohash %s\ntracker %s"
                                     % (r.get("name", ""), r.get("hash", ""),
                                        r.get("tracker", "")), wrap=620)
                dpg.add_text(fmt_size(r.get("size", 0)))
                dpg.add_text((r.get("category") or "-")[:16])
                dpg.add_text("%.2f" % (r.get("ratio", 0) or 0))
                dpg.add_text((r.get("first_seen") or "")[:16])
                dpg.add_text((r.get("last_seen") or "")[:16])
                dpg.add_text((r.get("emby_name") or "-")[:60],
                             color=(46, 204, 113) if r.get("emby_name")
                             else (150, 150, 175))
                dpg.add_text((r.get("save_path") or "-")[:40])


def snap_now(sender=None, app_data=None, user_data=None):
    if not G.get("torrents"):
        modal("Rien a sauvegarder",
              "Connecte-toi a la seedbox : la liste est lue sur le client.")
        return

    def worker():
        res = write_snapshot(list(G["torrents"]), force=True)
        ui(lambda: (set_status("Liste sauvegardee : %d present(s), %d disparu(s) "
                               "en inventaire." % res if res
                               else "Sauvegarde impossible.",
                               (46, 204, 113) if res else (215, 75, 90)),
                    render_inventory()))
    threading.Thread(target=worker, daemon=True).start()


def export_inventory(sender=None, app_data=None, user_data=None):
    inv = load_inventory()
    if not inv:
        modal("Inventaire vide", "Aucun torrent enregistre pour l'instant.")
        return
    try:
        import tkinter as tk
        from tkinter import filedialog
        r = tk.Tk()
        r.withdraw()
        r.attributes("-topmost", True)
        fp = filedialog.asksaveasfilename(
            title="Exporter l'inventaire", defaultextension=".csv",
            initialfile="seedbox_inventaire_%s.csv" % time.strftime("%Y%m%d"),
            filetypes=[("CSV", "*.csv"), ("Tous", "*.*")])
        r.destroy()
    except Exception:
        fp = str(APP_DIR / "seedbox_inventaire.csv")
    if not fp:
        return
    try:
        with open(fp, "w", newline="", encoding="utf-8-sig") as f:
            w = csv.DictWriter(f, fieldnames=SNAP_FIELDS, delimiter=";",
                               extrasaction="ignore")
            w.writeheader()
            for r in inv_rows():
                w.writerow(r)
        set_status("Inventaire exporte : %s" % fp, (46, 204, 113))
    except Exception as exc:
        modal("Export impossible", str(exc))


def open_snap_dir(sender=None, app_data=None, user_data=None):
    d = str(snap_dir())
    try:
        os.makedirs(d, exist_ok=True)
        if sys.platform == "win32":
            os.startfile(d)
        else:
            subprocess.Popen(["xdg-open", d])
    except Exception as exc:
        modal("Ouverture impossible", str(exc))


# =====================================================================
#  THEME / POLICE
# =====================================================================
def setup_theme():
    ACCENT = (233, 69, 96)
    BLUE = (15, 52, 96)
    BLUE_H = (26, 80, 144)
    with dpg.theme() as th:
        with dpg.theme_component(dpg.mvAll):
            for k, v in ((dpg.mvThemeCol_WindowBg, (26, 26, 46)),
                         (dpg.mvThemeCol_ChildBg, (22, 33, 62)),
                         (dpg.mvThemeCol_PopupBg, (24, 35, 66)),
                         (dpg.mvThemeCol_FrameBg, (13, 27, 42)),
                         (dpg.mvThemeCol_FrameBgHovered, (20, 40, 60)),
                         (dpg.mvThemeCol_FrameBgActive, (26, 52, 78)),
                         (dpg.mvThemeCol_Button, BLUE),
                         (dpg.mvThemeCol_ButtonHovered, BLUE_H),
                         (dpg.mvThemeCol_ButtonActive, ACCENT),
                         (dpg.mvThemeCol_Header, BLUE),
                         (dpg.mvThemeCol_HeaderHovered, BLUE_H),
                         (dpg.mvThemeCol_HeaderActive, (30, 90, 160)),
                         (dpg.mvThemeCol_TableHeaderBg, BLUE),
                         (dpg.mvThemeCol_TableRowBg, (30, 42, 58)),
                         (dpg.mvThemeCol_TableRowBgAlt, (25, 34, 50)),
                         (dpg.mvThemeCol_TableBorderLight, (40, 58, 84)),
                         (dpg.mvThemeCol_TableBorderStrong, (50, 72, 104)),
                         (dpg.mvThemeCol_Text, (228, 230, 236)),
                         (dpg.mvThemeCol_TextDisabled, (136, 136, 170)),
                         (dpg.mvThemeCol_TitleBg, BLUE),
                         (dpg.mvThemeCol_TitleBgActive, (20, 66, 120)),
                         (dpg.mvThemeCol_ScrollbarBg, (16, 24, 40)),
                         (dpg.mvThemeCol_ScrollbarGrab, (40, 60, 92)),
                         (dpg.mvThemeCol_CheckMark, (255, 110, 130)),
                         (dpg.mvThemeCol_SliderGrab, ACCENT),
                         (dpg.mvThemeCol_Border, (38, 56, 84)),
                         (dpg.mvThemeCol_Separator, (38, 56, 84)),
                         (dpg.mvThemeCol_Tab, (24, 38, 68)),
                         (dpg.mvThemeCol_TabHovered, BLUE_H),
                         (dpg.mvThemeCol_TabActive, (176, 46, 72)),
                         (dpg.mvThemeCol_PlotHistogram, (46, 204, 113))):
                dpg.add_theme_color(k, v)
            dpg.add_theme_style(dpg.mvStyleVar_FrameRounding, 5)
            dpg.add_theme_style(dpg.mvStyleVar_WindowRounding, 7)
            dpg.add_theme_style(dpg.mvStyleVar_ChildRounding, 6)
            dpg.add_theme_style(dpg.mvStyleVar_TabRounding, 5)
            dpg.add_theme_style(dpg.mvStyleVar_FramePadding, 7, 5)
            dpg.add_theme_style(dpg.mvStyleVar_ItemSpacing, 8, 6)
            dpg.add_theme_style(dpg.mvStyleVar_CellPadding, 6, 3)
    dpg.bind_theme(th)
    with dpg.theme(tag="sb_th_ok"):
        with dpg.theme_component(dpg.mvButton):
            dpg.add_theme_color(dpg.mvThemeCol_Button, (0, 150, 160))
            dpg.add_theme_color(dpg.mvThemeCol_ButtonHovered, (10, 182, 194))
            dpg.add_theme_color(dpg.mvThemeCol_Text, (10, 20, 22))
            dpg.add_theme_style(dpg.mvStyleVar_FramePadding, 12, 6)
    with dpg.theme(tag="sb_th_go"):
        with dpg.theme_component(dpg.mvButton):
            dpg.add_theme_color(dpg.mvThemeCol_Button, (235, 130, 18))
            dpg.add_theme_color(dpg.mvThemeCol_ButtonHovered, (252, 162, 48))
            dpg.add_theme_color(dpg.mvThemeCol_Text, (20, 18, 10))
            dpg.add_theme_style(dpg.mvStyleVar_FramePadding, 12, 6)
    for tag, col in (("sb_th_bar_ok", (46, 204, 113)),
                     ("sb_th_bar_warn", (235, 140, 20)),
                     ("sb_th_bar_bad", (215, 75, 90))):
        with dpg.theme(tag=tag):
            with dpg.theme_component(dpg.mvProgressBar):
                dpg.add_theme_color(dpg.mvThemeCol_PlotHistogram, col)
                dpg.add_theme_color(dpg.mvThemeCol_FrameBg, (18, 28, 46))
    with dpg.theme(tag="sb_th_danger"):
        with dpg.theme_component(dpg.mvButton):
            dpg.add_theme_color(dpg.mvThemeCol_Button, (150, 40, 55))
            dpg.add_theme_color(dpg.mvThemeCol_ButtonHovered, (190, 55, 72))


def build_font():
    cands = [r"C:\Windows\Fonts\segoeui.ttf", r"C:\Windows\Fonts\arial.ttf",
             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
             "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
             "/System/Library/Fonts/Supplemental/Arial.ttf"]
    path = next((p for p in cands if os.path.exists(p)), None)
    if not path:
        return
    try:
        with dpg.font_registry():
            with dpg.font(path, FONT_SIZE) as f:
                dpg.add_font_range_hint(dpg.mvFontRangeHint_Default)
                dpg.add_font_range(0x0020, 0x017F)
                dpg.add_font_range(0x2000, 0x206F)
        dpg.bind_font(f)
    except Exception:
        pass


# =====================================================================
#  VERIFICATION DEPUIS LE NAVIGATEUR
# =====================================================================
# Un petit serveur HTTP local (127.0.0.1 uniquement) repond a un script
# Tampermonkey / Violentmonkey : en survolant un nom de film sur une page,
# ou en ouvrant une fiche (IMDb, TMDB, tracker...), le navigateur demande
# "ai-je deja ce film ?" et affiche les differences visibles avec ce qui est
# deja sur Emby ou sur le disque (VFQ au lieu de VFF, BLURAY au lieu de
# WEB-DL, 1080p au lieu de 4K...).
#
# Securite : ecoute sur la boucle locale seulement, en-tete Host verifie
# (parade au DNS rebinding), jeton secret exige, et aucune en-tete CORS :
# une page web ordinaire ne peut pas lire les reponses, seul le script
# utilisateur (GM_xmlhttpRequest) le peut.
USERSCRIPT_NAME = "emby_checker.user.js"
_SRV = {"httpd": None, "port": 0, "hits": 0}


def browser_token():
    tok = str(CFG.get("browser_token") or "")
    if len(tok) < 16:
        tok = CFG["browser_token"] = secrets.token_hex(16)
        save_cfg(CFG)
    return tok


def _versions_of(e, idx):
    """Toutes les versions du meme film (Emby : une entree par fichier)."""
    out = [x for x in idx["entries"]
           if x["name"] == e["name"] and x["year"] == e["year"]
           and x.get("norm") == e.get("norm")]
    return out or [e]


def _version_json(x, page_attrs, page_size=0):
    a = x.get("attrs") or release_attrs(x.get("path") or x.get("name", ""))
    diffs = attrs_diff(page_attrs, a)
    same_size = bool(page_size and x["size"] and
                     abs(page_size - x["size"]) * 100.0
                     / max(page_size, x["size"]) <= 2.0)
    return {"name": x["name"], "year": x["year"],
            "file": Path(x["path"]).name if x.get("path") else "",
            "path": x.get("path", ""), "size": x["size"],
            "size_txt": fmt_size(x["size"]), "attrs": a, "diffs": diffs,
            "same_size": same_size}


def _lookup_in(idx, q, page_attrs, imdb="", tmdb="", page_size=0):
    """-> (versions, methode) pour un index (Emby ou dossiers locaux)."""
    if not idx:
        return [], ""
    hits = []
    for key in (("imdb:" + imdb) if imdb else "", ("tmdb:" + tmdb) if tmdb else ""):
        if key and idx.get("by_id", {}).get(key):
            hits = [idx["entries"][i] for i in idx["by_id"][key]]
            return [_version_json(x, page_attrs, page_size) for x in hits], "ID"
    if not q:
        return [], ""
    e, score, method = match_torrent_to_emby({"name": q, "size": page_size}, idx,
                                             int(CFG.get("emby_fuzzy", 80)))
    if not e:
        return [], ""
    return [_version_json(x, page_attrs, page_size)
            for x in _versions_of(e, idx)], method


def _summary(versions, page_attrs):
    """Verdict global, du point de vue de ce qu'on regarde dans la page."""
    if not versions:
        return "absent"
    if any(v["same_size"] for v in versions):
        return "identique"
    if not any(page_attrs.get(k) for k in ("lang", "source", "res")):
        return "present"        # simple titre ou fiche IMDb : rien a comparer
    if any(not v["diffs"] for v in versions):
        return "identique"
    # La page n'apporte un plus que si elle bat TOUTES les versions possedees
    # sur la resolution ou la source.
    def better(v):
        c = {d["key"]: d["cmp"] for d in v["diffs"]}
        return c.get("res") == "mieux" or (c.get("res") != "moins" and
                                           c.get("source") == "mieux")
    if page_attrs.get("res") or page_attrs.get("source"):
        if all(better(v) for v in versions):
            return "mieux"
    return "present"


def browser_lookup(q="", imdb="", tmdb="", size=0):
    """Coeur de la verification : utilisable aussi sans navigateur."""
    q = re.sub(r"\s+", " ", str(q or "")).strip()[:400]
    imdb = (re.search(r"tt\d{5,10}", str(imdb or "").lower()) or [""])[0]
    tmdb = re.sub(r"\D", "", str(tmdb or ""))[:12]
    page = release_attrs(q) if q else release_attrs("")
    nt, ny = split_title_year(q) if q else ("", None)
    eidx, lidx = G.get("emby_idx"), G.get("local_idx")
    ev, em = _lookup_in(eidx, q, page, imdb, tmdb, size)
    lv, lm = _lookup_in(lidx, q, page, imdb, tmdb, size)
    _SRV["hits"] += 1
    return {"ok": True, "query": q, "title": nt, "year": ny,
            "page": page, "emby_loaded": bool(eidx),
            "local_loaded": bool(lidx),
            "emby": {"status": _summary(ev, page) if eidx else "inconnu",
                     "method": em, "versions": ev},
            "local": {"status": _summary(lv, page) if lidx else "inconnu",
                      "method": lm, "versions": lv}}


def lookup_text(res):
    """Version texte d'une reponse, pour l'essai dans l'application."""
    st_lbl = {"absent": "ABSENT", "identique": "DEJA PRESENT (identique)",
              "present": "DEJA PRESENT", "mieux": "DEJA PRESENT, la page a mieux",
              "inconnu": "non charge"}
    p = res["page"]
    L = ["Recherche : %s (%s)" % (res["title"] or "?", res["year"] or "annee ?"),
         "Page : " + ("  ".join("%s %s" % (lbl, p[k]) for k, lbl in ATTR_LABELS
                                if p.get(k)) or "(aucune caracteristique lisible)")]
    for src, lbl in (("emby", "EMBY"), ("local", "DISQUE")):
        r = res[src]
        L += ["", "%-7s %s%s" % (lbl, st_lbl.get(r["status"], r["status"]),
                                 "   [%s]" % r["method"] if r["method"] else "")]
        for v in r["versions"]:
            a = v["attrs"]
            L.append("  - %s  (%s)" % (v["file"] or v["name"], v["size_txt"]))
            L.append("      " + "  ".join(a[k] for k, _ in ATTR_LABELS
                                         if a.get(k) and k != "group"))
            for d in v["diffs"]:
                L.append("      %-10s page %s  /  possede %s%s"
                         % (d["label"], d["page"], d["have"],
                            {"mieux": "   (page mieux)",
                             "moins": "   (page moins bien)"}.get(d["cmp"], "")))
    return "\n".join(L)


class _CheckHandler(BaseHTTPRequestHandler):
    server_version = "EmbyCheck/1"

    def log_message(self, fmt, *args):
        pass                                   # pas de bruit dans la console

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        data = body if isinstance(body, bytes) else body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _json(self, code, obj):
        self._send(code, json.dumps(obj, ensure_ascii=False))

    def do_GET(self):
        try:
            host = (self.headers.get("Host") or "").split(":")[0].lower()
            if host not in ("127.0.0.1", "localhost"):
                return self._json(403, {"ok": False, "error": "host"})
            u = urllib.parse.urlsplit(self.path)
            qs = dict(urllib.parse.parse_qsl(u.query))
            if u.path == "/" + USERSCRIPT_NAME:
                return self._send(200, userscript_source(),
                                  "text/javascript; charset=utf-8")
            tok = self.headers.get("X-Emby-Check-Token") or qs.get("token", "")
            if not secrets.compare_digest(str(tok), browser_token()):
                return self._json(401, {"ok": False, "error": "jeton"})
            if u.path == "/ping":
                ei, li = G.get("emby_idx"), G.get("local_idx")
                return self._json(200, {
                    "ok": True, "emby": len(ei["entries"]) if ei else 0,
                    "local": len(li["entries"]) if li else 0})
            if u.path == "/check":
                try:
                    size = int(qs.get("size", 0) or 0)
                except ValueError:
                    size = 0
                return self._json(200, browser_lookup(
                    qs.get("q", ""), qs.get("imdb", ""), qs.get("tmdb", ""), size))
            return self._json(404, {"ok": False, "error": "route"})
        except Exception as exc:
            log("serveur navigateur: %s" % traceback.format_exc())
            try:
                self._json(500, {"ok": False, "error": str(exc)})
            except Exception:
                pass


def start_browser_server(port=None):
    """Demarre le serveur local. -> (ok, message)."""
    stop_browser_server()
    port = int(port or CFG.get("browser_port") or 8765)
    try:
        httpd = ThreadingHTTPServer(("127.0.0.1", port), _CheckHandler)
    except OSError as exc:
        return False, ("Port %d indisponible (%s). Choisis un autre port."
                       % (port, exc))
    httpd.daemon_threads = True
    _SRV.update(httpd=httpd, port=port)
    threading.Thread(target=httpd.serve_forever, daemon=True,
                     name="emby-check").start()
    log("serveur navigateur actif sur 127.0.0.1:%d" % port)
    return True, "Actif sur 127.0.0.1:%d" % port


def stop_browser_server():
    httpd = _SRV.get("httpd")
    _SRV["httpd"] = None
    if httpd:
        threading.Thread(target=lambda: (httpd.shutdown(), httpd.server_close()),
                         daemon=True).start()


def userscript_source():
    return (_USERSCRIPT.replace("__PORT__", str(_SRV.get("port") or
                                                CFG.get("browser_port") or 8765))
            .replace("__TOKEN__", browser_token()))


_USERSCRIPT = r"""// ==UserScript==
// @name         Emby Checker (Seedbox Dashboard)
// @namespace    emby-toolbox
// @version      1.0
// @description  Survol d'un nom de film ou ouverture d'une fiche : deja sur Emby ? Differences visibles (VFQ/VFF, BLURAY/WEB-DL, 1080p/4K...).
// @match        *://*/*
// @exclude      http://127.0.0.1:*/*
// @exclude      http://localhost:*/*
// @grant        GM_xmlhttpRequest
// @grant        GM_registerMenuCommand
// @grant        GM_getValue
// @grant        GM_setValue
// @connect      127.0.0.1
// @connect      localhost
// @run-at       document-idle
// @noframes
// ==/UserScript==
// Genere par Seedbox Dashboard : l'adresse et le jeton ci-dessous sont
// propres a cette installation. Reinstalle le script depuis l'application
// si tu changes le port.
(function () {
  'use strict';
  const API = 'http://127.0.0.1:__PORT__';
  const TOKEN = '__TOKEN__';
  const HOVER_DELAY = 350;

  const opt = {
    hover: GM_getValue('hover', true),     // survol des liens
    page: GM_getValue('page', true),       // bandeau a l'ouverture d'une fiche
    absent: GM_getValue('absent', true),   // bandeau aussi quand le film manque
    select: GM_getValue('select', true),   // texte selectionne
  };

  // ---------------------------------------------------------------- reseau
  const cache = new Map();
  function ask(params) {
    const clean = {};
    for (const k in params) if (params[k]) clean[k] = params[k];
    const qs = new URLSearchParams(clean).toString();
    if (cache.has(qs)) return cache.get(qs);
    const p = new Promise((resolve) => {
      GM_xmlhttpRequest({
        method: 'GET', url: API + '/check?' + qs, timeout: 8000,
        headers: { 'X-Emby-Check-Token': TOKEN },
        onload: (r) => {
          if (r.status === 401) return resolve({ ok: false, error: 'jeton' });
          try { resolve(JSON.parse(r.responseText)); }
          catch (e) { resolve({ ok: false, error: 'reponse' }); }
        },
        onerror: () => resolve({ ok: false, error: 'hors-ligne' }),
        ontimeout: () => resolve({ ok: false, error: 'hors-ligne' }),
      });
    });
    cache.set(qs, p);
    p.then((r) => { if (!r.ok) cache.delete(qs); });   // reessayer plus tard
    return p;
  }

  // ------------------------------------------------------------- detection
  const RE_YEAR = /(^|[^0-9])(19[0-9]{2}|20[0-9]{2})([^0-9]|$)/;
  const RE_TAG = /(^|[^a-z0-9])(2160p|1080p|720p|4k|uhd|blu-?ray|bdrip|web-?dl|webrip|hdlight|remux|x26[45]|h\.?26[45]|hevc|av1|multi|vff|vfq|vf2|vostfr|truefrench|french)([^a-z0-9]|$)/i;
  function looksLikeMovie(t) {
    return !!t && t.length >= 4 && t.length <= 300 &&
      (RE_YEAR.test(t) || RE_TAG.test(t));
  }
  const squash = (t) => (t || '').replace(/\s+/g, ' ').trim();

  function textOf(el) {
    let t = squash(el.innerText || el.textContent);
    const alt = squash(el.getAttribute('title') || el.getAttribute('data-title') ||
                       el.getAttribute('aria-label'));
    if (alt && (alt.length > t.length || /(…|\.\.\.)$/.test(t))) t = alt;
    return t.length > 300 ? '' : t;
  }

  // Taille affichee sur la meme ligne (tableaux de trackers) : permet de
  // reconnaitre un fichier identique a celui deja possede.
  const UNIT = { o: 0, b: 0, k: 1, m: 2, g: 3, t: 4 };
  function sizeNear(el) {
    const row = el.closest('tr, li, article');
    if (!row) return 0;
    const m = squash(row.innerText).match(/(\d+(?:[.,]\d+)?)\s*([KMGT])i?[oB]\b/i);
    if (!m) return 0;
    return Math.round(parseFloat(m[1].replace(',', '.')) *
                      Math.pow(1024, UNIT[m[2].toLowerCase()]));
  }

  // ----------------------------------------------------------------- rendu
  const STATUS = {
    absent: ['Pas sur Emby', '#9aa0aa', '✖'],
    identique: ['Déjà sur Emby (même version)', '#2ecc71', '✔'],
    present: ['Déjà sur Emby (autre version)', '#6ec896', '✔'],
    mieux: ['Sur Emby, mais cette release est meilleure', '#eb8c14', '▲'],
    inconnu: ['Bibliothèque Emby non chargée', '#9aa0aa', '?'],
  };
  const LSTATUS = {
    absent: 'Pas sur le disque', identique: 'Sur le disque (même version)',
    present: 'Sur le disque (autre version)',
    mieux: 'Sur le disque, mais cette release est meilleure',
  };
  const KEYS = [['lang', 'Langue'], ['source', 'Source'], ['res', 'Résolution'],
                ['codec', 'Codec'], ['hdr', 'HDR'], ['audio', 'Audio'],
                ['edition', 'Édition'], ['group', 'Équipe']];

  function h(tag, css, text) {
    const e = document.createElement(tag);
    if (css) e.className = css;
    if (text != null) e.textContent = text;
    return e;
  }
  function attrLine(a) {
    return KEYS.filter(([k]) => a[k] && k !== 'group').map(([k]) => a[k]).join(' · ');
  }

  function versionsBlock(box, versions, where) {
    for (const v of versions) {
      const d = box.appendChild(h('div', 'ver'));
      d.appendChild(h('div', 'file', v.file || v.name));
      d.appendChild(h('div', 'attrs', [attrLine(v.attrs), v.size_txt]
        .filter((x) => x && x !== '-').join(' · ')));
      if (v.same_size) d.appendChild(h('div', 'diff same', 'Même taille que la release'));
      for (const df of v.diffs) {
        const row = d.appendChild(h('div', 'diff ' + df.cmp));
        const arrow = df.cmp === 'mieux' ? '▲ ' : df.cmp === 'moins' ? '▼ ' : '≠ ';
        row.textContent = arrow + df.label + ' : ' + df.page + ' ici, ' + df.have + ' ' + where;
      }
      if (!v.diffs.length && !v.same_size) d.appendChild(h('div', 'diff same', 'Aucune différence visible'));
    }
  }

  function render(res, opts) {
    const root = h('div', 'card');
    if (opts && opts.close) {
      const x = root.appendChild(h('span', 'close', '×'));
      x.title = 'Fermer';
      x.addEventListener('click', opts.close);
    }
    if (!res || !res.ok) {
      const msg = res && res.error === 'jeton'
        ? 'Jeton refusé : réinstalle le script depuis Seedbox Dashboard.'
        : 'Seedbox Dashboard injoignable (lancé ? port __PORT__)';
      root.appendChild(h('div', 'head grey', msg));
      return root;
    }
    const e = res.emby, l = res.local;
    const [lbl, col, ico] = STATUS[e.status] || STATUS.inconnu;
    const head = root.appendChild(h('div', 'head', ico + ' ' + lbl));
    head.style.color = col;
    const cap = (t) => (t || '').replace(/(^|\s)\S/g, (c) => c.toUpperCase());
    const tt = (e.versions[0] && e.versions[0].name) || cap(res.title);
    const yr = (e.versions[0] && e.versions[0].year) || res.year;
    if (tt) root.appendChild(h('div', 'title', tt + (yr ? ' (' + yr + ')' : '')));
    const pl = attrLine(res.page);
    if (pl) root.appendChild(h('div', 'page', 'Cette release : ' + pl));
    versionsBlock(root, e.versions, 'sur Emby');
    if (res.local_loaded && l.status !== 'absent') {
      root.appendChild(h('div', 'sub', LSTATUS[l.status] || ''));
      versionsBlock(root, l.versions, 'sur le disque');
    }
    if (e.versions.concat(l.versions).some((v) => v.diffs.some((d) => d.cmp !== 'autre')))
      root.appendChild(h('div', 'legend', '\u25b2 cette release a mieux \u00b7 \u25bc tu as d\u00e9j\u00e0 mieux'));
    return root;
  }

  // ------------------------------------------------- calque (shadow DOM)
  const host = document.createElement('div');
  host.style.cssText = 'all:initial;position:fixed;z-index:2147483647;top:0;left:0;';
  const shadow = host.attachShadow({ mode: 'closed' });
  const style = document.createElement('style');
  style.textContent = `
    .card{font:13px/1.4 system-ui,Segoe UI,Arial,sans-serif;color:#e4e6ec;background:#1e2028f2;
      border:1px solid #3a3d4a;border-radius:8px;padding:9px 12px;max-width:460px;
      box-shadow:0 6px 24px #0008;position:relative}
    .head{font-weight:600;font-size:14px;padding-right:16px}
    .grey{color:#9aa0aa}
    .title{color:#fff;margin-top:2px}
    .page{color:#9fb4d8;font-size:12px;margin-top:4px}
    .sub{margin-top:8px;font-weight:600;color:#9fb4d8}
    .ver{margin-top:6px;padding:5px 7px;background:#ffffff0d;border-radius:5px}
    .file{font-size:11px;color:#a8acb8;word-break:break-all}
    .attrs{color:#e4e6ec}
    .diff{font-size:12px;margin-top:1px}
    .diff.mieux{color:#eb8c14} .diff.moins{color:#6ec896} .diff.autre{color:#e6c35c}
    .diff.same{color:#2ecc71}
    .legend{font-size:11px;color:#7d8290;margin-top:6px}
    .close{position:absolute;top:4px;right:8px;cursor:pointer;color:#9aa0aa;font-size:16px}
    .tip{position:fixed;pointer-events:none}
    .badge{position:fixed;top:12px;right:12px;pointer-events:auto}
  `;
  shadow.appendChild(style);
  const tip = shadow.appendChild(h('div', 'tip'));
  const badge = shadow.appendChild(h('div', 'badge'));
  tip.style.display = badge.style.display = 'none';
  (document.body || document.documentElement).appendChild(host);

  let mouse = { x: 0, y: 0 };
  function placeTip() {
    const r = tip.getBoundingClientRect();
    let x = mouse.x + 16, y = mouse.y + 18;
    if (x + r.width > innerWidth - 8) x = Math.max(8, mouse.x - r.width - 16);
    if (y + r.height > innerHeight - 8) y = Math.max(8, mouse.y - r.height - 12);
    tip.style.left = x + 'px';
    tip.style.top = y + 'px';
  }
  function showTip(node) {
    tip.replaceChildren(node);
    tip.style.display = 'block';
    placeTip();
  }
  function hideTip() { tip.style.display = 'none'; tip.replaceChildren(); }

  // ----------------------------------------------------------------- survol
  let cur = null, timer = null;
  document.addEventListener('mousemove', (ev) => {
    mouse = { x: ev.clientX, y: ev.clientY };
    if (tip.style.display === 'block') placeTip();
  }, { passive: true, capture: true });

  document.addEventListener('mouseover', (ev) => {
    if (!opt.hover || !ev.target.closest) return;
    const el = ev.target.closest('a, [title], [data-title]') || (ev.altKey ? ev.target : null);
    if (!el || el === cur) return;
    const text = textOf(el);
    if (!text || !(ev.altKey || looksLikeMovie(text))) return;
    cur = el;
    clearTimeout(timer);
    timer = setTimeout(async () => {
      const res = await ask({ q: text, size: sizeNear(el) });
      if (cur === el) showTip(render(res));
    }, HOVER_DELAY);
  }, true);

  document.addEventListener('mouseout', (ev) => {
    if (cur && !cur.contains(ev.relatedTarget)) {
      cur = null;
      clearTimeout(timer);
      hideTip();
    }
  }, true);

  // ---------------------------------------------------- texte selectionne
  document.addEventListener('mouseup', () => {
    if (!opt.select) return;
    setTimeout(async () => {
      const t = squash(String(getSelection() || ''));
      if (t.length < 3 || t.length > 200) return;
      const res = await ask({ q: t });
      if (squash(String(getSelection() || '')) !== t) return;
      // Une selection quelconque n'affiche rien, sauf si le film est trouve.
      if (!looksLikeMovie(t) && !(res.ok && res.emby.versions.length)) return;
      showTip(render(res));
      setTimeout(hideTip, 6000);
    }, 50);
  }, true);
  document.addEventListener('mousedown', () => { if (!cur) hideTip(); }, true);

  // --------------------------------------------- ouverture d'une fiche
  function pageIds() {
    const u = location.href;
    let m = u.match(/imdb\.com\/(?:[a-z-]+\/)?title\/(tt\d+)/i);
    if (m) return { imdb: m[1] };
    m = u.match(/themoviedb\.org\/movie\/(\d+)/i);
    if (m) return { tmdb: m[1] };
    // Fiche de tracker / forum : un seul film IMDb cite dans la page.
    const ids = new Set();
    for (const a of document.querySelectorAll('a[href*="imdb.com/title/tt"]')) {
      const t = (a.href.match(/tt\d+/) || [])[0];
      if (t) ids.add(t);
    }
    if (ids.size === 1) return { imdb: [...ids][0] };
    return null;
  }

  function pageTexts() {
    const out = [];
    const h1 = document.querySelector('h1');
    if (h1) out.push(squash(h1.innerText));
    const og = document.querySelector('meta[property="og:title"]');
    if (og) out.push(squash(og.content));
    out.push(squash(document.title));
    return out.filter(Boolean);
  }

  let badgeTimer = null;
  function closeBadge() { badge.style.display = 'none'; badge.replaceChildren(); }
  async function checkPage() {
    if (!opt.page) return;
    const ids = pageIds();
    const texts = pageTexts();
    const q = texts.find(looksLikeMovie) || (ids ? texts[0] : '');
    if (!ids && !q) return;
    const res = await ask(Object.assign({ q: q }, ids || {}));
    if (!res.ok && !ids) return;                  // pas de bruit hors fiches
    const st = res.ok ? res.emby.status : '';
    if (st === 'absent' && !opt.absent) return;
    if (st === 'inconnu' && !ids) return;
    badge.replaceChildren(render(res, { close: closeBadge }));
    badge.style.display = 'block';
    clearTimeout(badgeTimer);
    if (st === 'absent') badgeTimer = setTimeout(closeBadge, 8000);
  }

  let lastUrl = location.href;
  setTimeout(checkPage, 600);
  setInterval(() => {                            // sites a navigation interne
    if (location.href !== lastUrl) {
      lastUrl = location.href;
      closeBadge();
      setTimeout(checkPage, 900);
    }
  }, 1000);

  // ------------------------------------------------------------- menu
  function toggle(key, lbl) {
    GM_registerMenuCommand((opt[key] ? '☑ ' : '☐ ') + lbl, () => {
      opt[key] = !opt[key];
      GM_setValue(key, opt[key]);
      alert(lbl + ' : ' + (opt[key] ? 'activé' : 'désactivé') +
            '\n(effet complet au rechargement de la page)');
    });
  }
  GM_registerMenuCommand('Vérifier un titre…', async () => {
    const t = prompt('Nom du film ou de la release :', String(getSelection() || ''));
    if (!t) return;
    const res = await ask({ q: t });
    badge.replaceChildren(render(res, { close: closeBadge }));
    badge.style.display = 'block';
  });
  GM_registerMenuCommand('Vérifier cette page', () => { cache.clear(); checkPage(); });
  toggle('hover', 'Survol des liens');
  toggle('page', 'Bandeau à l’ouverture d’une fiche');
  toggle('absent', 'Bandeau aussi pour un film absent');
  toggle('select', 'Vérifier le texte sélectionné');
})();
"""


def _browser_lbl(msg, ok=True):
    if dpg.does_item_exist("sb_br_lbl"):
        dpg.set_value("sb_br_lbl", msg)
        dpg.configure_item("sb_br_lbl",
                           color=(46, 204, 113) if ok else (215, 75, 90))


def apply_browser_server(sender=None, app_data=None, user_data=None):
    """(Re)demarre ou arrete le serveur selon la case et le port saisis."""
    CFG["browser_enabled"] = bool(gv("sb_br_on", CFG["browser_enabled"]))
    try:
        CFG["browser_port"] = max(1024, min(65535, int(
            gv("sb_br_port", CFG["browser_port"]) or 8765)))
    except (TypeError, ValueError):
        CFG["browser_port"] = 8765
    save_cfg(CFG)
    if not CFG["browser_enabled"]:
        stop_browser_server()
        _browser_lbl("Arrete", ok=False)
        return
    ok, msg = start_browser_server(CFG["browser_port"])
    _browser_lbl(msg, ok)
    if not ok:
        add_log("Verification navigateur : " + msg, (215, 75, 90))


def install_userscript(sender=None, app_data=None, user_data=None):
    """Ouvre l'adresse du script : Tampermonkey / Violentmonkey proposent
    alors de l'installer. Copie aussi le fichier a cote de l'application."""
    if not _SRV.get("httpd"):
        modal("Serveur arrete",
              "Active d'abord la verification navigateur (case 'Activer').")
        return
    try:
        (APP_DIR / USERSCRIPT_NAME).write_text(userscript_source(),
                                               encoding="utf-8")
    except Exception as exc:
        log("userscript: %s" % exc)
    url = "http://127.0.0.1:%d/%s" % (_SRV["port"], USERSCRIPT_NAME)
    webbrowser.open(url)
    add_log("Script navigateur ouvert : %s  (copie : %s)"
            % (url, APP_DIR / USERSCRIPT_NAME), (120, 200, 255))


def new_browser_token(sender=None, app_data=None, user_data=None):
    CFG["browser_token"] = ""
    browser_token()
    _browser_lbl("Nouveau jeton : reinstalle le script dans le navigateur",
                 ok=True)


def test_browser_lookup(sender=None, app_data=None, user_data=None):
    q = (gv("sb_br_test") or "").strip()
    if not q:
        return
    try:
        txt = lookup_text(browser_lookup(q))
    except Exception as exc:
        txt = "Erreur : %s" % exc
    dpg.set_value("sb_br_out", txt)


# =====================================================================
#  INTERFACE
# =====================================================================
def build_ui():
    with dpg.window(tag="sb_win", no_title_bar=True, no_move=True,
                    no_resize=True, no_scrollbar=True):

        with dpg.collapsing_header(label="Connexion a la seedbox",
                                   default_open=not CRD["url"]):
            with dpg.group(horizontal=True):
                dpg.add_text("URL")
                dpg.add_input_text(tag="sb_url", width=380,
                                   hint="https://xxx.seedbox.io",
                                   default_value=CRD["url"])
                dpg.add_text("Utilisateur")
                dpg.add_input_text(tag="sb_user", width=150,
                                   default_value=CRD["user"])
                dpg.add_text("Mot de passe")
                dpg.add_input_text(tag="sb_pass", width=170, password=True,
                                   default_value=CRD["password"])
            with dpg.group(horizontal=True):
                dpg.add_text("Client")
                dpg.add_combo(("auto", "qbittorrent", "rtorrent", "deluge"),
                              tag="sb_kind", width=140,
                              default_value=CRD.get("client", "auto"))
                with dpg.tooltip(dpg.last_item()):
                    dpg.add_text("'auto' teste qBittorrent, Deluge puis "
                                 "rTorrent et garde celui qui repond.\n"
                                 "Pour ruTorrent, pointe l'URL sur le dossier "
                                 "rutorrent (ou directement sur rpc.php).",
                                 wrap=380)
                dpg.add_checkbox(label="Verifier le certificat SSL",
                                 tag="sb_verify",
                                 default_value=CRD.get("verify_ssl", "1") != "0")
                b = dpg.add_button(label="Connecter", width=130,
                                   tag="sb_btn_connect", callback=do_connect)
                dpg.bind_item_theme(b, "sb_th_ok")
                dpg.add_text("", tag="sb_clientlbl", color=(46, 204, 113))
            with dpg.group(horizontal=True):
                dpg.add_text("Options d'ajout :", color=(150, 150, 200))
                dpg.add_text("Categorie/label")
                dpg.add_input_text(tag="sb_cat", width=130, hint="films",
                                   default_value=CFG["category"],
                                   callback=lambda s, a, u: persist())
                dpg.add_text("Dossier de destination")
                dpg.add_input_text(tag="sb_savepath", width=240,
                                   hint="laisser vide = defaut du client",
                                   default_value=CFG["save_path"],
                                   callback=lambda s, a, u: persist())
                dpg.add_checkbox(label="Ajouter en pause", tag="sb_paused",
                                 default_value=CFG["add_paused"],
                                 callback=lambda s, a, u: persist())
            with dpg.group(horizontal=True):
                dpg.add_text("Espace :", color=(150, 150, 200))
                dpg.add_text("Quota de l'offre")
                dpg.add_input_int(tag="sb_quota", width=110, min_value=0,
                                  max_value=1000000, step=100,
                                  default_value=int(CFG["quota_go"]),
                                  callback=lambda s, a, u: defer(
                                      "espace", lambda: (persist(),
                                                         update_disk()), 0.3))
                dpg.add_text("Go")
                with dpg.tooltip(dpg.last_item()):
                    dpg.add_text("Capacite totale annoncee par ton offre. "
                                 "Sert a afficher le taux de remplissage quand "
                                 "le client ne publie que l'espace libre "
                                 "(qBittorrent, Deluge).\n0 = inconnu.", wrap=360)
                dpg.add_text("Garder au moins")
                dpg.add_input_int(tag="sb_minfree", width=100, min_value=0,
                                  max_value=100000, step=5,
                                  default_value=int(CFG["min_free_go"]),
                                  callback=lambda s, a, u: defer(
                                      "espace", lambda: (persist(),
                                                         update_disk()), 0.3))
                dpg.add_text("Go libres")
                dpg.add_checkbox(label="Refuser un ajout qui ne tiendrait pas",
                                 tag="sb_checkspace",
                                 default_value=CFG["check_space"],
                                 callback=lambda s, a, u: persist())
                dpg.add_checkbox(label="Reserver aussi l'espace des torrents "
                                 "dormants", tag="sb_countpaused",
                                 default_value=CFG["count_paused"],
                                 callback=lambda s, a, u: (persist(),
                                                           update_disk()))
                with dpg.tooltip(dpg.last_item()):
                    dpg.add_text("Dormant = en pause, en erreur, d'etat inconnu, "
                                 "ou bloque sans aucun pair. Par defaut ces "
                                 "torrents ne reservent pas d'espace : rien ne "
                                 "dit qu'ils repartiront un jour, et les "
                                 "compter ferait refuser des ajouts "
                                 "legitimes.\nCoche si tu comptes tous les "
                                 "relancer.", wrap=380)

        with dpg.collapsing_header(label="Comparaison avec Emby",
                                   default_open=not EMB["url"]):
            dpg.add_text("Croise les torrents de la seedbox avec ta "
                         "bibliotheque Emby : ce qui est deja rapatrie "
                         "apparait en vert et peut etre efface de la seedbox.",
                         color=(150, 150, 175), wrap=1100)
            with dpg.group(horizontal=True):
                dpg.add_text("URL Emby")
                dpg.add_input_text(tag="sb_emby_url", width=300,
                                   hint="http://192.168.1.x:8096",
                                   default_value=EMB["url"])
                dpg.add_text("Cle API")
                dpg.add_input_text(tag="sb_emby_key", width=260, password=True,
                                   default_value=EMB["api_key"])
                dpg.add_text("User ID")
                dpg.add_input_text(tag="sb_emby_uid", width=110,
                                   hint="optionnel", default_value=EMB["user_id"])
                b = dpg.add_button(label="Charger Emby", width=140,
                                   tag="sb_emby_btn", callback=do_load_emby)
                dpg.bind_item_theme(b, "sb_th_ok")
                dpg.add_text("", tag="sb_emby_lbl", color=(46, 204, 113))
                dpg.add_text("Seuil")
                dpg.add_slider_int(tag="sb_embyfuzzy", width=120, min_value=60,
                                   max_value=100, format="%d%%",
                                   default_value=int(CFG["emby_fuzzy"]),
                                   callback=lambda s, a, u: defer(
                                       "emby_seuil", apply_emby_seuil, 0.35))

            dpg.add_separator()
            dpg.add_text("DOSSIERS COMPARES EN PLUS D'EMBY", color=(120, 200, 255))
            dpg.add_text("Le nom des fichiers video presents dans ces dossiers "
                         "est compare au nom des torrents. Utile pour un film "
                         "deja rapatrie mais pas encore indexe dans Emby.",
                         color=(150, 150, 175), wrap=1100)
            with dpg.group(horizontal=True):
                dpg.add_input_text(tag="sb_dirin", width=440,
                                   hint=r"\\NAS\video\Films  (UNC accepte)",
                                   on_enter=True, callback=add_local_dir)
                dpg.add_button(label="Ajouter", width=90, callback=add_local_dir)
                dpg.add_button(label="Parcourir", width=100,
                               callback=lambda s, a, u: (browse_dir("sb_dirin"),
                                                         add_local_dir()))
                b = dpg.add_button(label="Analyser les dossiers", width=180,
                                   tag="sb_dirscan", callback=do_scan_local)
                dpg.bind_item_theme(b, "sb_th_ok")
                dpg.add_text("", tag="sb_dirlbl", color=(46, 204, 113))
            with dpg.child_window(tag="sb_dirlist", height=80, border=True):
                pass

            dpg.add_separator()
            dpg.add_text("VERIFICATION DEPUIS LE NAVIGATEUR", color=(120, 200, 255))
            dpg.add_text("En survolant un nom de film sur une page web (ou en "
                         "ouvrant sa fiche IMDb, TMDB, tracker...), le navigateur "
                         "indique s'il est deja sur Emby ou sur le disque, et les "
                         "differences visibles : VFQ au lieu de VFF, BLURAY au "
                         "lieu de WEB-DL, 1080p au lieu de 4K... Necessite "
                         "l'extension Tampermonkey ou Violentmonkey.",
                         color=(150, 150, 175), wrap=1100)
            with dpg.group(horizontal=True):
                dpg.add_checkbox(label="Activer", tag="sb_br_on",
                                 default_value=bool(CFG["browser_enabled"]),
                                 callback=apply_browser_server)
                dpg.add_text("Port")
                dpg.add_input_int(tag="sb_br_port", width=110, min_value=1024,
                                  max_value=65535, step=0,
                                  default_value=int(CFG["browser_port"]),
                                  on_enter=True, callback=apply_browser_server)
                b = dpg.add_button(label="Installer le script navigateur",
                                   width=250, callback=install_userscript)
                dpg.bind_item_theme(b, "sb_th_ok")
                with dpg.tooltip(dpg.last_item()):
                    dpg.add_text("Ouvre le script dans le navigateur par "
                                 "defaut : Tampermonkey propose de l'installer.\n"
                                 "Le script ne parle qu'a cette application "
                                 "(127.0.0.1), avec un jeton secret.\n"
                                 "Dans la page : survol d'un lien = infobulle ; "
                                 "Alt + survol = n'importe quel texte ; "
                                 "selection d'un titre = verification ; menu "
                                 "de l'extension = options.", wrap=420)
                dpg.add_button(label="Nouveau jeton", width=130,
                               callback=new_browser_token)
                dpg.add_text("", tag="sb_br_lbl", color=(46, 204, 113))
            with dpg.group(horizontal=True):
                dpg.add_text("Essai")
                dpg.add_input_text(tag="sb_br_test", width=560, on_enter=True,
                                   hint="Dune.Part.Two.2024.MULTI.VFQ.2160p."
                                        "WEB-DL.x265-GRP",
                                   callback=test_browser_lookup)
                dpg.add_button(label="Verifier", width=100,
                               callback=test_browser_lookup)
            dpg.add_input_text(tag="sb_br_out", multiline=True, readonly=True,
                               width=-1, height=110, default_value="")

        dpg.add_progress_bar(tag="sb_pb", default_value=0.0, width=-1,
                             overlay="", show=False)
        dpg.add_text("Pret.", tag="sb_status", color=(150, 200, 240))
        dpg.add_separator()

        with dpg.tab_bar():
            with dpg.tab(label="Tableau de bord"):
                dpg.add_text("", tag="sb_headline", color=(150, 200, 240))
                dpg.add_progress_bar(tag="sb_diskbar", default_value=0.0,
                                     width=-1, overlay="")
                with dpg.group(horizontal=True):
                    dpg.add_button(label="Detail de l'espace", width=170,
                                   callback=show_space_detail)
                    dpg.add_text("", tag="sb_diskline", color=(150, 200, 240))
                with dpg.group(horizontal=True):
                    dpg.add_text("Filtre")
                    dpg.add_input_text(tag="sb_filter", width=220,
                                       hint="nom ou categorie...",
                                       callback=lambda s, a, u: defer(
                                           "filtre", render_dashboard, 0.25))
                    dpg.add_text("Etat")
                    dpg.add_combo(STATE_FILTERS, tag="sb_statefilter", width=150,
                                  default_value="Tous",
                                  callback=lambda s, a, u: defer(
                                      "filtre", render_dashboard, 0.05))
                    dpg.add_text("Emby")
                    dpg.add_combo(EMBY_FILTERS, tag="sb_embyfilter", width=150,
                                  default_value="Tous",
                                  callback=lambda s, a, u: defer(
                                      "filtre", render_dashboard, 0.05))
                    dpg.add_checkbox(label="Auto", tag="sb_autoref",
                                     default_value=CFG["auto_refresh"],
                                     callback=lambda s, a, u: persist())
                    dpg.add_input_int(tag="sb_refresh", width=90, min_value=2,
                                      max_value=120, step=1,
                                      default_value=int(CFG["refresh"]),
                                      callback=lambda s, a, u: defer(
                                          "refresh", persist, 0.3))
                    dpg.add_text("s")
                    dpg.add_button(label="Rafraichir", width=110,
                                   callback=refresh_now)
                    dpg.add_text("Colonnes")
                    dpg.add_combo(VUES, tag="sb_vue", width=140,
                                  default_value=CFG.get("vue", "Tout"),
                                  callback=change_vue)
                    with dpg.tooltip(dpg.last_item()):
                        dpg.add_text("Tout        : toutes les colonnes\n"
                                     "Comparaison : masque DL/UL/ETA/ratio/"
                                     "tracker pour ne garder que le croisement "
                                     "Emby et disque\n"
                                     "Transfert   : masque les colonnes de "
                                     "comparaison", wrap=400)
                with dpg.group(horizontal=True):
                    dpg.add_button(label="Tout cocher", width=110, callback=sel_all)
                    dpg.add_button(label="Tout decocher", width=120,
                                   callback=sel_none)
                    b = dpg.add_button(label="Cocher ceux deja presents",
                                       width=210, callback=sel_emby)
                    dpg.bind_item_theme(b, "sb_th_ok")
                    dpg.add_text("|", color=(80, 90, 120))
                    dpg.add_button(label="Pause", width=90, callback=act_pause)
                    dpg.add_button(label="Reprendre", width=110,
                                   callback=act_resume)
                    dpg.add_button(label="Verifier", width=100,
                                   callback=act_recheck)
                    b = dpg.add_button(label="Reinitialiser", width=130,
                                       callback=act_reset)
                    dpg.bind_item_theme(b, "sb_th_danger")
                    with dpg.tooltip(b):
                        dpg.add_text("Efface les fichiers deja telecharges sur "
                                     "la seedbox et re-ajoute le torrent a "
                                     "zero.\nLe .torrent d'origine est "
                                     "reutilise s'il est retrouve, sinon "
                                     "l'export du client, sinon un magnet.",
                                     wrap=400)
                    b = dpg.add_button(label="Retirer de la seedbox", width=190,
                                       callback=act_delete)
                    dpg.bind_item_theme(b, "sb_th_danger")
                with dpg.child_window(tag="sb_table_area", height=-1,
                                      border=False):
                    dpg.add_text("Non connecte.", color=(150, 150, 175))

            with dpg.tab(label="Inventaire"):
                dpg.add_text("Tout ce qui a ete vu au moins une fois sur la "
                             "seedbox est conserve ici, y compris ce qui a "
                             "disparu depuis. Contrairement au journal, cela "
                             "couvre aussi les torrents ajoutes hors de cet "
                             "outil.", color=(150, 150, 175), wrap=1100)
                with dpg.group(horizontal=True):
                    dpg.add_checkbox(label="Sauvegarde automatique",
                                     tag="sb_snapon",
                                     default_value=CFG["snap_enabled"],
                                     callback=lambda s, a, u: persist())
                    dpg.add_text("toutes les")
                    dpg.add_input_int(tag="sb_snapint", width=90, min_value=1,
                                      max_value=1440,
                                      default_value=int(CFG["snap_interval"]),
                                      callback=lambda s, a, u: defer(
                                          "snap", persist, 0.3))
                    dpg.add_text("min, et a chaque changement de la liste")
                    dpg.add_text("|", color=(80, 90, 120))
                    dpg.add_text("conserver")
                    dpg.add_input_int(tag="sb_snapkeep", width=90, min_value=0,
                                      max_value=3650,
                                      default_value=int(CFG["snap_keep"]),
                                      callback=lambda s, a, u: defer(
                                          "snap", persist, 0.3))
                    dpg.add_text("jours d'instantanes")
                with dpg.group(horizontal=True):
                    dpg.add_text("Dossier")
                    dpg.add_input_text(tag="sb_snapdir", width=400,
                                       default_value=CFG["snap_dir"],
                                       hint="vide = seedbox_snapshots a cote du script",
                                       callback=lambda s, a, u: persist())
                    dpg.add_button(label="Parcourir", width=110,
                                   callback=lambda s, a, u: browse_dir("sb_snapdir"))
                    dpg.add_button(label="Ouvrir le dossier", width=160,
                                   callback=open_snap_dir)
                    b = dpg.add_button(label="Sauvegarder maintenant", width=200,
                                       callback=snap_now)
                    dpg.bind_item_theme(b, "sb_th_ok")
                    dpg.add_button(label="Exporter en CSV", width=150,
                                   callback=export_inventory)
                dpg.add_separator()
                with dpg.group(horizontal=True):
                    dpg.add_text("Filtre")
                    dpg.add_input_text(tag="sb_invfilter", width=250,
                                       hint="nom ou categorie...",
                                       callback=lambda s, a, u: defer(
                                           "inv", render_inventory, 0.25))
                    dpg.add_combo(("Tout", "Presents", "Disparus"),
                                  tag="sb_invmode", width=140,
                                  default_value="Tout",
                                  callback=lambda s, a, u: defer(
                                      "inv", render_inventory, 0.05))
                    dpg.add_button(label="Rafraichir", width=110,
                                   callback=render_inventory)
                    dpg.add_text("", tag="sb_invlbl", color=(150, 200, 240))
                with dpg.child_window(tag="sb_inv_area", height=-1, border=False):
                    pass

            with dpg.tab(label="Ajout de torrents"):
                dpg.add_text("ENVOI MANUEL", color=(120, 200, 255))
                with dpg.group(horizontal=True):
                    b = dpg.add_button(label="Choisir des .torrent...", width=210,
                                       callback=pick_files)
                    dpg.bind_item_theme(b, "sb_th_go")
                    dpg.add_text("Les options d'ajout ci-dessus (categorie, "
                                 "destination, pause) s'appliquent.",
                                 color=(150, 150, 175))
                with dpg.group(horizontal=True):
                    dpg.add_text("Si le film est deja sur Emby :",
                                 color=(150, 150, 200))
                    dpg.add_combo(("demander", "ignorer", "envoyer"),
                                  tag="sb_embydup", width=140,
                                  default_value=CFG["emby_dup"],
                                  callback=lambda s, a, u: persist())
                    with dpg.tooltip(dpg.last_item()):
                        dpg.add_text(
                            "Avant chaque envoi, le contenu du .torrent est "
                            "compare a ta bibliotheque Emby.\n\n"
                            "demander : une fenetre recapitule et te laisse "
                            "trancher (defaut)\n"
                            "ignorer  : le torrent n'est pas envoye, une ligne "
                            "l'indique dans le journal\n"
                            "envoyer  : aucun controle, tout part\n\n"
                            "Un torrent de MEILLEURE qualite que la version "
                            "Emby n'est jamais bloque : c'est un upgrade.",
                            wrap=420)
                    dpg.add_text("(necessite d'avoir charge Emby)",
                                 color=(150, 150, 175))
                with dpg.group(horizontal=True):
                    dpg.add_text("Les refuses vont dans")
                    dpg.add_input_text(tag="sb_refusdir", width=380,
                                       default_value=CFG["refus_dir"],
                                       hint="vide = sous-dossier _deja_presents",
                                       callback=lambda s, a, u: persist())
                    dpg.add_button(label="Parcourir", width=110,
                                   callback=lambda s, a, u: browse_dir("sb_refusdir"))
                    dpg.add_text("Le .torrent quitte le dossier surveille : il "
                                 "ne reviendra pas au cycle suivant.",
                                 color=(150, 150, 175))
                dpg.add_spacer(height=4)
                dpg.add_text("Liens magnet (un par ligne, ou un infohash brut) :")
                dpg.add_input_text(tag="sb_magnets", multiline=True, width=-1,
                                   height=80)
                with dpg.group(horizontal=True):
                    dpg.add_button(label="Envoyer les magnets", width=200,
                                   callback=send_magnets)
                    dpg.add_text("Les magnets ne sont pas compares a Emby : "
                                 "leur contenu n'est connu qu'apres "
                                 "recuperation des metadonnees.",
                                 color=(150, 150, 175))
                dpg.add_separator()

                dpg.add_text("DOSSIER SURVEILLE  -  ajout automatique",
                             color=(120, 200, 255))
                dpg.add_text("Tout .torrent depose dans ce dossier part sur la "
                             "seedbox. Pratique avec un navigateur configure "
                             "pour telecharger les .torrent dedans.",
                             color=(150, 150, 175), wrap=1100)
                with dpg.group(horizontal=True):
                    dpg.add_text("Dossier")
                    dpg.add_input_text(tag="sb_watchdir", width=430,
                                       default_value=CFG["watch_dir"],
                                       hint=r"C:\Users\...\Downloads\torrents",
                                       callback=lambda s, a, u: persist())
                    dpg.add_button(label="Parcourir", width=110,
                                   callback=lambda s, a, u: browse_dir("sb_watchdir"))
                    dpg.add_text("Intervalle")
                    dpg.add_input_int(tag="sb_watch_int", width=90, min_value=3,
                                      max_value=600,
                                      default_value=int(CFG["watch_interval"]),
                                      callback=lambda s, a, u: persist())
                    dpg.add_text("s")
                with dpg.group(horizontal=True):
                    dpg.add_text("Apres envoi")
                    dpg.add_combo(("archiver", "supprimer", "rien"),
                                  tag="sb_after", width=130,
                                  default_value=CFG["after_send"],
                                  callback=lambda s, a, u: persist())
                    dpg.add_text("Dossier d'archive")
                    dpg.add_input_text(tag="sb_archdir", width=330,
                                       default_value=CFG["archive_dir"],
                                       hint="vide = sous-dossier _envoyes",
                                       callback=lambda s, a, u: persist())
                    dpg.add_button(label="Parcourir", width=110,
                                   callback=lambda s, a, u: browse_dir("sb_archdir"))
                    dpg.add_checkbox(label="Surveillance active", tag="sb_watch_on",
                                     default_value=CFG["watch_enabled"],
                                     callback=toggle_watch)
                dpg.add_separator()
                dpg.add_text("JOURNAL", color=(120, 200, 255))
                with dpg.child_window(tag="sb_logarea", height=-1, border=True):
                    pass


def main():
    dpg.create_context()
    setup_theme()
    dpg.create_viewport(title=APP_TITLE, width=1560, height=900,
                        min_width=1100, min_height=620)
    build_font()
    build_ui()
    dpg.setup_dearpygui()
    dpg.show_viewport()
    dpg.set_primary_window("sb_win", True)

    def layout():
        try:
            dpg.set_item_width("sb_win", dpg.get_viewport_client_width())
            dpg.set_item_height("sb_win", dpg.get_viewport_client_height())
        except Exception:
            pass

    dpg.set_viewport_resize_callback(lambda: layout())
    layout()
    rebuild_local_dirs()
    render_inventory()
    add_log("Demarrage.", (120, 200, 255))
    update_disk()
    if CRD["url"] and (CRD["password"] or CRD["user"]):
        ui(do_connect)          # reconnexion automatique au lancement
    if EMB["url"] and EMB["api_key"]:
        ui(do_load_emby)
    if CFG.get("browser_enabled"):
        ui(apply_browser_server)

    while dpg.is_dearpygui_running():
        drain_ui_queue()
        run_deferred()
        dpg.render_dearpygui_frame()

    _stop_refresh.set()
    _stop_watch.set()
    stop_browser_server()
    persist()
    dpg.destroy_context()


if __name__ == "__main__":
    main()
