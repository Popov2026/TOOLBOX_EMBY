// headless.cpp — exécute le jeu sans fenêtre (tests, captures).
// usage : scr_headless DISQUE.st [--track N] [--frames N] [--up] [--shot fichier.ppm] [--bench]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "machine.hpp"

int main(int argc, char **argv) {
  std::string disk, shot;
  int track = -1, frames = 500;
  bool up = false;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--track") && i + 1 < argc) track = std::atoi(argv[++i]) - 1;
    else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
    else if (!std::strcmp(argv[i], "--up")) up = true;
    else disk = argv[i];
  }
  try {
    auto bytes = scr::readFile(disk);
    auto t0 = std::chrono::steady_clock::now();
    scr::Machine m(bytes);
    if (track < 0) m.coldStart(); else m.startPractice(track);
    auto t1 = std::chrono::steady_clock::now();
    scr::Joystick j; j.up = up;
    m.setJoystick(j);
    for (int f = 0; f < frames; f++) m.runFrame();
    auto t2 = std::chrono::steady_clock::now();
    double ds = std::chrono::duration<double>(t1 - t0).count(), rs = std::chrono::duration<double>(t2 - t1).count();
    std::printf("démarrage %.2f s ; %d trames (%.1f s de jeu) en %.3f s = %.0f× le temps réel\n", ds, frames, frames / 50.0, rs,
                frames / 50.0 / rs);
    std::printf("pièce %d  position x=%.2f y=%.2f z=%.2f  lacet=%.1f°\n", m.b(0x10906), m.l(0x10ac2) / 65536.0,
                m.l(0x10ac6) / 65536.0, m.l(0x10aca) / 65536.0, m.w(0x10ad0) * 360.0 / 65536);
    if (!shot.empty()) {
      static uint32_t px[320 * 200];
      m.screenARGB(px);
      FILE *f = std::fopen(shot.c_str(), "wb");
      std::fprintf(f, "P6\n320 200\n255\n");
      for (uint32_t p : px) { std::fputc((p >> 16) & 255, f); std::fputc((p >> 8) & 255, f); std::fputc(p & 255, f); }
      std::fclose(f);
    }
  } catch (std::exception &e) {
    std::fprintf(stderr, "Erreur : %s\n", e.what());
    return 1;
  }
  return 0;
}
