// machine.cpp — voir machine.hpp.  Adresses : jeu chargé à $10100 (docs/RETRO_INGENIERIE.md).
#include "machine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include "m68k.h"
}

namespace scr {

namespace {
constexpr uint32_t BASE = 0x10100;
constexpr uint32_t A_BOOT = 0x103dc, A_CHECKSUM_DONE = 0x1041e, A_PREMENU = 0x4a5c6;
constexpr uint32_t A_OVERVIEW = 0x4a80a, A_OVERVIEW_WAIT = 0x4af5c, A_RACE = 0x4a924, A_LOOPTOP = 0x4aa74;
constexpr uint32_t SCREEN_LO = 0x40000;
constexpr uint32_t A_SPRITE = 0x56762, A_SPRITE_XY = 0x5687e, SPRITE_PTRS = 0x73298, SPRITE_DESC = 0x5692c + 4;
constexpr uint32_t A_DRAW_OPPONENT = 0x546da, A_RENDER = 0x51bcc, A_PHYSICS = 0x4eeb0, A_WAITVBL = 0x4b0a6, A_VBLCOUNTER = 0x4ec20, A_TRACKSEL = 0x1112d;

// routines d'écriture à l'écran du jeu : remplissages de polygones du décor ($53166, $533EC-$53456,
// $5461A-$546BE, $553D8-$55442 ; $53212-$5327C dessinent le tableau de bord)
// et blits de sprites masqués du cockpit ($567B0-$56880)
static inline void markWrite(Machine *m, uint32_t a, uint32_t v, uint32_t pc, bool changed = true) {
  bool scene = (pc >= 0x53100 && pc < 0x531a0) || (pc >= 0x533a0 && pc < 0x53460) || (pc >= 0x54600 && pc < 0x546c0) ||
               (pc >= 0x553a0 && pc < 0x55450);
  // blit de sprite qui réécrit la même valeur sur un pixel fixe du cockpit : il reste fixe (opaque)
  if (!changed && pc >= 0x567b0 && pc < 0x56880 && m->layers_[a] == Machine::W_OTHER) return;
  uint8_t cls = (pc >= 0x567b0 && pc < 0x56880) ? Machine::W_SPRITE
                : m->inOpponent_                    ? Machine::W_OPPONENT
                : scene && !m->inCrack_             ? Machine::W_SCENE
                                                    : Machine::W_OTHER;
  m->layers_[a] = cls;
  if (cls == Machine::W_SCENE) { m->layers_[Machine::RAMSIZE + a] = uint8_t(v); m->layers_[2 * Machine::RAMSIZE + a] = 1; }
}

constexpr uint32_t A_JOYSTICK = 0x106a6, A_KEYS = 0x6f02c;
constexpr uint32_t HLE = 0xffe00;                 // « ROM » : RTE en fin de RAM
constexpr uint32_t HLE_GEMDOS = HLE + 0x10, HLE_XBIOS = HLE + 0x20, HLE_LINEA = HLE + 0x30, SENTINEL = HLE + 0x40;
constexpr uint32_t PARK = HLE + 0x70;               // bra.s * : instruction neutre pour s'arrêter
constexpr uint32_t SCREEN = 0xf8000;
constexpr uint32_t CYCLES_PER_FRAME = 160256;     // 8 MHz, 50 Hz PAL
const uint32_t DT_SITES[] = {0x48a08, 0x4efaa, 0x4efc6, 0x4efe2, 0x4f014, 0x4f02c, 0x4f044, 0x4f136, 0x4f14e, 0x4f166,
                             0x4f180, 0x4f198, 0x4f1b0, 0x507fe, 0x509ea, 0x50ed4, 0x50eec, 0x50f06, 0x50f1e, 0x50f38, 0x50f50};
constexpr uint32_t CAR_TABLE = 0x1455a, CAR_BLOCK = 0x108e2;
}  // namespace

Machine *Machine::current = nullptr;

void Machine::enableLayers(bool on) {
  if (on && !layers_) layers_ = new uint8_t[3 * RAMSIZE]();   // classe, valeur écrite par la scène, octet écrit par la scène
  if (!on && layers_) { delete[] layers_; layers_ = nullptr; }
}

bool Machine::spritePixels(int id, std::vector<int> &px, int &wOut, int &h) const {
  uint32_t ptr = l(SPRITE_PTRS + id * 4) & 0xffffff, d = SPRITE_DESC + id * 16;
  int gw = this->w(d) + 1;
  h = this->w(d + 2) + 1;
  if (ptr < 0x10000 || ptr + gw * h * 10 >= RAMSIZE || gw > 20 || h > 200) return false;
  wOut = gw * 16;
  px.assign(size_t(wOut) * h, -1);
  uint32_t a = ptr;
  for (int r = 0; r < h; r++)
    for (int g = 0; g < gw; g++, a += 10) {
      uint16_t mask = this->w(a), p0 = this->w(a + 2), p1 = this->w(a + 4), p2 = this->w(a + 6), p3 = this->w(a + 8);
      for (int b = 15; b >= 0; b--)
        if (!((mask >> b) & 1))
          px[size_t(r) * wOut + g * 16 + 15 - b] =
              ((p0 >> b) & 1) | (((p1 >> b) & 1) << 1) | (((p2 >> b) & 1) << 2) | (((p3 >> b) & 1) << 3);
    }
  return true;
}

// entrée d'une routine de sprite : on note la position et on mémorise ce qui va être recouvert
void Machine::recordSprite(uint32_t pc) {
  int id = m68k_get_reg(nullptr, M68K_REG_D0) & 0xff;
  uint32_t d = SPRITE_DESC + id * 16;
  SpriteDraw sd;
  sd.id = id;
  sd.w = (w(d) + 1) * 16;
  sd.h = w(d + 2) + 1;
  if (pc == A_SPRITE) { sd.x = w(d + 4) * 16; sd.y = w(d + 6); }
  else { sd.x = int16_t(m68k_get_reg(nullptr, M68K_REG_D4)); sd.y = int16_t(m68k_get_reg(nullptr, M68K_REG_D5)); }
  sd.buffer = l(0x56c74) & 0xfffffe;
  if (sd.buffer + 32000 > RAMSIZE) return;
  if (spriteWatch_[id]) sd.under.assign(size_t(sd.w) * sd.h, 255);   // ce qu'il recouvre : seulement s'il sera remplacé
  for (int r = 0; r < sd.h && !sd.under.empty(); r++) {
    int y = sd.y + r;
    if (y < 0 || y >= 200) continue;
    for (int c = 0; c < sd.w; c++) {
      int x = sd.x + c;
      if (x < 0 || x >= 320) continue;
      uint32_t a = sd.buffer + y * 160 + (x >> 4) * 8;
      int bit = 15 - (x & 15), o = (x & 15) < 8 ? 0 : 1;
      if (layers_ && layers_[a + o] == W_SCENE) continue;   // la scène 3D : transparent
      uint16_t p0 = w(a), p1 = w(a + 2), p2 = w(a + 4), p3 = w(a + 6);
      sd.under[size_t(r) * sd.w + c] =
          uint8_t(((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3));
    }
  }
  // remplace un dessin précédent au même endroit dans le même écran
  for (auto &e : spriteDraws_)
    if (e.buffer == sd.buffer && e.x == sd.x && e.y == sd.y && e.w == sd.w && e.h == sd.h) { e = std::move(sd); return; }
  spriteDraws_.push_back(std::move(sd));
  if (spriteDraws_.size() > 96) {
    // ménage : on retire les sprites qui ne sont plus visibles dans leur écran (chaîne qui a bougé...)
    auto pix = [&](uint32_t buf, int x, int y) {
      uint32_t a = buf + y * 160 + (x >> 4) * 8;
      int bit = 15 - (x & 15);
      return ((w(a) >> bit) & 1) | (((w(a + 2) >> bit) & 1) << 1) | (((w(a + 4) >> bit) & 1) << 2) | (((w(a + 6) >> bit) & 1) << 3);
    };
    std::vector<int> px;
    for (size_t k = 0; k + 1 < spriteDraws_.size();) {   // le sprite qu'on vient d'ajouter n'est pas encore dessiné
      const SpriteDraw &e = spriteDraws_[k];
      int sw, sh, tot = 0, ok = 0;
      if (spritePixels(e.id, px, sw, sh))
        for (int r = 0; r < sh; r++)
          for (int c = 0; c < sw; c++) {
            int x = e.x + c, y = e.y + r, v = px[size_t(r) * sw + c];
            if (v < 0 || x < 0 || x >= 320 || y < 0 || y >= 200) continue;
            tot++; ok += pix(e.buffer, x, y) == v;
          }
      if (!tot || ok * 4 < tot) spriteDraws_.erase(spriteDraws_.begin() + long(k));
      else k++;
    }
    if (spriteDraws_.size() > 160) spriteDraws_.erase(spriteDraws_.begin());
  }
}

void Machine::overlayARGB(uint32_t *out, std::vector<SpriteDraw> *visible) const {
  static uint8_t idx[320 * 200], sidx[320 * 200], cls[320 * 200], sval[320 * 200];
  screenIndex(idx);
  uint32_t base = vbase();
  for (int y = 0; y < 200; y++)
    for (int g = 0; g < 20; g++) {
      uint32_t a = base + y * 160 + g * 8;
      for (int px = 0; px < 16; px++) {
        int bit = 15 - px, o = px < 8 ? 0 : 1;
        int i = y * 320 + g * 16 + px;
        cls[i] = layers_ ? layers_[a + o] : W_OTHER;
        sval[i] = layers_ ? layers_[2 * RAMSIZE + a + o] : 0;   // la scène a-t-elle déjà dessiné ici ?
        if (layers_) {
          const uint8_t *sh = layers_ + RAMSIZE + a;
          uint16_t p0 = (sh[0] << 8) | sh[1], p1 = (sh[2] << 8) | sh[3], p2 = (sh[4] << 8) | sh[5], p3 = (sh[6] << 8) | sh[7];
          sidx[i] = uint8_t(((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3));
        }
      }
    }
  // vitres latérales : bleu statique relié aux bords de l'écran (remplissage par diffusion)
  static uint8_t clear[320 * 200];
  std::memset(clear, 0, sizeof clear);
  std::vector<int> st;
  // limité à l'extérieur de l'arceau (bandeau du haut, côtés) : ne jamais déborder sur le tableau de bord
  auto outside = [](int i) {
    int x = i % 320, y = i / 320;
    return y < 16 || (y < 150 ? (x < 32 || x >= 288) : (x < 16 || x >= 304));
  };
  // le noir n'est « vitre » que dans les coins hors de l'arceau : les encoches noires du haut du cadre
  // (entre les rivets) restent opaques
  auto seed = [&](int i) {
    int x = i % 320;
    bool blackOk = idx[i] == 0 && (x < 36 || x >= 284);
    if (!clear[i] && outside(i) && cls[i] != W_SCENE && (idx[i] == 7 || blackOk)) { clear[i] = 1; st.push_back(i); }
  };
  for (int x = 0; x < 320; x++) seed(x);
  for (int y = 0; y < 200; y++) { seed(y * 320); seed(y * 320 + 319); }
  while (!st.empty()) {
    int i = st.back(); st.pop_back();
    int x = i % 320, y = i / 320;
    if (x > 0) seed(i - 1);
    if (x < 319) seed(i + 1);
    if (y > 0) seed(i - 320);
    if (y < 199) seed(i + 320);
  }
  // sprites du cockpit encore visibles dans l'image affichée (au moins le quart de leurs pixels) :
  // seuls leurs pixels opaques sont gardés. Le reste de ce qu'ont écrit les routines de sprites
  // (blocs de 16 pixels réécrits en entier, restes d'un sprite qui a bougé) laisse voir la scène.
  static uint8_t sprMask[320 * 200];
  std::memset(sprMask, 0, sizeof sprMask);
  uint32_t vb = vbase();
  std::vector<int> spx;
  std::vector<std::pair<size_t, std::vector<int>>> live;   // (indice, pixels) des sprites visibles
  for (size_t k = 0; k < spriteDraws_.size();) {
    SpriteDraw &e = spriteDraws_[k];
    int sw, sh;
    if (e.buffer != vb) { k++; continue; }
    if (!spritePixels(e.id, spx, sw, sh)) { spriteDraws_.erase(spriteDraws_.begin() + long(k)); continue; }
    int tot = 0, ok = 0;
    for (int r = 0; r < sh; r++)
      for (int c = 0; c < sw; c++) {
        int x = e.x + c, y = e.y + r, v = spx[size_t(r) * sw + c];
        if (v < 0 || x < 0 || x >= 320 || y < 0 || y >= 200) continue;
        tot++; ok += idx[y * 320 + x] == v;
      }
    if (!tot || ok * 4 < tot) { spriteDraws_.erase(spriteDraws_.begin() + long(k)); continue; }
    for (int r = 0; r < sh; r++)
      for (int c = 0; c < sw; c++) {
        int x = e.x + c, y = e.y + r, v = spx[size_t(r) * sw + c];
        if (v >= 0 && x >= 0 && x < 320 && y >= 0 && y < 200 && idx[y * 320 + x] == v) sprMask[y * 320 + x] = 1;
      }
    live.push_back({k, spx});
    k++;
  }
  for (int i = 0; i < 64000; i++) {
    // voiture adverse : seules les couleurs de la carrosserie sont gardées (pas le ciel, le sol, les collines, la route)
    bool oppBg = cls[i] == W_OPPONENT && (hideOpp_ || idx[i] == 7 || idx[i] == 13 || idx[i] == 5 || idx[i] == 1 || idx[i] == 2 || idx[i] == 3);
    bool transparent = clear[i] || cls[i] == W_SCENE || oppBg || (cls[i] == W_SPRITE && !sprMask[i] && sval[i]);
    out[i] = transparent ? 0 : paletteARGB(idx[i]);
  }
  // sprites remplacés en HD : on efface l'original du cockpit (on remet ce qu'il recouvrait)
  if (visible) visible->clear();
  for (auto &[k, px] : live) {
    const SpriteDraw &e = spriteDraws_[k];
    if (e.under.empty()) continue;
    int sw = e.w, sh = e.h;
    for (int r = 0; r < sh; r++)
      for (int c = 0; c < sw; c++) {
        int x = e.x + c, y = e.y + r, v = px[size_t(r) * sw + c];
        if (v < 0 || x < 0 || x >= 320 || y < 0 || y >= 200 || idx[y * 320 + x] != v) continue;
        uint8_t u = e.under[size_t(r) * e.w + c];
        out[y * 320 + x] = u == 255 ? 0 : paletteARGB(u);
      }
    if (visible) visible->push_back(e);
  }
}


Machine::Machine(const Bytes &file) {
  ram = new uint8_t[RAMSIZE]();
  if (!(file.size() > 2 && file[0] == 0x60 && file[1] == 0x1a)) disk_ = file;
  Bytes prg = innerProgram(file);
  uint32_t bss = 0;
  Bytes img = relocate(prg, BASE, bss);
  image_ = img;
  if (BASE + img.size() + bss > HLE) throw std::runtime_error("programme trop grand");
  std::memcpy(ram + BASE, img.data(), img.size());
  // contrôle : noms des circuits à l'adresse attendue
  if (std::memcmp(ram + 0x13498, "LITTLE RAMP", 11) != 0) throw std::runtime_error("version du jeu non reconnue");
  current = this;
  m68k_init();
  m68k_set_cpu_type(M68K_CPU_TYPE_68000);
  setupLowMem();
  m68k_pulse_reset();
}

Machine::~Machine() {
  enableLayers(false);
  if (current == this) current = nullptr;
  delete[] ram;
}

void Machine::setupLowMem() {
  for (int i = 0; i < 0x80; i += 2) { ram[HLE + i] = 0x4e; ram[HLE + i + 1] = 0x73; }  // RTE
  ram[PARK] = 0x60; ram[PARK + 1] = 0xfe;
  auto w32 = [&](uint32_t a, uint32_t v) { ram[a] = v >> 24; ram[a + 1] = v >> 16; ram[a + 2] = v >> 8; ram[a + 3] = v; };
  for (int v = 2; v < 256; v++) w32(v * 4, HLE);
  w32(0, 0x7000); w32(4, A_BOOT);
  w32(33 * 4, HLE_GEMDOS); w32(46 * 4, HLE_XBIOS); w32(10 * 4, HLE_LINEA);
  w32(0x42e, RAMSIZE); w32(0x44e, SCREEN);
  ram[0x484] = 7;
}

// ------------------------------------------------------------------- mémoire / matériel
uint32_t Machine::read8(uint32_t a) {
  a &= 0xffffff;
  if (a < RAMSIZE) return ram[a];
  if (a >= 0xff8240 && a < 0xff8260) { int i = (a - 0xff8240) >> 1; return (a & 1) ? palette_[i] & 0xff : palette_[i] >> 8; }
  switch (a) {
    case 0xff8201: return (vbase_ >> 16) & 0xff;
    case 0xff8203: return (vbase_ >> 8) & 0xff;
    case 0xff8800: return ym_[ymSel_ & 15];
    case 0xfffc00: return 2;     // ACIA : prêt à émettre
    case 0xfffc02: return 0;
  }
  if (a >= 0xfffa00 && a < 0xfffa40) return mfp_[a & 63];
  return 0;
}

void Machine::write8(uint32_t a, uint32_t v) {
  a &= 0xffffff; v &= 0xff;
  if (a < RAMSIZE) {
    bool changed = ram[a] != uint8_t(v);
    ram[a] = uint8_t(v);
    if (a >= tagLo && a < tagHi) tags_[a - tagLo] = m68k_get_reg(nullptr, M68K_REG_PPC);
    if (layers_ && a >= SCREEN_LO) markWrite(this, a, v, m68k_get_reg(nullptr, M68K_REG_PPC), changed);
    return;
  }
  if (a >= 0xff8240 && a < 0xff8260) {
    int i = (a - 0xff8240) >> 1;
    palette_[i] = (a & 1) ? uint16_t((palette_[i] & 0xff00) | v) : uint16_t((palette_[i] & 0xff) | (v << 8));
    return;
  }
  switch (a) {
    case 0xff8201: vbase_ = (vbase_ & 0xffff) | (v << 16); std::memcpy(dispSnap_, renderSnap_, 18); return;
    case 0xff8203: vbase_ = (vbase_ & 0xff00ff) | (v << 8); std::memcpy(dispSnap_, renderSnap_, 18); return;
    case 0xff8800: ymSel_ = uint8_t(v); return;
    case 0xff8802:
      ym_[ymSel_ & 15] = uint8_t(v);
      ymWrites_.push_back({uint32_t(frameCycleBase_ + m68k_cycles_run()), uint8_t(ymSel_ & 15), uint8_t(v)});
      if (ymWrites_.size() > 100000) ymWrites_.erase(ymWrites_.begin(), ymWrites_.begin() + 50000);
      return;
  }
  if (a >= 0xfffa00 && a < 0xfffa40) mfp_[a & 63] = uint8_t(v);
}

// SR avec échange des piles (Musashi ne le fait pas via set_reg)
void Machine::setSR(uint32_t nsr) {
  uint32_t osr = m68k_get_reg(nullptr, M68K_REG_SR);
  if ((osr & 0x2000) && !(nsr & 0x2000)) {
    uint32_t ssp = m68k_get_reg(nullptr, M68K_REG_A7), usp = m68k_get_reg(nullptr, M68K_REG_USP);
    m68k_set_reg(M68K_REG_SR, nsr); m68k_set_reg(M68K_REG_A7, usp); m68k_set_reg(M68K_REG_ISP, ssp);
  } else if (!(osr & 0x2000) && (nsr & 0x2000)) {
    uint32_t usp = m68k_get_reg(nullptr, M68K_REG_A7), ssp = m68k_get_reg(nullptr, M68K_REG_ISP);
    m68k_set_reg(M68K_REG_SR, nsr); m68k_set_reg(M68K_REG_A7, ssp); m68k_set_reg(M68K_REG_USP, usp);
  } else {
    m68k_set_reg(M68K_REG_SR, nsr);
  }
}

void Machine::hleReturn(uint32_t sr, uint32_t pc, uint32_t d0) {
  m68k_set_reg(M68K_REG_D0, d0);
  setSR(sr);
  m68k_set_reg(M68K_REG_PC, pc);
}

// ------------------------------------------------------------------- TOS minimal
void Machine::hleGemdos(uint32_t sr, uint32_t pc, uint32_t args) {
  uint32_t fn = (read8(args) << 8) | read8(args + 1);
  if (fn == 0x20) {  // Super
    uint32_t a = (read8(args + 2) << 24) | (read8(args + 3) << 16) | (read8(args + 4) << 8) | read8(args + 5);
    uint32_t cursp = m68k_get_reg(nullptr, M68K_REG_A7);
    if (!(sr & 0x2000)) {
      uint32_t usp = m68k_get_reg(nullptr, M68K_REG_USP);
      m68k_set_reg(M68K_REG_A7, a ? a : usp);
      m68k_set_reg(M68K_REG_D0, cursp);
      m68k_set_reg(M68K_REG_SR, sr | 0x2000);
      m68k_set_reg(M68K_REG_PC, pc);
      return;
    }
    m68k_set_reg(M68K_REG_SR, sr & ~0x2000u);
    m68k_set_reg(M68K_REG_A7, cursp); m68k_set_reg(M68K_REG_ISP, a);
    m68k_set_reg(M68K_REG_D0, 0); m68k_set_reg(M68K_REG_PC, pc);
    return;
  }
  hleReturn(sr, pc, 0);
}

void Machine::hleXbios(uint32_t sr, uint32_t pc, uint32_t args) {
  auto rw = [&](uint32_t a) { return (read8(a) << 8) | read8(a + 1); };
  auto rl = [&](uint32_t a) { return (uint32_t(rw(a)) << 16) | rw(a + 2); };
  uint32_t fn = rw(args), d0 = 0;
  switch (fn) {
    case 2: case 3: d0 = SCREEN; break;
    case 6: { uint32_t p = rl(args + 2); for (int i = 0; i < 16; i++) palette_[i] = uint16_t(rw(p + 2 * i)); break; }
    case 8: {  // Floprd : secteur 5 piste 0 (clé inutilisée du jeu)
      uint32_t buf = rl(args + 2), sec = rw(args + 12), trk = rw(args + 14), side = rw(args + 16), cnt = rw(args + 18);
      size_t off = (size_t(trk * 2 + side) * 9 + (sec - 1)) * 512;
      for (uint32_t i = 0; i < cnt * 512; i++) write8(buf + i, off + i < disk_.size() ? disk_[off + i] : 0);
      break;
    }
    case 16: {  // Keytbl
      static const char *sc = "\0\x1b" "1234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0\0\0 ";
      uint32_t t = 0x1c00;
      auto w32 = [&](uint32_t a, uint32_t v) { for (int k = 0; k < 4; k++) write8(a + k, v >> (24 - 8 * k)); };
      w32(t, t + 0x40); w32(t + 4, t + 0xc0); w32(t + 8, t + 0x140);
      for (int i = 0; i < 128; i++) {
        int ch = i < 58 ? (unsigned char)sc[i] : 0;
        write8(t + 0x40 + i, ch); write8(t + 0xc0 + i, toupper(ch)); write8(t + 0x140 + i, toupper(ch));
      }
      d0 = t;
      break;
    }
    default: break;  // Cursconf, Jenabint… : sans effet
  }
  hleReturn(sr, pc, d0);
}

// appelé par Musashi avant chaque instruction
void Machine::hook(uint32_t pc) {
  if (pc == stopAt_) {     // Musashi exécute l'instruction après le crochet : on la remplace par bra.s *
    reached_ = true; m68k_set_reg(M68K_REG_PC, PARK); m68k_end_timeslice(); return;
  }
  if (debugHook) debugHook(*this, pc);
  if (layers_) {   // voiture adverse : de l'entrée de $546DA jusqu'au retour à l'appelant
    if (pc == A_DRAW_OPPONENT && !inOpponent_) {
      uint32_t sp = m68k_get_reg(nullptr, M68K_REG_A7);
      oppReturn_ = (uint32_t(ram[sp]) << 24 | ram[sp + 1] << 16 | ram[sp + 2] << 8 | ram[sp + 3]) & 0xffffff;
      inOpponent_ = true;
    } else if (inOpponent_ && pc == oppReturn_) inOpponent_ = false;
    // fissure des dégâts dans le haut du cadre : fait partie du cockpit (opaque), pas du décor
    if (pc == 0x52f00) inCrack_ = true;
    else if (pc == 0x52fa4 || pc == 0x52e5c) inCrack_ = false;
  }
  if ((pc == A_SPRITE || pc == A_SPRITE_XY) && (layers_ || spriteWatch_.any())) recordSprite(pc);
  if (pc == A_DRAW_OPPONENT) oppDrawnInRace_ = true;
  if (pc == A_RACE) oppDrawnInRace_ = false;
  if (pc == A_PHYSICS) ticks_++;
  if (pc == A_RENDER) { std::memcpy(renderSnap_, ram + 0x10ac2, 18); inOpponent_ = false; }
  if (skipWaits_ && pc == A_WAITVBL) ram[A_VBLCOUNTER] = 0;
  if (pc < HLE || pc >= HLE + 0x40) return;
  uint32_t sp = m68k_get_reg(nullptr, M68K_REG_A7);
  if (pc != HLE_GEMDOS && pc != HLE_XBIOS && pc != HLE_LINEA) return;   // simple RTE
  uint32_t sr = (ram[sp] << 8) | ram[sp + 1];
  uint32_t rpc = (uint32_t(ram[sp + 2]) << 24) | (ram[sp + 3] << 16) | (ram[sp + 4] << 8) | ram[sp + 5];
  m68k_set_reg(M68K_REG_A7, sp + 6);
  uint32_t args = (sr & 0x2000) ? sp + 6 : m68k_get_reg(nullptr, M68K_REG_USP);
  if (pc == HLE_GEMDOS) hleGemdos(sr, rpc, args);
  else if (pc == HLE_XBIOS) hleXbios(sr, rpc, args);
  else { setSR(sr); m68k_set_reg(M68K_REG_PC, rpc + 2); }   // Line-A : ignoré
}

int Machine::intAck(int level) {
  m68k_set_irq(0);
  return M68K_INT_ACK_AUTOVECTOR;
  (void)level;
}

// ------------------------------------------------------------------- exécution
void Machine::vblTick() {
  vblCount_++;
  if (autoUser_) {  // utilisateur simulé pour traverser les menus d'avant la course
    int ph = vblCount_ % 60;
    Joystick j; j.fire = ph >= 10 && ph < 15;
    setJoystick(j);
    setKey(0x1e, ph >= 30 && ph < 33);
    setKey(0x1c, ph >= 45 && ph < 48);
  }
  m68k_set_irq(4);
}

void Machine::runFrame() {
  // écritures restées d'avant (démarrage) : appliquées en début de trame
  for (auto &w : ymWrites_) ym2149_.write(w.reg, w.val);
  ymWrites_.clear();
  uint64_t done = 0;
  frameCycleBase_ = 0;
  while (done < CYCLES_PER_FRAME) { done += m68k_execute(int(CYCLES_PER_FRAME - done)); frameCycleBase_ = done; }
  frameCycleBase_ = 0;
  renderAudio();
  vblTick();
}

// son de la trame : les écritures dans la YM2149 sont appliquées à l'échantillon correspondant à leur cycle
void Machine::renderAudio() {
  if (!audioRate_) { ymWrites_.clear(); audio_.clear(); return; }
  audioFrac_ += double(audioRate_) / 50.0;   // PAL : 50 trames/s
  int n = int(audioFrac_);
  audioFrac_ -= n;
  mix_.assign(size_t(n), 0.f);
  int pos = 0;
  for (auto &w : ymWrites_) {
    int at = std::min(n, int(double(w.cycle) / CYCLES_PER_FRAME * n));
    if (at > pos) { ym2149_.render(mix_.data() + pos, at - pos, audioRate_); pos = at; }
    ym2149_.write(w.reg, w.val);
  }
  if (pos < n) ym2149_.render(mix_.data() + pos, n - pos, audioRate_);
  ymWrites_.clear();
  audio_.resize(size_t(n));
  for (int i = 0; i < n; i++) audio_[i] = int16_t(std::clamp(mix_[i] * volume_ * 32767.0, -32768.0, 32767.0));
}

void Machine::runUntil(uint32_t stopAt, uint64_t maxCycles) {
  stopAt_ = stopAt; reached_ = false;
  uint64_t total = 0;
  while (!reached_) {
    int n = m68k_execute(4096);
    total += n; cyclesInFrame_ += n;
    if (cyclesInFrame_ >= CYCLES_PER_FRAME) { cyclesInFrame_ = 0; vblTick(); }
    if (reached_) break;
    if (total > maxCycles) {
      stopAt_ = 0xffffffff;
      char msg[160];
      std::snprintf(msg, sizeof msg, "jeu bloqué pendant le démarrage automatique (attendu $%X, pc=$%X)", stopAt,
                    m68k_get_reg(nullptr, M68K_REG_PC));
      throw std::runtime_error(msg);
    }
  }
  stopAt_ = 0xffffffff;
  m68k_set_reg(M68K_REG_PC, stopAt);
}

void Machine::passChecksum() {
  runUntil(A_CHECKSUM_DONE, 50000000ull);
  patchable_ = true;
  applyTuning();
}

void Machine::coldStart() {
  m68k_set_reg(M68K_REG_PC, A_BOOT);
  m68k_set_reg(M68K_REG_A7, 0x7000);
  m68k_set_reg(M68K_REG_USP, 0x103da);
  setSR(0x0000);
  m68k_set_reg(M68K_REG_A7, 0x103da);
  passChecksum();
}

void Machine::boot() {
  coldStart();
  autoUser_ = true;
  runUntil(A_PREMENU, 4000000000ull);
  autoUser_ = false;
  setJoystick(Joystick());
  setKey(0x1e, false); setKey(0x1c, false);
}

void Machine::startPractice(int track) {
  boot();
  skipWaits_ = true;
  ram[A_TRACKSEL] = uint8_t(track & 7);
  // la vue d'ensemble attend « feu » : on répond immédiatement C=0
  const uint8_t patch[] = {0x44, 0xfc, 0x00, 0x00, 0x4e, 0x75};
  std::memcpy(ram + A_OVERVIEW_WAIT, patch, sizeof patch);
  auto push = [&](uint32_t v) {
    uint32_t sp = m68k_get_reg(nullptr, M68K_REG_A7) - 4;
    ram[sp] = v >> 24; ram[sp + 1] = v >> 16; ram[sp + 2] = v >> 8; ram[sp + 3] = v;
    m68k_set_reg(M68K_REG_A7, sp);
  };
  push(SENTINEL); m68k_set_reg(M68K_REG_PC, A_OVERVIEW);
  runUntil(SENTINEL, 4000000000ull);
  push(SENTINEL); m68k_set_reg(M68K_REG_PC, A_RACE);
  runUntil(A_LOOPTOP, 4000000000ull);
  skipWaits_ = false;
}

void Machine::setJoystick(const Joystick &j) {
  int b = (j.up ? 1 : 0) | (j.down ? 2 : 0) | (j.left ? 4 : 0) | (j.right ? 8 : 0) | (j.fire ? 0x10 : 0);
  ram[A_JOYSTICK] = uint8_t(~b);
}

void Machine::setKey(int sc, bool down) { ram[A_KEYS + (sc & 0x7f)] = down ? 0xb3 : 0; }

uint32_t Machine::paletteARGB(int i) const {
  uint16_t c = palette_[i & 15];
  uint32_t r = ((c >> 8) & 7) * 255 / 7, g = ((c >> 4) & 7) * 255 / 7, bl = (c & 7) * 255 / 7;
  return 0xff000000u | (r << 16) | (g << 8) | bl;
}

void Machine::screenIndex(uint8_t *out) const {
  uint32_t base = vbase_ & 0xfffffe;
  for (int y = 0; y < 200; y++)
    for (int x = 0; x < 20; x++) {
      uint32_t a = base + y * 160 + x * 8;
      if (a + 7 >= RAMSIZE) continue;
      uint16_t p0 = (ram[a] << 8) | ram[a + 1], p1 = (ram[a + 2] << 8) | ram[a + 3];
      uint16_t p2 = (ram[a + 4] << 8) | ram[a + 5], p3 = (ram[a + 6] << 8) | ram[a + 7];
      for (int bit = 15; bit >= 0; bit--)
        out[y * 320 + x * 16 + (15 - bit)] =
            uint8_t(((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3));
    }
}

void Machine::screenARGB(uint32_t *out) const {
  static uint8_t idx[320 * 200];
  screenIndex(idx);
  for (int i = 0; i < 320 * 200; i++) out[i] = paletteARGB(idx[i]);
}

// ------------------------------------------------------------------- réglages (docs §8)
void Machine::setTuning(const Tuning &t) { tuning_ = t; if (patchable_) applyTuning(); }

void Machine::applyTuning() {
  auto w16 = [&](uint32_t a, uint32_t v) { ram[a] = uint8_t(v >> 8); ram[a + 1] = uint8_t(v); };
  if (!haveOrig_) {
    std::memcpy(origCar_, ram + CAR_TABLE, 22);
    origG_ = w(0x4e88c); origDamp_ = w(0x4ee64); origDt_ = ram[DT_SITES[0] + 3];
    haveOrig_ = true;
  }
  auto clampi = [](double v, int lo, int hi) { return int(std::lround(v < lo ? lo : v > hi ? hi : v)); };
  int g = clampi(origG_ * tuning_.gravity, 1, 0x7fff);
  w16(0x4e884, uint32_t(-g) & 0xffff); w16(0x4e88c, uint32_t(g));
  int dt = clampi(origDt_ * tuning_.timeStep, 1, 255);
  for (uint32_t s : DT_SITES) ram[s + 3] = uint8_t(dt);
  w16(0x4ee64, uint32_t(clampi(origDamp_ * tuning_.damping, 0, 0x7fff)));
  int brake = clampi(240 * tuning_.brake, 0, 0x7fff);
  uint32_t bw = uint32_t(-brake) & 0xffff;
  ram[0x4af39] = uint8_t(bw); ram[0x4af3d] = uint8_t(bw >> 8);
  for (int blk = 0; blk < 2; blk++) {
    uint8_t b8[11];
    std::memcpy(b8, origCar_ + blk * 11, 11);
    int thrust = clampi((b8[2] | (b8[3] << 8)) * tuning_.thrust, 0, 0x7fff);
    b8[2] = uint8_t(thrust); b8[3] = uint8_t(thrust >> 8);
    b8[6] = uint8_t(clampi(b8[6] / (tuning_.boostUse < 0.05 ? 0.05 : tuning_.boostUse), 1, 255));
    b8[9] = uint8_t(clampi(b8[9] + tuning_.shockTolerance, 0, 255));
    std::memcpy(ram + CAR_TABLE + blk * 11, b8, 11);
    if ((ram[0x110ca] ? 1 : 0) == blk)
      for (int i : {2, 3, 6, 9}) ram[CAR_BLOCK + i] = b8[i];
  }
}

}  // namespace scr

// ------------------------------------------------------------------- rappels Musashi
using scr::Machine;
extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) { return Machine::current->read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) {
  a &= 0xffffff;
  if (a + 1 < Machine::RAMSIZE) return (Machine::current->ram[a] << 8) | Machine::current->ram[a + 1];
  return (Machine::current->read8(a) << 8) | Machine::current->read8(a + 1);
}
unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { Machine::current->write8(a, v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) {
  a &= 0xffffff;
  Machine *m = Machine::current;
  if (a + 1 < Machine::RAMSIZE) {
    bool ch0 = m->ram[a] != uint8_t(v >> 8), ch1 = m->ram[a + 1] != uint8_t(v);
    m->ram[a] = uint8_t(v >> 8); m->ram[a + 1] = uint8_t(v);
    if (a >= m->tagLo && a < m->tagHi) {
      uint32_t pc = m68k_get_reg(nullptr, M68K_REG_PPC);
      m->tags_[a - m->tagLo] = pc;
      if (a + 1 < m->tagHi) m->tags_[a + 1 - m->tagLo] = pc;
    }
    if (m->layers_ && a >= scr::SCREEN_LO) {
      uint32_t pc = m68k_get_reg(nullptr, M68K_REG_PPC);
      scr::markWrite(m, a, v >> 8, pc, ch0); scr::markWrite(m, a + 1, v & 0xff, pc, ch1);
    }
    return;
  }
  Machine::current->write8(a, v >> 8); Machine::current->write8(a + 1, v);
}
void m68k_write_memory_32(unsigned int a, unsigned int v) { m68k_write_memory_16(a, v >> 16); m68k_write_memory_16(a + 2, v & 0xffff); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
void scr_instr_hook(unsigned int pc) { Machine::current->hook(pc); }
int scr_int_ack(int level) { return Machine::current->intAck(level); }
}
