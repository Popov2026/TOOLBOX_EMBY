#!/usr/bin/env python3
"""
stemu_script.py - pilote l'émulateur stemu à partir d'un petit script de scénario.

usage : stemu_script.py SCENARIO.txt --disk IMAGE.st --dir FICHIERS_EXTRAITS [options stemu...]

Commandes du scénario (une par ligne, '#' = commentaire) :
  wait N          avance de N images (50 images = 1 s)
  key SC [SC...]  appuie puis relâche les touches (scancodes ST en hexa, ex. 39 = espace, 1C = entrée)
  joy MASK [N]    joystick (bit0 haut, 1 bas, 2 gauche, 3 droite, 7 feu) pendant N images (défaut 6)
  joyon MASK      joystick maintenu jusqu'à nouvel ordre
  shot FICHIER    capture d'écran (PPM) à l'image courante
  every N:PREFIX  capture toutes les N images
  end             fin du scénario

Exemple (atteindre l'entraînement sur Little Ramp) : voir scenarios/practice_little_ramp.txt
"""
import os
import subprocess
import sys


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    here = os.path.dirname(os.path.abspath(__file__))
    script = open(sys.argv[1]).read().split('\n')
    rest = sys.argv[2:]
    disk = files = None
    extra = []
    i = 0
    while i < len(rest):
        if rest[i] == '--disk':
            disk = rest[i + 1]; i += 2
        elif rest[i] == '--dir':
            files = rest[i + 1]; i += 2
        else:
            extra.append(rest[i]); i += 1
    args = []
    f = 0
    for line in script:
        p = line.split('#')[0].split()
        if not p:
            continue
        c = p[0]
        if c == 'wait':
            f += int(p[1])
        elif c == 'key':
            for sc in p[1:]:
                args += ['--key', '%d:%s:down' % (f, sc), '--key', '%d:%s:up' % (f + 10, sc)]
                f += 25
        elif c == 'joy':
            hold = int(p[2]) if len(p) > 2 else 6
            args += ['--joy', '%d:%s' % (f, p[1]), '--joy', '%d:0' % (f + hold)]
            f += hold + 4
        elif c == 'joyon':
            args += ['--joy', '%d:%s' % (f, p[1])]
        elif c == 'shot':
            args += ['--shot', '%d:%s' % (f, p[1])]
        elif c == 'every':
            args += ['--shots-every', p[1]]
        elif c == 'end':
            break
    cmd = [os.path.join(here, 'stemu'), '--frames', str(f + 1)]
    if disk:
        cmd += ['--disk', disk]
    if files:
        cmd += ['--dir', files]
    cmd += args + extra + [os.path.join(files or '.', 'GAME.PUT')]
    r = subprocess.run(cmd, capture_output=True, text=True)
    sys.stderr.write('\n'.join(l for l in r.stderr.split('\n') if 'Floprd' not in l)[-3000:])
    print('images :', f)
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
