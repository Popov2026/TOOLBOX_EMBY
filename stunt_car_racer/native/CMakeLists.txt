# Stunt Car Racer — version native (C++17 / SDL2)
cmake_minimum_required(VERSION 3.16)
project(stunt_car_racer CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
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
add_library(scrcore STATIC src/scrdata.cpp src/machine.cpp src/track.cpp src/render3d.cpp src/hdview.cpp src/hdassets.cpp src/ym2149.cpp src/net.cpp src/banner.cpp)
find_package(Threads REQUIRED)
target_link_libraries(scrcore PUBLIC Threads::Threads)
target_include_directories(scrcore PUBLIC src)
target_link_libraries(scrcore PUBLIC musashi)
if(WIN32)
  target_link_libraries(scrcore PUBLIC ws2_32)
endif()

# --- serveur relais (multijoueur en ligne), pour un serveur Linux
if(NOT WIN32)
  add_executable(scr_relay server/scr_relay.cpp)
endif()

# --- test / outil sans fenêtre
add_executable(scr_headless src/headless.cpp)
target_link_libraries(scr_headless scrcore)

# --- jeu (SDL2)
option(SCR_WITH_SDL "Construire le jeu SDL2" ON)
if(SCR_WITH_SDL)
  if(SCR_SDL2_INCLUDE AND SCR_SDL2_LIB)          # chemins manuels (ex. bibliothèque système sans paquet -dev)
    add_executable(scr src/main.cpp)
    target_include_directories(scr PRIVATE ${SCR_SDL2_INCLUDE})
    target_link_libraries(scr scrcore ${SCR_SDL2_LIB})
  else()
    find_package(SDL2 REQUIRED)
    add_executable(scr WIN32 src/main.cpp)
    if(TARGET SDL2::SDL2main)
      target_link_libraries(scr SDL2::SDL2main)
    endif()
    target_link_libraries(scr scrcore SDL2::SDL2)
  endif()
endif()
