#!/bin/sh
# Compile l'émulateur de test stemu (CPU 68000 : Musashi, licence MIT, récupéré depuis GitHub).
set -e
cd "$(dirname "$0")"
MUSASHI_REV=313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd
[ -d Musashi ] || git clone https://github.com/kstenerud/Musashi.git
(cd Musashi && git checkout -q $MUSASHI_REV)
mkdir -p build/gen build/conf
gcc -O2 -o build/m68kmake Musashi/m68kmake.c
build/m68kmake build/gen Musashi/m68k_in.c >/dev/null
# configuration : crochet d'instruction (HLE du TOS) et acquittement d'interruption (MFP)
sed 's|#define M68KCONF__HEADER|#define M68KCONF__HEADER\nvoid instr_hook(unsigned int pc);\nint int_ack(int level);|' Musashi/m68kconf.h > build/conf/m68kconf.h
SRC="Musashi/m68kdasm.c build/gen/m68kops.c Musashi/m68kcpu.c Musashi/softfloat/softfloat.c"
gcc -O2 -w -Ibuild/conf -Ibuild/gen -IMusashi \
  "-DM68K_INSTRUCTION_HOOK=2" "-DM68K_INSTRUCTION_CALLBACK(pc)=instr_hook(pc)" \
  "-DM68K_EMULATE_INT_ACK=2" "-DM68K_INT_ACK_CALLBACK(l)=int_ack(l)" \
  -o stemu stemu.c $SRC -lm
gcc -O2 -w -Ibuild/gen -IMusashi -o dasm dasm.c $SRC -lm
echo "OK : ./stemu et ./dasm"
