// headless.cpp — exécute le jeu sans fenêtre (tests, captures, calage du mode HD).
// usage : scr_headless DISQUE.st [--track N] [--frames N] [--up] [--shot f.ppm]
//         [--hd f.ppm] [--cmp f.ppm] [--focal F] [--cx X] [--cy Y] [--eyeup H] [--eyefwd D] [--psign ±1] [--rsign ±1]
//         [--bench-hd N] [--fire] [--hddir DOSSIER] [--wav son.wav]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "hdview.hpp"
#include "machine.hpp"

static void savePPM(const std::string &path, const uint32_t *px, int W, int H) {
  FILE *f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int i = 0; i < W * H; i++) { std::fputc((px[i] >> 16) & 255, f); std::fputc((px[i] >> 8) & 255, f); std::fputc(px[i] & 255, f); }
  std::fclose(f);
}

int main(int argc, char **argv) {
  std::string disk, shot, hd, cmp;
  int track = -1, frames = 500, benchHd = 0;
  bool up = false, fire = false;
  std::string hdDir, wav;
  scr::HdParams hp;  // valeurs par défaut calées
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto num = [&] { return std::atof(argv[++i]); };
    if (a == "--track" && i + 1 < argc) track = std::atoi(argv[++i]) - 1;
    else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
    else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
    else if (a == "--hd" && i + 1 < argc) hd = argv[++i];
    else if (a == "--cmp" && i + 1 < argc) cmp = argv[++i];
    else if (a == "--focal" && i + 1 < argc) hp.focal = num();
    else if (a == "--cx" && i + 1 < argc) hp.cx = num();
    else if (a == "--cy" && i + 1 < argc) hp.cy = num();
    else if (a == "--aspect" && i + 1 < argc) hp.aspect = num();
    else if (a == "--eyeup" && i + 1 < argc) hp.eyeUp = num();
    else if (a == "--eyefwd" && i + 1 < argc) hp.eyeFwd = num();
    else if (a == "--psign" && i + 1 < argc) hp.pitchSign = int(num());
    else if (a == "--rsign" && i + 1 < argc) hp.rollSign = int(num());
    else if (a == "--bench-hd" && i + 1 < argc) benchHd = std::atoi(argv[++i]);
    else if (a == "--up") up = true;
    else if (a == "--fire") fire = true;
    else if (a == "--wav" && i + 1 < argc) wav = argv[++i];
    else if (a == "--hddir" && i + 1 < argc) hdDir = argv[++i];
    else disk = a;
  }
  try {
    auto bytes = scr::readFile(disk);
    auto t0 = std::chrono::steady_clock::now();
    scr::Machine m(bytes);
    if (track < 0) m.coldStart(); else m.startPractice(track);
    bool wantHd = !hd.empty() || !cmp.empty() || benchHd;
    std::unique_ptr<scr::HdView> view;
    if (wantHd) {
      m.enableLayers(true); view = std::make_unique<scr::HdView>(m); view->params = hp;
      view->loadAssets(m, hdDir);
      std::printf("%s", view->assets.report.c_str());
    }
    auto t1 = std::chrono::steady_clock::now();
    scr::Joystick j; j.up = up;
    std::vector<int16_t> pcm;
    if (!wav.empty()) m.setAudioRate(44100);
    for (int f = 0; f < frames; f++) {
      j.fire = fire && f > 600;   // boost après le départ
      m.setJoystick(j);
      m.runFrame();
      if (view) view->afterFrame(m);
      if (!wav.empty()) pcm.insert(pcm.end(), m.frameAudio().begin(), m.frameAudio().end());
    }
    if (!wav.empty()) {   // WAV 44,1 kHz mono 16 bits
      FILE *o = std::fopen(wav.c_str(), "wb");
      auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, o); };
      auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, o); };
      uint32_t bytes = uint32_t(pcm.size() * 2);
      std::fwrite("RIFF", 1, 4, o); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, o);
      u32(16); u16(1); u16(1); u32(44100); u32(88200); u16(2); u16(16);
      std::fwrite("data", 1, 4, o); u32(bytes); std::fwrite(pcm.data(), 2, pcm.size(), o);
      std::fclose(o);
    }
    auto t2 = std::chrono::steady_clock::now();
    double ds = std::chrono::duration<double>(t1 - t0).count(), rs = std::chrono::duration<double>(t2 - t1).count();
    std::printf("démarrage %.2f s ; %d trames (%.1f s de jeu) en %.3f s = %.0f× le temps réel\n", ds, frames, frames / 50.0, rs,
                frames / 50.0 / rs);
    std::printf("pièce %d  position x=%.2f y=%.2f z=%.2f  lacet=%.1f°\n", m.b(0x10906), m.l(0x10ac2) / 65536.0,
                m.l(0x10ac6) / 65536.0, m.l(0x10aca) / 65536.0, m.w(0x10ad0) * 360.0 / 65536);
    static uint32_t px[320 * 200];
    m.screenARGB(px);
    if (!shot.empty()) savePPM(shot, px, 320, 200);
    if (view && !cmp.empty()) {
      // à gauche l'original, à droite la scène HD à la même projection, en bas les deux superposés
      std::vector<uint32_t> sc(320 * 200), out(640 * 400);
      scr::Pose p = scr::HdView::poseFromState(m.displayedCarState());   // état rendu dans l'image affichée
      view->renderScene(p, m.b(0x1112d) & 7, sc.data(), 320, 200, hp.focal, hp.cx, hp.cy, m);
      for (int y = 0; y < 200; y++)
        for (int x = 0; x < 320; x++) {
          out[y * 640 + x] = px[y * 320 + x];
          out[y * 640 + 320 + x] = sc[y * 320 + x];
          uint32_t a = px[y * 320 + x], b = sc[y * 320 + x];
          out[(y + 200) * 640 + x] = 0xff000000u | (((a >> 1) & 0x7f7f7f) + ((b >> 1) & 0x7f7f7f));
        }
      static uint32_t ov[320 * 200];
      m.overlayARGB(ov);
      for (int y = 0; y < 200; y++)
        for (int x = 0; x < 320; x++) out[(y + 200) * 640 + 320 + x] = ov[y * 320 + x] ? ov[y * 320 + x] : 0xffff00ff;
      savePPM(cmp, out.data(), 640, 400);
    }
    if (view && !hd.empty()) {
      std::vector<uint32_t> big(1920 * 1080);
      view->render(m, frames, big.data(), 1920, 1080);
      savePPM(hd, big.data(), 1920, 1080);
    }
    if (view && benchHd) {
      std::vector<uint32_t> big(1920 * 1080);
      auto b0 = std::chrono::steady_clock::now();
      for (int k = 0; k < benchHd; k++) view->render(m, frames + k * 0.01, big.data(), 1920, 1080);
      double bs = std::chrono::duration<double>(std::chrono::steady_clock::now() - b0).count();
      std::printf("rendu HD 1920×1080 : %.2f ms/image (%.0f images/s possibles)\n", bs * 1000 / benchHd, benchHd / bs);
    }
  } catch (std::exception &e) {
    std::fprintf(stderr, "Erreur : %s\n", e.what());
    return 1;
  }
  return 0;
}
