# Stunt Car Racer — version native (C++17 / SDL2)
cmake_minimum_required(VERSION 3.16)
project(stunt_car_racer CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(WIN32)
  add_compile_definitions(NOMINMAX)   # windows.h : pas de macros min / max (std::min, std::max)
endif()
if(MSVC)
  add_compile_options(/utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- Musashi (CPU 68000, licence MIT), version figée
include(FetchContent)
FetchContent_Declare(musashi
  GIT_REPOSITORY https://github.com/kstenerud/Musashi.git
  GIT_TAG 313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd)
FetchContent_GetProperties(musashi)
if(NOT musashi_POPULATED)
  FetchContent_Populate(musashi)
endif()
set(MUSASHI ${musashi_SOURCE_DIR})
set(GEN ${CMAKE_BINARY_DIR}/m68kgen)
file(MAKE_DIRECTORY ${GEN})

# configuration : crochet d'instruction (TOS simulé) et acquittement d'interruption.
# m68kconf.h utilise des #ifndef : réglages et prototypes passés par un en-tête inclus de force.
file(WRITE ${GEN}/scr_hooks.h "void scr_instr_hook(unsigned int pc);
int scr_int_ack(int level);
#define M68K_INSTRUCTION_HOOK 2
#define M68K_INSTRUCTION_CALLBACK(pc) scr_instr_hook(pc)
#define M68K_EMULATE_INT_ACK 2
#define M68K_INT_ACK_CALLBACK(l) scr_int_ack(l)
")

# m68kmake génère m68kops.c ; en compilation croisée, fournir un m68kmake compilé pour l'hôte (-DSCR_M68KMAKE=...)
if(SCR_M68KMAKE)
  set(M68KMAKE ${SCR_M68KMAKE})
else()
  add_executable(m68kmake ${MUSASHI}/m68kmake.c)
  set(M68KMAKE m68kmake)
endif()
add_custom_command(OUTPUT ${GEN}/m68kops.c ${GEN}/m68kops.h
  COMMAND ${M68KMAKE} ${GEN} ${MUSASHI}/m68k_in.c
  DEPENDS ${M68KMAKE} ${MUSASHI}/m68k_in.c)
add_library(musashi STATIC ${MUSASHI}/m68kcpu.c ${MUSASHI}/m68kdasm.c ${MUSASHI}/softfloat/softfloat.c ${GEN}/m68kops.c)
target_include_directories(musashi PUBLIC ${GEN} ${MUSASHI})
if(MSVC)
  target_compile_options(musashi PRIVATE /FI${GEN}/scr_hooks.h)
else()
  target_compile_options(musashi PRIVATE -include ${GEN}/scr_hooks.h)
endif()
if(NOT MSVC)
  target_compile_options(musashi PRIVATE -w)
  target_link_libraries(musashi PUBLIC m)
endif()

# --- cœur du jeu (sans dépendance graphique)
add_library(scrcore STATIC src/scrdata.cpp src/machine.cpp src/track.cpp src/tnt.cpp src/render3d.cpp src/scenery.cpp src/crowd.cpp src/sound.cpp src/hdview.cpp src/hdassets.cpp src/ym2149.cpp src/net.cpp src/wstunnel.cpp src/banner.cpp)
if(NOT EMSCRIPTEN)   # navigateur : pas de fils d'exécution (sinon mémoire partagée exigée)
  find_package(Threads REQUIRED)
  target_link_libraries(scrcore PUBLIC Threads::Threads)
endif()
target_include_directories(scrcore PUBLIC src)
target_link_libraries(scrcore PUBLIC musashi)
if(WIN32)
  target_link_libraries(scrcore PUBLIC ws2_32 winhttp)
endif()

# --- serveur relais (multijoueur en ligne), programme autonome pour Windows ou Linux
# --- version navigateur (WebAssembly, Emscripten) : le jeu seul (scr.js + scr.wasm), page web/index.html ;
# SDL2 d'Emscripten, boucle du jeu rendue au navigateur à chaque image (Asyncify), réglages et image
# disque gardés dans le navigateur (IndexedDB) ; m68kmake vient d'une compilation pour la machine hôte
if(EMSCRIPTEN)
  if(NOT SCR_M68KMAKE)
    message(FATAL_ERROR "version web : -DSCR_M68KMAKE=chemin/vers/m68kmake (compilé pour la machine hôte)")
  endif()
  target_compile_options(scrcore PRIVATE -fexceptions)
  add_executable(scr src/main.cpp src/gpu3d.cpp)
  # main.cpp sans exceptions : avec, presque chaque appel passe par un intermédiaire JavaScript
  # (invoke_*) qu'Asyncify ne sait pas suspendre (« invalid state » au premier rendu de la main)
  target_compile_options(scr PRIVATE -sUSE_SDL=2)
  target_link_libraries(scr scrcore)
  target_link_options(scr PRIVATE -sUSE_SDL=2 -fexceptions -O3
    -sASYNCIFY -sASYNCIFY_IGNORE_INDIRECT=1 -sASYNCIFY_STACK_SIZE=1048576
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=134217728 -sSTACK_SIZE=2097152
    -sINVOKE_RUN=0 -sEXIT_RUNTIME=1 -sENVIRONMENT=web -sFORCE_FILESYSTEM=1
    "-sEXPORTED_RUNTIME_METHODS=['callMain','FS']" -lidbfs.js --profiling-funcs)
  return()
endif()

add_executable(scr_relay server/scr_relay.cpp)
if(WIN32)
  target_link_libraries(scr_relay ws2_32)
  if(MSVC)
    set_property(TARGET scr_relay PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")   # sans DLL d'exécution
  endif()
endif()

# --- test / outil sans fenêtre
add_executable(scr_headless src/headless.cpp)
target_link_libraries(scr_headless scrcore)

# --- jeu (SDL2)
option(SCR_WITH_SDL "Construire le jeu SDL2" ON)
if(SCR_WITH_SDL)
  if(SCR_SDL2_INCLUDE AND SCR_SDL2_LIB)          # chemins manuels (ex. bibliothèque système sans paquet -dev)
    add_executable(scr src/main.cpp src/gpu3d.cpp)
    target_include_directories(scr PRIVATE ${SCR_SDL2_INCLUDE})
    target_link_libraries(scr scrcore ${SCR_SDL2_LIB})
    if(SCR_GL_LIB)                                 # OpenGL (rendu 3D sur la carte graphique)
      target_link_libraries(scr ${SCR_GL_LIB})
    endif()
  else()
    find_package(SDL2 REQUIRED)
    add_executable(scr WIN32 src/main.cpp src/gpu3d.cpp)
    if(TARGET SDL2::SDL2main)
      target_link_libraries(scr SDL2::SDL2main)
    endif()
    find_package(OpenGL REQUIRED)                  # rendu 3D sur la carte graphique
    target_link_libraries(scr scrcore SDL2::SDL2 OpenGL::GL)
  endif()
endif()
