#!/bin/bash
# build_release.sh — zips Windows d'une version (jeu + serveur relais), compilés sous Linux avec
# llvm-mingw : StuntCarRacer-win64-vNN.zip et ServeurRelaisStuntCar-vNN.zip (serveur pour Windows et
# Linux). C'est la compilation des releases GitHub (workflow) ; elle marche aussi sur un PC Linux.
#   WORK : dossier de travail (outils téléchargés, compilations) ; OUT : dossier des zips
# Aucune donnée du jeu d'origine n'est incluse : chaque joueur fournit son image disque .st.
# Images HD : celles de assets/hd (dépôt stuntcar-racer) si ce dossier existe.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)   # dossier native
WORK=$(mkdir -p "${WORK:-$PWD/build-release}" && cd "${WORK:-$PWD/build-release}" && pwd)
OUT=$(mkdir -p "${OUT:-$PWD/dist}" && cd "${OUT:-$PWD/dist}" && pwd)
LMV=20241030 SDLV=2.30.8
LM=$WORK/llvm-mingw-$LMV-ucrt-ubuntu-20.04-x86_64
SDL=$WORK/SDL2-$SDLV/x86_64-w64-mingw32
JOBS=$(nproc)

# outils : compilateur Windows (llvm-mingw) et SDL2 pour MinGW
[ -d "$LM" ] || curl -fsSL "https://github.com/mstorsjo/llvm-mingw/releases/download/$LMV/llvm-mingw-$LMV-ucrt-ubuntu-20.04-x86_64.tar.xz" | tar xJ -C "$WORK"
[ -d "$SDL" ] || curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-$SDLV/SDL2-devel-$SDLV-mingw.tar.gz" | tar xz -C "$WORK"
VER=$(sed -n 's/^#define SCR_VERSION "\(v[0-9]*\)".*/\1/p' "$ROOT/src/main.cpp")
[ -n "$VER" ] || { echo "version introuvable dans src/main.cpp" >&2; exit 1; }
echo "Stunt Car Racer $VER"

# m68kmake (générateur du CPU 68000 Musashi) pour la machine qui compile
cmake -S "$ROOT" -B "$WORK/host" -DSCR_WITH_SDL=OFF > "$WORK/host.log"
cmake --build "$WORK/host" --target m68kmake -j"$JOBS" >> "$WORK/host.log"

# compilation Windows
cat > "$WORK/tc.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER $LM/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER $LM/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER $LM/bin/x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH $SDL)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
B=$WORK/win
cmake -S "$ROOT" -B "$B" -DCMAKE_TOOLCHAIN_FILE="$WORK/tc.cmake" -DSCR_M68KMAKE="$WORK/host/m68kmake" \
      -DSDL2_DIR="$SDL/lib/cmake/SDL2" -DCMAKE_EXE_LINKER_FLAGS=-static > "$WORK/win.log"
cmake --build "$B" -j"$JOBS" >> "$WORK/win.log"
# édition de liens finale : tout en statique (SDL2 compris, aucune DLL à fournir), sans symboles
L=$LM/bin
( cd "$B"
  "$L/x86_64-w64-mingw32-clang++" -O3 -static -mwindows -s -Wl,--whole-archive CMakeFiles/scr.dir/objects.a -Wl,--no-whole-archive -o scr.exe \
    "$SDL/lib/libSDL2main.a" libscrcore.a "$SDL/lib/libSDL2.a" -lopengl32 -lshell32 -Wl,--undefined=WinMain -lpthread libmusashi.a -lm \
    -lws2_32 -lwinhttp -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lversion -lsetupapi -lole32 -loleaut32 -luuid -ladvapi32
  "$L/x86_64-w64-mingw32-clang++" -O3 -static -s -Wl,--whole-archive CMakeFiles/scr_relay.dir/objects.a -Wl,--no-whole-archive -o scr_relay.exe -lws2_32
  "$L/x86_64-w64-mingw32-clang++" -O3 -static -s -Wl,--whole-archive CMakeFiles/scr_headless.dir/objects.a -Wl,--no-whole-archive \
    -o scr_headless.exe libscrcore.a libmusashi.a -lws2_32 -lwinhttp )
# serveur relais pour Linux (x86-64, statique)
g++ -O2 -std=c++17 -static -s "$ROOT/server/scr_relay.cpp" -o "$WORK/scr_relay"

# paquets
P=$WORK/pkg
rm -rf "$P"; mkdir -p "$P/StuntCarRacer/DISK" "$P/StuntCarRacer/hd" "$P/StuntCarRacer/TNT" "$P/StuntCarRacer/serveur" "$P/ServeurRelaisStuntCar"
G=$P/StuntCarRacer
cp "$B/scr.exe" "$B/scr_headless.exe" "$ROOT/README.txt" "$ROOT/README_MULTIPLAYER.txt" "$G/"
cp "$ROOT/scr.ini.example" "$G/scr.ini.exemple"
cp "$ROOT/README_HD.txt" "$G/hd/"; cp "$ROOT/README_DISK.txt" "$G/DISK/README.txt"; cp "$ROOT/README_TNT.txt" "$G/TNT/"
# images HD (dépôt stuntcar-racer : assets/hd, ou HD_ASSETS=dossier) livrées dans le dossier hd du jeu
HD_ASSETS=${HD_ASSETS:-$ROOT/../assets/hd}
if [ -d "$HD_ASSETS" ]; then cp "$HD_ASSETS"/*.png "$HD_ASSETS"/hd.ini "$G/hd/"; echo "images HD : $(ls "$HD_ASSETS"/*.png | wc -l)"; fi
for d in "$G/serveur" "$P/ServeurRelaisStuntCar"; do
  cp "$ROOT/server/README_SERVER.txt" "$ROOT/server/scr-relay.service" "$ROOT/server/scr_relay.cpp" "$ROOT/server/scr_relay.ini" "$B/scr_relay.exe" "$WORK/scr_relay" "$d/"
done
rm -f "$OUT/StuntCarRacer-win64-$VER.zip" "$OUT/ServeurRelaisStuntCar-$VER.zip"
( cd "$P" && zip -q -X -r "$OUT/StuntCarRacer-win64-$VER.zip" StuntCarRacer && zip -q -X -r "$OUT/ServeurRelaisStuntCar-$VER.zip" ServeurRelaisStuntCar )
echo "VERSION=$VER" > "$OUT/version.txt"
ls -l "$OUT"
