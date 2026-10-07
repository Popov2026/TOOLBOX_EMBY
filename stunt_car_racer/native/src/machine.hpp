// machine.hpp — Stunt Car Racer d'origine exécuté nativement : CPU 68000 (Musashi),
// TOS minimal simulé, registres matériels utiles, VBL à 50 Hz.  Port C++ de
// replica/js/engine.js (même comportement, temporisation en cycles réels du 68000).
#pragma once
#include <cstdint>
#include <string>

#include "scrdata.hpp"

namespace scr {

// réglages de la physique d'origine (multiplicateurs ; 1 = jeu d'origine)
struct Tuning {
  double gravity = 1, thrust = 1, brake = 1, timeStep = 1, damping = 1, boostUse = 1;
  int shockTolerance = 0;
};

struct Joystick { bool up = false, down = false, left = false, right = false, fire = false; };

class Machine {
 public:
  explicit Machine(const Bytes &file);
  ~Machine();

  // démarrage complet (écran titre) ou entraînement direct sur un circuit (0..7)
  void coldStart();
  void startPractice(int track);

  void runFrame();                          // une trame vidéo (1/50 s)
  void setJoystick(const Joystick &j);
  void setKey(int scancode, bool down);
  void screenARGB(uint32_t *out) const;     // 320×200
  void setTuning(const Tuning &t);

  // accès mémoire (variables du jeu)
  uint8_t b(uint32_t a) const { return ram[a]; }
  uint16_t w(uint32_t a) const { return uint16_t((ram[a] << 8) | ram[a + 1]); }
  uint32_t l(uint32_t a) const { return (uint32_t(w(a)) << 16) | w(a + 2); }

  // --- interface interne avec les rappels de Musashi
  static Machine *current;
  uint32_t read8(uint32_t a);
  void write8(uint32_t a, uint32_t v);
  void hook(uint32_t pc);
  int intAck(int level);

  uint8_t *ram;
  static constexpr uint32_t RAMSIZE = 0x100000;

 private:
  void setupLowMem();
  void boot();
  void passChecksum();
  void runUntil(uint32_t stopAt, uint64_t maxCycles);
  void vblTick();
  void applyTuning();
  void setSR(uint32_t sr);
  void hleGemdos(uint32_t sr, uint32_t pc, uint32_t args);
  void hleXbios(uint32_t sr, uint32_t pc, uint32_t args);
  void hleReturn(uint32_t sr, uint32_t pc, uint32_t d0);

  Bytes disk_;                  // image .st (pour Floprd), vide si GAME.PUT
  uint16_t palette_[16] = {};
  uint32_t vbase_ = 0xf8000;
  uint8_t ymSel_ = 0, ym_[16] = {}, mfp_[64] = {};
  uint32_t stopAt_ = 0xffffffff;
  bool reached_ = false, skipWaits_ = false, patchable_ = false;
  uint64_t cyclesInFrame_ = 0;
  uint32_t vblCount_ = 0;
  bool autoUser_ = false;
  Tuning tuning_;
  bool haveOrig_ = false;
  uint8_t origCar_[22] = {};
  uint16_t origG_ = 0, origDamp_ = 0;
  uint8_t origDt_ = 0;
};

// scancodes ST utiles
namespace st {
enum : int { Esc = 0x01, Return = 0x1c, Space = 0x39, Up = 0x48, Left = 0x4b, Right = 0x4d, Down = 0x50 };
}

}  // namespace scr
