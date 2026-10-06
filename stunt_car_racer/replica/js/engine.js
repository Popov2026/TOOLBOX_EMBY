/*
 * engine.js — « moteur fidèle » : exécute la logique d'origine de Stunt Car Racer
 * (physique, IA, règles) dans le CPU 68000 JavaScript, à partir du programme lu sur la
 * disquette de l'utilisateur.  Le rendu d'origine est court-circuité ; la réplique
 * dessine la scène à partir de l'état de la mémoire du jeu.
 *
 * Adresses : jeu chargé à $10100 (voir docs/RETRO_INGENIERIE.md).
 */
var SCR = window.SCR || (window.SCR = {});

SCR.Engine = (function () {
  'use strict';

  var BASE = 0x10100;
  var A = {
    boot: 0x103dc,          // point d'entrée du programme
    preMenu: 0x4a5c6,       // juste avant les menus (jsr $49032)
    overview: 0x4a80a,      // vue d'ensemble + chargement du circuit ($1112D)
    overviewWait: 0x4af5c,  // attente « tourner la vue ou feu » (renvoie C=0 pour continuer)
    race: 0x4a924,          // préparation de la course, grue, puis boucle
    loopTop: 0x4aa74,       // haut de la boucle de course (une itération = un tick)
    getKey: 0x46d96,        // attend une touche, renvoie D0 = scancode<<8 | ascii, C=0
    waitVbl: 0x4b0a6,       // attente du compteur VBL + échange d'écrans
    vblCounter: 0x4ec20,
    render3d: 0x51bcc,      // rendu 3D de la vue cockpit
    trackSel: 0x1112d,
    joystick: 0x106a6,
    keys: 0x6f02c
  };

  var HLE = 0xffe00;        // petit « ROM » en fin de RAM : RTE pour les vecteurs non gérés
  var SCREEN = 0xf8000;

  function Engine(bytes, opts) {
    opts = opts || {};
    var prg = SCR.data.innerProgram(bytes);
    var rel = SCR.data.relocate(prg, BASE);
    this.disk = (bytes[0] === 0x60 && bytes[1] === 0x1a) ? null : bytes;
    var cpu = this.cpu = new SCR.CPU(0x100000);
    cpu.mem.set(rel.image, BASE);
    this.end = BASE + rel.image.length + rel.bss;
    this.vblEvery = opts.vblEvery || 20000;
    this.sinceVbl = 0;
    this.renderOriginal = opts.renderOriginal !== false;   // le rendu d'origine contient un peu de logique : exécuté par défaut
    this.io = { palette: new Uint16Array(16), vbase: SCREEN, ym: new Uint8Array(16), ymSel: 0, mfp: new Uint8Array(64) };
    this.setupLowMem();
    var self = this;
    cpu.io = function (a, sz, v) { return self.ioAccess(a, v); };
    cpu.trap = function (vec) { return self.hle(vec); };
    this.patches = [];
  }

  Engine.ADDR = A;

  Engine.prototype.setupLowMem = function () {
    var c = this.cpu, i;
    for (i = 0; i < 16; i++) { c.mem[HLE + 2 * i] = 0x4e; c.mem[HLE + 2 * i + 1] = 0x73; }   // RTE
    for (i = 2; i < 256; i++) c.w32(i * 4, HLE);
    c.w32(0x42e, 0x100000); c.w32(0x44e, SCREEN); c.w32(0x4ba, 0);
    c.mem[0x484] = 7;
  };

  /* ---------------------------------------------------------------- matériel minimal */
  Engine.prototype.ioAccess = function (a, v) {
    var io = this.io;
    if (a >= 0xff8240 && a < 0xff8260) {
      var i = (a - 0xff8240) >> 1;
      if (v === undefined) return (a & 1) ? io.palette[i] & 0xff : io.palette[i] >> 8;
      io.palette[i] = (a & 1) ? (io.palette[i] & 0xff00) | v : (io.palette[i] & 0xff) | (v << 8);
      return;
    }
    switch (a) {
      case 0xff8201: if (v === undefined) return (io.vbase >> 16) & 0xff; io.vbase = (io.vbase & 0xffff) | (v << 16); return;
      case 0xff8203: if (v === undefined) return (io.vbase >> 8) & 0xff; io.vbase = (io.vbase & 0xff00ff) | (v << 8); return;
      case 0xff8800: if (v === undefined) return io.ym[io.ymSel & 15]; io.ymSel = v; return;
      case 0xff8802: if (v !== undefined) io.ym[io.ymSel & 15] = v; return;
      case 0xfffc00: return v === undefined ? 2 : undefined;         // ACIA : prêt à émettre
      case 0xfffc02: return v === undefined ? 0 : undefined;
    }
    if (a >= 0xfffa00 && a < 0xfffa40) { if (v === undefined) return io.mfp[a & 63]; io.mfp[a & 63] = v; return; }
    return v === undefined ? 0 : undefined;
  };

  /* ---------------------------------------------------------------- TOS simulé */
  Engine.prototype.hle = function (vec) {
    var c = this.cpu;
    if (vec === 10) return true;                       // Line-A : ignoré (souris)
    if (vec !== 33 && vec !== 45 && vec !== 46) return false;
    var sp = c.a[7] >>> 0, fn = c.r16(sp), d0 = 0, i;
    if (vec === 33) {                                   // GEMDOS
      if (fn === 0x20) {                                // Super
        var arg = c.r32(sp + 2);
        if (!c.s) { d0 = c.ssp; c.s = 1; c.usp = c.a[7]; if (arg) c.a[7] = arg; }
        else if (arg) { c.s = 0; c.usp = c.a[7]; c.ssp = arg; }
      }
    } else if (vec === 46) {                            // XBIOS
      switch (fn) {
        case 2: case 3: d0 = SCREEN; break;
        case 4: d0 = 0; break;
        case 5: break;
        case 6: { var p = c.r32(sp + 2); for (i = 0; i < 16; i++) this.io.palette[i] = c.r16(p + 2 * i); break; }
        case 8: {                                       // Floprd
          var buf = c.r32(sp + 2), sec = c.r16(sp + 12), trk = c.r16(sp + 14), side = c.r16(sp + 16), cnt = c.r16(sp + 18);
          var off = ((trk * 2 + side) * 9 + (sec - 1)) * 512;
          for (i = 0; i < cnt * 512; i++) c.w8(buf + i, this.disk && off + i < this.disk.length ? this.disk[off + i] : 0);
          break;
        }
        case 16: {                                      // Keytbl
          var t = 0x1c00, sc = '\x00\x1b1234567890-=\x08\tqwertyuiop[]\r\x00asdfghjkl;\'`\x00\\zxcvbnm,./\x00\x00\x00 ';
          c.w32(t, t + 0x40); c.w32(t + 4, t + 0xc0); c.w32(t + 8, t + 0x140);
          for (i = 0; i < 128; i++) {
            var ch = i < sc.length ? sc.charCodeAt(i) : 0;
            c.w8(t + 0x40 + i, ch); c.w8(t + 0xc0 + i, String.fromCharCode(ch).toUpperCase().charCodeAt(0)); c.w8(t + 0x140 + i, String.fromCharCode(ch).toUpperCase().charCodeAt(0));
          }
          d0 = t; break;
        }
        default: break;                                 // Cursconf, Jenabint, Vsync… : sans effet ici
      }
    }
    c.d[0] = d0 >>> 0;
    return true;
  };

  /* ---------------------------------------------------------------- exécution */
  Engine.prototype.vbl = function () {
    var c = this.cpu;
    if (c.ipl >= 4) return;
    var sp = c.a[7], s = c.s, depth = 0, n = 0;
    c.interrupt(4);
    // exécute le gestionnaire jusqu'à son RTE
    var retSp = s ? sp : c.ssp;
    while (n++ < 200000) {
      var op = c.r16(c.pc);
      c.step();
      if (op === 0x4e73 && c.s === s && (c.a[7] >>> 0) === (sp >>> 0)) break;
    }
    c.w32(0x462, c.r32(0x462) + 1); c.w32(0x466, c.r32(0x466) + 1);
    this.vblCount = (this.vblCount || 0) + 1;
    if (this.onVbl) this.onVbl(this.vblCount);
  };

  /* exécute jusqu'à ce que pc == stopAt (ou limite), en injectant des VBL */
  Engine.prototype.runUntil = function (stopAt, limit) {
    var c = this.cpu, n = 0;
    limit = limit || 5e7;
    while (c.pc !== stopAt) {
      if (c.pc === A.waitVbl) c.mem[A.vblCounter] = 0;   // pas d'attente active
      if (c.pc === A.getKey && this.keyScript) {         // menus : réponses scriptées
        var k = this.keyScript.length ? this.keyScript.shift() : 0x1c0d;
        if (this.log) this.log('touche $' + k.toString(16) + ' (appelant $' + c.r32(c.a[7]).toString(16) + ')');
        c.d[0] = ((c.d[0] & 0xffff0000) | k) >>> 0; c.c = 0;
        c.pc = c.r32(c.a[7]); c.a[7] = (c.a[7] + 4) >>> 0; continue;
      }
      if (!this.renderOriginal && c.pc === A.render3d) { c.pc = c.r32(c.a[7]); c.a[7] = (c.a[7] + 4) >>> 0; continue; }
      c.step();
      if (c.stopped) { this.vbl(); this.sinceVbl = 0; continue; }
      if (++this.sinceVbl >= this.vblEvery) { this.sinceVbl = 0; this.vbl(); }
      if (++n > limit) throw new Error('moteur : arrêt non atteint ($' + stopAt.toString(16) + '), pc=$' + c.pc.toString(16));
    }
    return n;
  };

  /* démarre le programme et s'arrête juste avant les menus */
  Engine.prototype.boot = function () {
    var c = this.cpu;
    c.pc = A.boot; c.s = 0; c.a[7] = 0x103da; c.ssp = 0x7000; c.ipl = 0;
    this.passChecksum();
    // menus d'avant la boucle principale (titre, mode, nom, pilotes) : un « utilisateur »
    // simulé appuie régulièrement sur le bouton, tape « A » puis Entrée
    var self = this, n = 0;
    this.keyScript = null;
    this.onVbl = function (v) {
      var ph = v % 60;
      self.setInput({ fire: ph >= 10 && ph < 15 });
      self.setKey(0x1e, ph >= 30 && ph < 33);
      self.setKey(0x1c, ph >= 45 && ph < 48);
    };
    n = this.runUntil(A.preMenu, 3e8);
    this.onVbl = null;
    this.setInput({}); this.setKey(0x1e, false); this.setKey(0x1c, false);
    if (c.pc !== A.preMenu) throw new Error('démarrage du jeu impossible (pc=$' + c.pc.toString(16) + ')');
    return n;
  };

  /* exécute le démarrage jusqu'après la somme de contrôle du code, puis applique les réglages */
  Engine.prototype.passChecksum = function () {
    this.runUntil(CHECKSUM_DONE, 5e6);
    this.patchable = true;
    this.applyTuning();
  };

  /* entraînement sur le circuit `track` (0..7) : chargement, grue, jusqu'au premier tick */
  Engine.prototype.startPractice = function (track) {
    var c = this.cpu;
    c.mem[A.trackSel] = track & 7;
    // la vue d'ensemble attend « feu » : on répond immédiatement C=0
    c.w16(A.overviewWait, 0x44fc); c.w16(A.overviewWait + 2, 0x0000); c.w16(A.overviewWait + 4, 0x4e75);
    var SENT = HLE + 0x40;
    c.a[7] = (c.a[7] - 4) >>> 0; c.w32(c.a[7], SENT);
    c.pc = A.overview;
    this.runUntil(SENT, 8e7);
    c.a[7] = (c.a[7] - 4) >>> 0; c.w32(c.a[7], SENT);
    c.pc = A.race;
    return this.runUntil(A.loopTop, 8e7);
  };

  /* ------------------------------------------------------------ réglages de la physique d'origine
   * Constantes identifiées dans le code (voir docs/RETRO_INGENIERIE.md, §8) et réécrites
   * dans la mémoire du jeu.  Valeurs exprimées en multiplicateurs de l'original. */
  var DT_SITES = [0x48a08, 0x4efaa, 0x4efc6, 0x4efe2, 0x4f014, 0x4f02c, 0x4f044, 0x4f136, 0x4f14e, 0x4f166,
                  0x4f180, 0x4f198, 0x4f1b0, 0x507fe, 0x509ea, 0x50ed4, 0x50eec, 0x50f06, 0x50f1e, 0x50f38, 0x50f50];
  var CAR_TABLE = 0x1455a, CAR_BLOCK = 0x108e2, CHECKSUM_DONE = 0x1041e;

  Engine.prototype.setTuning = function (t) { this.tuning = t; if (this.patchable) this.applyTuning(); };

  Engine.prototype.applyTuning = function () {
    var c = this.cpu, t = this.tuning || {}, m = c.mem, i;
    if (!this.orig) {   // valeurs d'origine, lues une fois
      this.orig = { car: Array.prototype.slice.call(m, CAR_TABLE, CAR_TABLE + 22), g: c.r16(0x4e88c),
                    damp: c.r16(0x4ee64), dt: m[DT_SITES[0] + 3] };
    }
    var o = this.orig;
    function k(v) { return v === undefined ? 1 : +v; }
    var g = Math.max(1, Math.min(0x7fff, Math.round(o.g * k(t.gravity))));
    c.w16(0x4e884, (-g) & 0xffff); c.w16(0x4e88c, g);
    var dt = Math.max(1, Math.min(255, Math.round(o.dt * k(t.timeStep))));
    for (i = 0; i < DT_SITES.length; i++) m[DT_SITES[i] + 3] = dt;
    c.w16(0x4ee64, Math.max(0, Math.min(0x7fff, Math.round(o.damp * k(t.damping)))));
    var brake = Math.max(0, Math.min(0x7fff, Math.round(240 * k(t.brake)))), bw = (-brake) & 0xffff;
    m[0x4af39] = bw & 0xff; m[0x4af3d] = bw >> 8;
    for (var blk = 0; blk < 2; blk++) {
      var b = o.car.slice(blk * 11, blk * 11 + 11);
      var thrust = Math.max(0, Math.min(0x7fff, Math.round((b[2] | (b[3] << 8)) * k(t.thrust))));
      b[2] = thrust & 0xff; b[3] = thrust >> 8;
      b[6] = Math.max(1, Math.min(255, Math.round(b[6] / Math.max(0.05, k(t.boostUse)))));
      b[9] = Math.max(0, Math.min(255, b[9] + (t.shockTolerance | 0)));
      for (i = 0; i < 11; i++) m[CAR_TABLE + blk * 11 + i] = b[i];
      // bloc actif (copié au départ de chaque course) : ligue normale ou super ligue ($110CA)
      if ((m[0x110ca] ? 1 : 0) === blk) for (i = 0; i < 11; i++) if (i === 2 || i === 3 || i === 6 || i === 9) m[CAR_BLOCK + i] = b[i];
    }
  };

  /* entrées : joystick {up, down, left, right, fire} */
  Engine.prototype.setInput = function (j) {
    var b = (j.up ? 1 : 0) | (j.down ? 2 : 0) | (j.left ? 4 : 0) | (j.right ? 8 : 0) | (j.fire ? 0x10 : 0);
    this.cpu.mem[A.joystick] = (~b) & 0xff;
  };
  Engine.prototype.setKey = function (scancode, down) { this.cpu.mem[A.keys + (scancode & 0x7f)] = down ? 0xb3 : 0; };

  /* un tick de jeu (1/8,33 s dans l'original) */
  Engine.prototype.tick = function (vbls) {
    var c = this.cpu;
    vbls = vbls === undefined ? 6 : vbls;
    for (var i = 0; i < vbls; i++) this.vbl();
    this.sinceVbl = 0;
    c.step();                                  // quitte le haut de boucle
    return this.runUntil(A.loopTop, 2e6);
  };

  /* temps réel : exécute l'équivalent d'une trame vidéo (50 Hz) puis l'interruption VBL */
  Engine.prototype.runFrame = function (instr) {
    var c = this.cpu, n = instr || this.vblEvery, k = 0;
    while (k < n) {
      if (c.stopped) break;
      if (!this.renderOriginal && c.pc === A.render3d) { c.pc = c.r32(c.a[7]); c.a[7] = (c.a[7] + 4) >>> 0; continue; }
      c.step(); k++;
    }
    this.vbl();
    return k;
  };

  /* écran ST basse résolution (320×200, 4 plans) -> RGBA */
  Engine.prototype.screenRGBA = function (out) {
    var m = this.cpu.mem, base = this.io.vbase & 0xfffffe, pal = new Uint32Array(16), i, x, y;
    for (i = 0; i < 16; i++) {
      var col = this.io.palette[i];
      var r = ((col >> 8) & 7) * 255 / 7 | 0, g = ((col >> 4) & 7) * 255 / 7 | 0, b = (col & 7) * 255 / 7 | 0;
      pal[i] = (255 << 24) | (b << 16) | (g << 8) | r;
    }
    var o32 = new Uint32Array(out.buffer, out.byteOffset, 320 * 200);
    for (y = 0; y < 200; y++) {
      var row = base + y * 160;
      for (x = 0; x < 20; x++) {
        var a = row + x * 8;
        if (a + 7 >= m.length) continue;
        var p0 = (m[a] << 8) | m[a + 1], p1 = (m[a + 2] << 8) | m[a + 3], p2 = (m[a + 4] << 8) | m[a + 5], p3 = (m[a + 6] << 8) | m[a + 7];
        for (var bit = 15; bit >= 0; bit--) {
          var ci = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3);
          o32[y * 320 + x * 16 + (15 - bit)] = pal[ci];
        }
      }
    }
  };

  /* clavier ST : scancode -> table des touches du jeu */
  Engine.SCANCODES = {
    Escape: 0x01, Digit1: 0x02, Digit2: 0x03, Digit3: 0x04, Digit4: 0x05, Digit5: 0x06, Digit6: 0x07, Digit7: 0x08,
    Digit8: 0x09, Digit9: 0x0a, Digit0: 0x0b, Minus: 0x0c, Equal: 0x0d, Backspace: 0x0e, Tab: 0x0f,
    KeyQ: 0x10, KeyW: 0x11, KeyE: 0x12, KeyR: 0x13, KeyT: 0x14, KeyY: 0x15, KeyU: 0x16, KeyI: 0x17, KeyO: 0x18, KeyP: 0x19,
    BracketLeft: 0x1a, BracketRight: 0x1b, Enter: 0x1c, ControlLeft: 0x1d, KeyA: 0x1e, KeyS: 0x1f, KeyD: 0x20, KeyF: 0x21,
    KeyG: 0x22, KeyH: 0x23, KeyJ: 0x24, KeyK: 0x25, KeyL: 0x26, Semicolon: 0x27, Quote: 0x28, Backquote: 0x29,
    ShiftLeft: 0x2a, Backslash: 0x2b, KeyZ: 0x2c, KeyX: 0x2d, KeyC: 0x2e, KeyV: 0x2f, KeyB: 0x30, KeyN: 0x31, KeyM: 0x32,
    Comma: 0x33, Period: 0x34, Slash: 0x35, ShiftRight: 0x36, AltLeft: 0x38, Space: 0x39, CapsLock: 0x3a,
    F1: 0x3b, F2: 0x3c, F3: 0x3d, F4: 0x3e, F5: 0x3f, F6: 0x40, F7: 0x41, F8: 0x42, F9: 0x43, F10: 0x44,
    Home: 0x47, ArrowUp: 0x48, ArrowLeft: 0x4b, ArrowRight: 0x4d, ArrowDown: 0x50, Insert: 0x52, Delete: 0x53,
    NumpadEnter: 0x72
  };

  Engine.prototype.b = function (a) { return this.cpu.mem[a]; };
  Engine.prototype.w = function (a) { return (this.cpu.mem[a] << 8) | this.cpu.mem[a + 1]; };
  Engine.prototype.sw = function (a) { var v = this.w(a); return v & 0x8000 ? v - 0x10000 : v; };
  Engine.prototype.l = function (a) { return this.cpu.r32(a) | 0; };

  return Engine;
})();
