// machine.hpp — Stunt Car Racer d'origine exécuté nativement : CPU 68000 (Musashi),
// TOS minimal simulé, registres matériels utiles, VBL à 50 Hz.  Port C++ de
// replica/js/engine.js (même comportement, temporisation en cycles réels du 68000).
#pragma once
#include <cstdint>
#include <bitset>
#include <string>
#include <vector>

#include "scrdata.hpp"
#include "ym2149.hpp"

namespace scr {

// réglages de la physique d'origine (multiplicateurs ; 1 = jeu d'origine)
struct Tuning {
  double gravity = 1, thrust = 1, brake = 1, timeStep = 1, damping = 1, boostUse = 1;
  int shockTolerance = 0;
};

// sprite du cockpit dessiné par le jeu (blits $56762 / $5687E)
struct SpriteDraw {
  int id = 0, x = 0, y = 0, w = 0, h = 0;   // position et taille en pixels de l'écran d'origine
  uint32_t buffer = 0;                      // écran dans lequel il a été dessiné
  std::vector<uint8_t> under;               // ce qu'il recouvre : index de couleur, 255 = scène 3D
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
  // son : échantillons de la dernière trame (mono, 16 bits) à la fréquence choisie (0 = sans son)
  void setAudioRate(int hz) { audioRate_ = hz; }
  const std::vector<int16_t> &frameAudio() const { return audio_; }
  void setVolume(double v) { volume_ = v; }
  void setJoystick(const Joystick &j);
  void setKey(int scancode, bool down);
  void screenARGB(uint32_t *out) const;     // 320×200
  void screenIndex(uint8_t *out) const;     // 320×200, index de palette 0..15
  uint32_t paletteARGB(int i) const;
  void setTuning(const Tuning &t);

  const Bytes &image() const { return image_; }   // TEXT+DATA du jeu relogés à $10100 (non modifiés)
  uint32_t ticks() const { return ticks_; }       // nombre d'appels de la physique ($4EEB0)
  // adversaire : dessiné par le jeu depuis le début de la course en cours (absent en entraînement)
  bool opponentInRace() const { return oppDrawnInRace_; }
  // en HD, la voiture adverse est redessinée en 3D : on retire ses pixels d'origine du cockpit
  void hideOpponentPixels(bool on) { hideOpp_ = on; }
  uint32_t frames() const { return vblCount_; }
  // état de la voiture (18 octets à $10AC2) au début du rendu de l'image actuellement affichée
  const uint8_t *displayedCarState() const { return dispSnap_; }

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
  void (*debugHook)(Machine &, uint32_t pc) = nullptr;   // instrumentation (outils de test)
  // marquage des écritures : tags[a - lo] = PC de l'instruction qui a écrit l'octet a (lo <= a < hi)
  void tagWrites(uint32_t lo, uint32_t hi, uint32_t *tags) { tagLo = lo; tagHi = hi; tags_ = tags; }
  uint32_t tagLo = 0, tagHi = 0, *tags_ = nullptr;
  uint32_t vbase() const { return vbase_ & 0xfffffe; }

  // --- couches de l'image (mode HD) : on retient pour chaque octet de la mémoire écran quelle
  // routine l'a écrit (décor 3D, sprites du cockpit, autre) et la dernière valeur écrite par le décor.
  void enableLayers(bool on);
  bool layersEnabled() const { return layers_ != nullptr; }
  // cockpit de l'écran affiché : ARGB 320×200, alpha 0 là où l'on voit la scène 3D
  // visible : sprites suivis (watchSprite) encore visibles dans l'image ; leurs pixels sont
  // alors retirés du cockpit (remplacés par ce qu'ils recouvraient)
  void overlayARGB(uint32_t *out, std::vector<SpriteDraw> *visible = nullptr) const;
  const std::vector<SpriteDraw> &spriteDraws() const { return spriteDraws_; }   // (tests)
  void watchSprite(int id, bool on = true) { spriteWatch_[id & 255] = on; }
  // pixels d'un sprite : index de couleur, -1 = transparent
  bool spritePixels(int id, std::vector<int> &px, int &w, int &h) const;
  enum : uint8_t { W_OTHER = 0, W_SCENE = 1, W_SPRITE = 2, W_OPPONENT = 3 };
  bool inOpponent_ = false;       // dans la routine de dessin de la voiture adverse ($546DA)
  uint32_t oppReturn_ = 0;
  uint8_t *layers_ = nullptr;     // [RAMSIZE] classe ; [RAMSIZE..2×) valeur du décor ; [2×..3×) écrit par le décor
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

  Bytes image_;
  bool oppDrawnInRace_ = false, hideOpp_ = false;
  YM2149 ym2149_;
  struct YmWrite { uint32_t cycle; uint8_t reg, val; };
  std::vector<YmWrite> ymWrites_;
  uint64_t frameCycleBase_ = 0;
  int audioRate_ = 0;
  double audioFrac_ = 0, volume_ = 0.8;
  std::vector<int16_t> audio_;
  std::vector<float> mix_;
  void renderAudio();
  std::bitset<256> spriteWatch_;
  mutable std::vector<SpriteDraw> spriteDraws_;
  void recordSprite(uint32_t pc);
  uint8_t renderSnap_[18] = {}, dispSnap_[18] = {};
  uint32_t ticks_ = 0;
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
