#!/usr/bin/env python3
"""Génère des flammes d'exemple pour le mode HD (dossier hd/) : feu procédural sur fond noir
(mélange additif), 6 images d'animation pour chaque sprite de flamme du boost.

    python3 hd_flammes.py DOSSIER_HD

Sprites du boost : 6, 7, 49 (échappements gauches) et 8, 9, 50 (droits), 64x28 pixels d'origine.
Les images sont produites en 256x112 (x4) ; remplacez-les par les vôtres (même cadrage).
"""
import os
import sys

import numpy as np
from PIL import Image

W, H = 256, 112
# sorties des 4 échappements gauches (pixels du sprite d'origine) et taille relative des flammes
TIPS = [(21, 27, 1.0), (38, 20, 0.85), (50, 14, 0.7), (59.5, 9, 0.55)]


def noise(h, w, seed, octaves=5):
    r = np.random.default_rng(seed)
    out = np.zeros((h, w))
    for o in range(octaves):
        f = 2 ** o
        g = r.random((4 * f + 2, 4 * f + 2))
        ys, xs = np.linspace(0, 4 * f, h), np.linspace(0, 4 * f, w)
        y0, x0 = ys.astype(int), xs.astype(int)
        fy, fx = (ys - y0)[:, None], (xs - x0)[None, :]
        fy, fx = fy * fy * (3 - 2 * fy), fx * fx * (3 - 2 * fx)
        a, b, c, d = g[y0][:, x0], g[y0][:, x0 + 1], g[y0 + 1][:, x0], g[y0 + 1][:, x0 + 1]
        out += ((a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy) / 2 ** o
    return out / 1.9


def fire(t):
    """dégradé du corps chauffé : rouge sombre -> orange -> jaune -> blanc bleuté"""
    t = np.clip(t, 0, 1)
    r = np.clip(t * 3, 0, 1)
    g = np.clip(t * 3 - 0.9, 0, 1)
    b = np.clip(t * 3 - 2.0, 0, 1) * 0.9 + np.clip(1 - abs(t - 0.95) * 8, 0, 1) * 0.3
    return np.stack([r, g, b], -1)


def frame(k):
    img = np.zeros((H, W, 3))
    n = noise(H, W, 100 + k)
    yy, xx = np.mgrid[0:H, 0:W].astype(float)
    for tx, ty, s in TIPS:
        tx, ty = tx * 4, ty * 4
        length, radius = 95 * s, 16 * s
        dy = (ty - yy) / length
        dx = (xx - tx - dy * length * 0.35) / (radius * (0.6 + 1.4 * np.clip(dy, 0, 1)))
        shape = np.clip(1 - dx ** 2, 0, 1) * np.clip(1 - dy, 0, 1) * (dy > -0.08)
        t = shape * np.clip(n * 1.6 - 0.25 + 0.5 * (1 - np.clip(dy, 0, 1)), 0, 1) * 1.25
        img = np.maximum(img, fire(t) * t[..., None] ** 0.6)
    return Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "hd"
    os.makedirs(out, exist_ok=True)
    for k in range(6):
        left = frame(k)
        right = left.transpose(Image.FLIP_LEFT_RIGHT)
        for sid, im in ((6, left), (7, left), (49, left), (8, right), (9, right), (50, right)):
            im.save(os.path.join(out, f"sprite_{sid:02d}_{k + 1}.png"))
    with open(os.path.join(out, "hd.ini"), "w") as f:
        f.write("; flammes du boost : fond noir -> mélange additif ; un peu plus grandes que l'original\n")
        for sid in (6, 7, 49, 8, 9, 50):
            f.write(f"[sprite_{sid:02d}]\nblend = add\nscale = 1.3\nanchor = bottom\nfps = 20\n\n")
    print("flammes écrites dans", out)


if __name__ == "__main__":
    main()
