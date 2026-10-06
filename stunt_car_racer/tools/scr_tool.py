#!/usr/bin/env python3
"""
scr_tool.py - outils de rétro-ingénierie pour Stunt Car Racer (Atari ST, Micro Style 1989).

Sous-commandes :
  ls      IMAGE.st                 liste les fichiers de l'image disque (FAT12)
  extract IMAGE.st DOSSIER         extrait tous les fichiers
  unpack  FICHIER.PRG SORTIE       décompresse un exécutable JEK Packer / ByteKiller
  game    IMAGE.st SORTIE.prg      extrait + décompresse GAME.PUT et isole le programme du jeu
  image   IMAGE.st SORTIE.bin      programme du jeu relogé à $10100 (TEXT+DATA brut, pour dasm)
  tracks  IMAGE.st SORTIE.json     décode les 8 circuits (géométrie complète) en JSON
  preview IMAGE.st SORTIE.png      dessine les 8 circuits vus de dessus (nécessite Pillow)

Aucune donnée du jeu n'est incluse dans ce dépôt : tout est lu depuis votre propre image disque.
"""
import json
import struct
import sys

# --------------------------------------------------------------------------- FAT12 (.st)

def fat_list(img):
    """Retourne {chemin: bytes} pour tous les fichiers d'une image .st (FAT12)."""
    bps, spc, res, nfat, ndir, nsec, media, spf = struct.unpack('<HBHBHHBH', img[11:24])
    spc = spc or 1          # cette compilation a un BPB avec 0 secteur/cluster (TOS l'accepte)
    fat = img[res * bps:(res + spf) * bps]
    root = (res + nfat * spf) * bps
    data = root + ndir * 32
    csize = spc * bps

    def nxt(n):
        o = n * 3 // 2
        v = fat[o] | fat[o + 1] << 8
        return v >> 4 if n & 1 else v & 0xFFF

    def chain(cl):
        out = bytearray()
        seen = 0
        while 2 <= cl < 0xFF0 and seen < 4096:
            out += img[data + (cl - 2) * csize:data + (cl - 1) * csize]
            cl = nxt(cl)
            seen += 1
        return bytes(out)

    files = {}

    def walk(buf, path):
        for i in range(len(buf) // 32):
            e = buf[i * 32:i * 32 + 32]
            if e[0] == 0:
                break
            if e[0] in (0xE5, 0x2E):
                continue
            name = e[:8].decode('latin1').rstrip()
            ext = e[8:11].decode('latin1').rstrip()
            if ext:
                name += '.' + ext
            attr = e[11]
            cl, size = struct.unpack('<HI', e[26:32])
            if attr & 0x08:
                continue
            if attr & 0x10:
                walk(chain(cl), path + name + '/')
            else:
                files[path + name] = chain(cl)[:size]

    walk(img[root:data], '')
    return files

# --------------------------------------------------------------------------- JEK Packer 1.3 (ByteKiller)

def bytekiller_unpack(buf, end):
    """Décompression arrière (format ByteKiller, utilisé par JEK Packer 1.3).
    `end` = offset juste après les données compressées. Renvoie (données, checksum) ;
    un checksum nul signifie que la décompression est valide."""
    p = end

    def rl():
        nonlocal p
        p -= 4
        return struct.unpack('>I', buf[p:p + 4])[0]

    length = rl()
    crc = rl()
    d0 = rl()
    crc ^= d0
    out = bytearray(length)
    q = length
    st = [d0, crc]

    def bit():
        d = st[0]
        c = d & 1
        d >>= 1
        if d == 0:
            n = rl()
            st[1] ^= n
            c = n & 1
            d = (n >> 1) | 0x80000000
        st[0] = d
        return c

    def bits(n):
        v = 0
        for _ in range(n):
            v = (v << 1) | bit()
        return v

    while q > 0:
        if bit() == 0:
            if bit() == 0:                     # 00 : 1..8 octets littéraux
                for _ in range(bits(3) + 1):
                    q -= 1
                    out[q] = bits(8)
            else:                              # 01 : copie 2 octets, offset 8 bits
                off = bits(8)
                for _ in range(2):
                    q -= 1
                    out[q] = out[q + off]
        else:
            t = bits(2)
            if t == 3:                         # 1 11 : 9..264 littéraux
                for _ in range(bits(8) + 9):
                    q -= 1
                    out[q] = bits(8)
            else:
                if t == 2:                     # 1 10 : longueur 8 bits, offset 12 bits
                    cnt = bits(8) + 1
                    off = bits(12)
                else:                          # 1 0x : 3..4 octets, offset 9..10 bits
                    cnt = t + 3
                    off = bits(9 + t)
                for _ in range(cnt):
                    q -= 1
                    out[q] = out[q + off]
    return bytes(out), st[1]


def unpack_prg(data):
    tlen, dlen = struct.unpack('>II', data[2:10])
    return bytekiller_unpack(data, 0x1C + tlen + dlen)


def game_program(img):
    """Image .st -> programme du jeu (le PRG interne de GAME.PUT, sans l'intro du cracker)."""
    files = fat_list(img)
    key = next(k for k in files if k.upper().endswith('GAME.PUT'))
    out, crc = unpack_prg(files[key])
    if crc:
        raise ValueError('GAME.PUT : checksum de décompression invalide')
    tlen, dlen = struct.unpack('>II', out[2:10])
    # le segment DATA de GAME.PUT contient un second exécutable (le jeu)
    i = out.find(b'\x60\x1a', 0x1C + tlen - 0x40)
    if i < 0:
        raise ValueError('programme interne introuvable')
    return out[i:]

# --------------------------------------------------------------------------- décodage des circuits
#
# Le jeu est une traduction quasi mécanique du code 6502 de la version C64 : les tables
# sont restées au format 6502 (pointeurs 16 bits petit-boutistes).  L'adresse 6502 $B100
# correspond à l'adresse $13670 dans le jeu chargé (base TEXT = $10100).
# Routine d'origine : $48390 (décodage), $4D3D2 (paramètres de forme), $4881C (profils).

TEXT_BASE = 0x10100
BLOB = 0x13670
TRACK_TABLE = 0x13790      # 8 pointeurs 6502 vers les circuits
SHAPE_TABLE = 0x13670      # 16 pointeurs vers les formes de pièces (sections gauche/droite)
PROFILE_TABLE = 0x13690    # pointeurs vers les profils de hauteur
SPECIAL_BBA = 0x48802      # tables pour les types 12..15
SPECIAL_C1E = 0x48804
CELL = 0x800               # une case de la grille 16x16 = 0x800 unités
NAMES = ["LITTLE RAMP", "STEPPING STONES", "HUMP BACK", "BIG RAMP",
         "SKI JUMP", "DRAW BRIDGE", "HIGH JUMP", "ROLLER COASTER"]


class Mem:
    """Accès au programme du jeu par adresse mémoire ST (après relocation à $10100)."""

    def __init__(self, prg):
        self.img = prg
        self.base = TEXT_BASE - 0x1C

    def b(self, a):
        return self.img[a - self.base]

    def le(self, a):
        return self.b(a) | self.b(a + 1) << 8


def p6502(addr):
    return BLOB + (addr - 0xB100)


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def decode_track(m, track):
    """Port fidèle de la routine $48390. Validé octet par octet contre la mémoire du jeu."""
    ptr = p6502(m.le(TRACK_TABLE + 2 * track))
    pos = 0

    def fetch():
        nonlocal pos
        v = m.b(ptr + pos)
        pos += 1
        return v

    hdr = [fetch() for _ in range(4)]          # $11114..$11117 : nb pièces, ?, ?, ?
    npieces = hdr[0]
    lo = fetch()
    hi = fetch()
    hl = hr = (hi << 8) | lo                   # hauteurs courantes gauche/droite ($109E0/$109E2)
    c6 = dist = repeat = prev_type = prev_coord = 0
    pieces = []
    while len(pieces) < npieces:
        if repeat:                             # pièce répétée : même type, case suivante
            repeat -= 1
            t = prev_type
            ce6 = dirb = t
            if t & 0x10:
                dirb ^= 0xC0
            c = prev_coord
            step = 1 if dirb & 0x40 else 0x10
            c = (c - step if dirb & 0x80 else c + step) & 0xFF
        else:
            t = fetch()
            if (t & 0xF) == 0xF:               # $nF : répéter la pièce précédente n fois
                repeat = t >> 4
                continue
            ce6 = dirb = prev_type = t
            c = fetch()                        # case de la grille : Z<<4 | X
        prev_coord = c
        b905 = 0x80 if c6 & 2 else 0
        if (dirb & 0xF) >= 12:
            ce6 &= 0xF0
            bba = m.b(SPECIAL_BBA + (dirb & 0xF))
            v = m.b(SPECIAL_C1E + (dirb & 0xF))
        else:
            bba = fetch()                      # profil de hauteur gauche
            v = bba if dirb & 0x20 else fetch()  # profil droit (= gauche si bit 5)
        c1e = (v & 0x7F) | b905
        pL = m.le(PROFILE_TABLE + ((bba << 1) & 0xFF))
        pR = m.le(PROFILE_TABLE + ((c1e << 1) & 0xFF))
        neg = bool(bba & 0x80)
        c6 = 2 if c1e & 0x80 else 0
        shape = p6502(m.le(SHAPE_TABLE + 2 * (ce6 & 0xF)))
        nsec = m.b(shape + m.b(shape))        # nombre de points (2 par section)
        segs = ((nsec >> 1) - 1) & 0xFF
        startL = (hl - profile(m, pL, neg, 0)) & 0xFFFF
        hl = (startL + profile(m, pL, neg, segs)) & 0xFFFF
        startR = (hr - profile(m, pR, neg, 0)) & 0xFFFF
        hr = (startR + profile(m, pR, neg, segs)) & 0xFFFF
        pieces.append(dict(coord=c, x=c & 0xF, z=c >> 4, type=ce6, bba=bba, c1e=c1e,
                           dist=(dist << 5) & 0xFFFF, hL=startL, hR=startR,
                           segs=segs, profL=pL, profR=pR, neg=neg))
        c6 = (c6 + nsec - 2) & 2
        dist = (dist + segs) & 0xFFFF
    tail = [m.b(ptr + pos + i) for i in range(6)]
    return dict(header=hdr, pieces=pieces, tail=tail)


def profile_flag(m, ptr, neg, i):
    """bit 7 de l'octet de profil : section sans revêtement (trou, saut)."""
    a = p6502(ptr)
    return bool(m.b(a + (2 * i if neg else i)) & 0x80)


def profile(m, ptr, neg, i):
    a = p6502(ptr)
    if neg:
        return ((m.b(a + 2 * i) & 0x7F) << 8) | m.b(a + 2 * i + 1)
    x = m.b(a + i)
    return ((x & 0xF) << 8) | ((x << 1) & 0xE0)


def shape_points(m, typ):
    a = p6502(m.le(SHAPE_TABLE + 2 * (typ & 0xF)))
    o = m.b(a)
    n = m.b(a + o)
    base = a + o + 7
    return [(s16(m.le(base + 4 * i)), s16(m.le(base + 4 * i + 2))) for i in range(n)]


# orientation : bits 7-6 du type = quart de tour ; bit 4 = pièce parcourue à l'envers
_ROT = [lambda x, z: (x, z), lambda x, z: (z, CELL - x),
        lambda x, z: (CELL - x, CELL - z), lambda x, z: (CELL - z, x)]


def build_track(m, track):
    t = decode_track(m, track)
    pieces = []
    for p in t['pieces']:
        typ = p['type']
        pts = shape_points(m, typ)
        secs = [(pts[2 * k], pts[2 * k + 1]) for k in range(len(pts) // 2)]
        hs = [((p['hL'] + profile(m, p['profL'], p['neg'], k)) & 0xFFFF,
               (p['hR'] + profile(m, p['profR'], p['neg'], k)) & 0xFFFF) for k in range(len(secs))]
        flags = [(profile_flag(m, p['profL'], p['neg'], k), profile_flag(m, p['profR'], p['neg'], k))
                 for k in range(len(secs))]
        rot = _ROT[(typ >> 6) & 3]
        if typ & 0x10:                         # parcours inverse : gauche/droite permutés
            secs = [(r, l) for l, r in secs[::-1]]
        out = []
        for k, ((lx, lz), (rx, rz)) in enumerate(secs):
            lx, lz = rot(lx, lz)
            rx, rz = rot(rx, rz)
            out.append([lx + p['x'] * CELL, hs[k][0], lz + p['z'] * CELL,
                        rx + p['x'] * CELL, hs[k][1], rz + p['z'] * CELL,
                        int(flags[k][0]) | int(flags[k][1]) << 1])
        pieces.append(dict(type=typ, shape=typ & 0xF, reversed=bool(typ & 0x10),
                           dir=(typ >> 6) & 3, cell=[p['x'], p['z']], dist=p['dist'],
                           profile_left=p['bba'], profile_right=p['c1e'], sections=out))
    return dict(name=NAMES[track], index=track, header=t['header'], tail=t['tail'],
                cell_size=CELL, pieces=pieces)


def relocate(prg, base=TEXT_BASE):
    """Applique la table de relocation TOS : renvoie TEXT+DATA tels qu'en mémoire à `base`."""
    tlen, dlen, blen, slen = struct.unpack('>IIII', prg[2:18])
    img = bytearray(prg[0x1C:0x1C + tlen + dlen])
    r = 0x1C + tlen + dlen + slen
    off = struct.unpack('>I', prg[r:r + 4])[0]
    r += 4
    if off:
        a = off
        while True:
            v = struct.unpack('>I', img[a:a + 4])[0]
            img[a:a + 4] = struct.pack('>I', (v + base) & 0xFFFFFFFF)
            c = prg[r]
            r += 1
            if c == 0:
                break
            while c == 1:
                a += 254
                c = prg[r]
                r += 1
            a += c
    return bytes(img)


def all_tracks(img):
    m = Mem(game_program(img))
    return [build_track(m, i) for i in range(8)]

# --------------------------------------------------------------------------- CLI

def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    cmd, src = argv[1], argv[2]
    data = open(src, 'rb').read()
    if cmd == 'ls':
        for k, v in fat_list(data).items():
            print('%-20s %8d' % (k, len(v)))
    elif cmd == 'extract':
        import os
        for k, v in fat_list(data).items():
            path = os.path.join(argv[3], *k.split('/'))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, 'wb').write(v)
            print(path)
    elif cmd == 'unpack':
        out, crc = unpack_prg(data)
        open(argv[3], 'wb').write(out)
        print('%d octets, checksum %s' % (len(out), 'OK' if crc == 0 else 'INVALIDE'))
    elif cmd == 'game':
        open(argv[3], 'wb').write(game_program(data))
    elif cmd == 'image':
        open(argv[3], 'wb').write(relocate(game_program(data)))
    elif cmd == 'tracks':
        json.dump(dict(source='Stunt Car Racer (Atari ST)', tracks=all_tracks(data)),
                  open(argv[3], 'w'), separators=(',', ':'))
        print('8 circuits écrits dans', argv[3])
    elif cmd == 'preview':
        preview(all_tracks(data), argv[3])
    else:
        print(__doc__)
        return 1
    return 0


def preview(tracks, out):
    from PIL import Image, ImageDraw
    im = Image.new('RGB', (4 * 260, 2 * 280), (30, 30, 30))
    d = ImageDraw.Draw(im)
    sc = 256 / (16 * CELL)
    for t in tracks:
        ox = (t['index'] % 4) * 260 + 2
        oy = (t['index'] // 4) * 280 + 20
        hs = [s[1] for p in t['pieces'] for s in p['sections']]
        lo, hi = min(hs), max(hs)
        for p in t['pieces']:
            S = p['sections']
            for a, b in zip(S, S[1:]):
                c = int(60 + 195 * ((a[1] + a[4]) / 2 - lo) / max(1, hi - lo))
                d.polygon([(ox + a[0] * sc, oy + (16 * CELL - a[2]) * sc), (ox + a[3] * sc, oy + (16 * CELL - a[5]) * sc),
                           (ox + b[3] * sc, oy + (16 * CELL - b[5]) * sc), (ox + b[0] * sc, oy + (16 * CELL - b[2]) * sc)],
                          fill=(c, c // 2, 255 - c))
        d.text((ox, oy - 16), '%d %s' % (t['index'], t['name']), fill=(255, 255, 255))
    im.save(out)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
