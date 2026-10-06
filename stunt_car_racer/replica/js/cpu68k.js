/*
 * cpu68k.js — interpréteur Motorola 68000 (jeu d'instructions entier complet, sans
 * comptage de cycles).  Sert à exécuter, dans la réplique, les routines logiques
 * d'origine (physique, IA…) lues depuis la disquette de l'utilisateur.
 * Validé par comparaison instruction par instruction avec Musashi (tools/stemu).
 */
var SCR = window.SCR || (window.SCR = {});

SCR.CPU = (function () {
  'use strict';

  var MASK = [0, 0xff, 0xffff, 0, 0xffffffff];
  var MSB = [0, 0x80, 0x8000, 0, 0x80000000];

  function CPU(memSize) {
    this.mem = new Uint8Array(memSize || 0x100000);
    this.d = [0, 0, 0, 0, 0, 0, 0, 0];
    this.a = [0, 0, 0, 0, 0, 0, 0, 0];
    this.pc = 0;
    this.s = 1; this.ipl = 7; this.t = 0;
    this.x = 0; this.n = 0; this.z = 0; this.v = 0; this.c = 0;
    this.usp = 0; this.ssp = 0;
    this.io = null;          // fonction(addr, size, value|undefined) pour les accès hors RAM
    this.trap = null;        // fonction(numéro de vecteur) -> true si géré
    this.stopped = false;
    this.count = 0;
  }

  /* ------------------------------------------------------------ mémoire */
  CPU.prototype.r8 = function (a) {
    a &= 0xffffff;
    if (a < this.mem.length) return this.mem[a];
    return this.io ? this.io(a, 1) & 0xff : 0xff;
  };
  CPU.prototype.r16 = function (a) {
    a &= 0xffffff;
    if (a + 1 < this.mem.length) return (this.mem[a] << 8) | this.mem[a + 1];
    return ((this.r8(a) << 8) | this.r8(a + 1)) >>> 0;
  };
  CPU.prototype.r32 = function (a) { return ((this.r16(a) << 16) | this.r16(a + 2)) >>> 0; };
  CPU.prototype.w8 = function (a, v) {
    a &= 0xffffff;
    if (a < this.mem.length) { this.mem[a] = v; return; }
    if (this.io) this.io(a, 1, v & 0xff);
  };
  CPU.prototype.w16 = function (a, v) {
    a &= 0xffffff;
    if (a + 1 < this.mem.length) { this.mem[a] = v >>> 8; this.mem[a + 1] = v; return; }
    this.w8(a, v >>> 8); this.w8(a + 1, v);
  };
  CPU.prototype.w32 = function (a, v) { this.w16(a, v >>> 16); this.w16(a + 2, v & 0xffff); };
  CPU.prototype.rd = function (a, sz) { return sz === 1 ? this.r8(a) : sz === 2 ? this.r16(a) : this.r32(a); };
  CPU.prototype.wr = function (a, sz, v) { if (sz === 1) this.w8(a, v); else if (sz === 2) this.w16(a, v); else this.w32(a, v); };

  CPU.prototype.fetch16 = function () { var v = this.r16(this.pc); this.pc = (this.pc + 2) >>> 0; return v; };
  CPU.prototype.fetch32 = function () { var v = this.r32(this.pc); this.pc = (this.pc + 4) >>> 0; return v; };

  /* ------------------------------------------------------------ registre d'état */
  CPU.prototype.getSR = function () {
    return (this.t << 15) | (this.s << 13) | (this.ipl << 8) | (this.x << 4) | (this.n << 3) | (this.z << 2) | (this.v << 1) | this.c;
  };
  CPU.prototype.setCCR = function (v) { this.x = (v >> 4) & 1; this.n = (v >> 3) & 1; this.z = (v >> 2) & 1; this.v = (v >> 1) & 1; this.c = v & 1; };
  CPU.prototype.setSR = function (v) {
    var ns = (v >> 13) & 1;
    if (ns !== this.s) {
      if (ns) { this.usp = this.a[7]; this.a[7] = this.ssp; } else { this.ssp = this.a[7]; this.a[7] = this.usp; }
      this.s = ns;
    }
    this.t = (v >> 15) & 1; this.ipl = (v >> 8) & 7; this.setCCR(v);
  };

  function sx8(v) { return (v << 24) >> 24; }
  function sx16(v) { return (v << 16) >> 16; }
  function sext(v, sz) { return sz === 1 ? sx8(v) : sz === 2 ? sx16(v) : v | 0; }

  /* ------------------------------------------------------------ adressage */
  // renvoie une « référence » : {k: 0 Dn, 1 An, 2 mémoire, 3 immédiat, r, addr, val}
  CPU.prototype.ea = function (mode, reg, sz) {
    var addr, ext, idx;
    switch (mode) {
      case 0: return { k: 0, r: reg };
      case 1: return { k: 1, r: reg };
      case 2: return { k: 2, addr: this.a[reg] >>> 0 };
      case 3:
        addr = this.a[reg] >>> 0;
        this.a[reg] = (addr + (reg === 7 && sz === 1 ? 2 : sz)) >>> 0;
        return { k: 2, addr: addr };
      case 4:
        this.a[reg] = (this.a[reg] - (reg === 7 && sz === 1 ? 2 : sz)) >>> 0;
        return { k: 2, addr: this.a[reg] >>> 0 };
      case 5: return { k: 2, addr: (this.a[reg] + sx16(this.fetch16())) >>> 0 };
      case 6:
        ext = this.fetch16();
        idx = (ext & 0x8000) ? this.a[(ext >> 12) & 7] : this.d[(ext >> 12) & 7];
        if (!(ext & 0x800)) idx = sx16(idx & 0xffff);
        return { k: 2, addr: (this.a[reg] + sx8(ext & 0xff) + idx) >>> 0 };
      case 7:
        switch (reg) {
          case 0: return { k: 2, addr: sx16(this.fetch16()) >>> 0 };
          case 1: return { k: 2, addr: this.fetch32() };
          case 2: addr = this.pc; return { k: 2, addr: (addr + sx16(this.fetch16())) >>> 0 };
          case 3:
            addr = this.pc; ext = this.fetch16();
            idx = (ext & 0x8000) ? this.a[(ext >> 12) & 7] : this.d[(ext >> 12) & 7];
            if (!(ext & 0x800)) idx = sx16(idx & 0xffff);
            return { k: 2, addr: (addr + sx8(ext & 0xff) + idx) >>> 0 };
          case 4:
            if (sz === 4) return { k: 3, val: this.fetch32() };
            var w = this.fetch16();
            return { k: 3, val: sz === 1 ? w & 0xff : w };
        }
    }
    throw new Error('mode d\'adressage invalide ' + mode + '/' + reg);
  };
  CPU.prototype.get = function (e, sz) {
    switch (e.k) {
      case 0: return (this.d[e.r] & MASK[sz]) >>> 0;
      case 1: return (this.a[e.r] & MASK[sz]) >>> 0;
      case 2: return this.rd(e.addr, sz);
      case 3: return e.val >>> 0;
    }
  };
  CPU.prototype.set = function (e, sz, v) {
    v = (v & MASK[sz]) >>> 0;
    switch (e.k) {
      case 0: this.d[e.r] = sz === 4 ? v : ((this.d[e.r] & ~MASK[sz]) | v) >>> 0; return;
      case 1: this.a[e.r] = sz === 4 ? v : (sx16(v) >>> 0); return;
      case 2: this.wr(e.addr, sz, v); return;
    }
  };

  /* ------------------------------------------------------------ drapeaux */
  CPU.prototype.flagsLogic = function (r, sz) {
    r = (r & MASK[sz]) >>> 0;
    this.n = (r & MSB[sz]) ? 1 : 0; this.z = r === 0 ? 1 : 0; this.v = 0; this.c = 0;
    return r;
  };
  CPU.prototype.add = function (s, d, sz, x) {
    var m = MASK[sz], b = MSB[sz];
    var r = (d + s + (x || 0));
    var rr = (r & m) >>> 0;
    this.c = this.x = r > m ? 1 : 0;
    if (sz === 4) this.c = this.x = (d + s + (x || 0)) > 0xffffffff ? 1 : 0;
    this.v = ((~(s ^ d) & (s ^ rr)) & b) ? 1 : 0;
    this.n = (rr & b) ? 1 : 0;
    return rr;
  };
  CPU.prototype.sub = function (s, d, sz, x) {   // d - s - x
    var m = MASK[sz], b = MSB[sz];
    var r = d - s - (x || 0);
    var rr = (r & m) >>> 0;
    this.c = this.x = r < 0 ? 1 : 0;
    this.v = (((s ^ d) & (rr ^ d)) & b) ? 1 : 0;
    this.n = (rr & b) ? 1 : 0;
    return rr;
  };
  CPU.prototype.cond = function (cc) {
    switch (cc) {
      case 0: return true; case 1: return false;
      case 2: return !this.c && !this.z; case 3: return !!(this.c || this.z);
      case 4: return !this.c; case 5: return !!this.c;
      case 6: return !this.z; case 7: return !!this.z;
      case 8: return !this.v; case 9: return !!this.v;
      case 10: return !this.n; case 11: return !!this.n;
      case 12: return this.n === this.v; case 13: return this.n !== this.v;
      case 14: return !this.z && this.n === this.v; case 15: return !!this.z || this.n !== this.v;
    }
  };

  /* ------------------------------------------------------------ exceptions */
  CPU.prototype.exception = function (vec, pcOverride) {
    if (this.trap && this.trap(vec, this)) return;
    var sr = this.getSR();
    this.setSR((sr | 0x2000) & ~0x8000);
    this.a[7] = (this.a[7] - 4) >>> 0; this.w32(this.a[7], pcOverride !== undefined ? pcOverride : this.pc);
    this.a[7] = (this.a[7] - 2) >>> 0; this.w16(this.a[7], sr);
    this.pc = this.r32(vec * 4);
  };
  CPU.prototype.interrupt = function (level, vector) {
    if (level <= this.ipl && level < 7) return false;
    this.stopped = false;
    var sr = this.getSR();
    this.setSR((sr | 0x2000) & ~0x8000);
    this.ipl = level;
    this.a[7] = (this.a[7] - 4) >>> 0; this.w32(this.a[7], this.pc);
    this.a[7] = (this.a[7] - 2) >>> 0; this.w16(this.a[7], sr);
    this.pc = this.r32((vector !== undefined ? vector : 24 + level) * 4);
    return true;
  };

  var SIZE_STD = [1, 2, 4];     // bits 7-6 : 00 octet, 01 mot, 10 long

  /* ------------------------------------------------------------ exécution */
  CPU.prototype.step = function () {
    if (this.stopped) return;
    var start = this.pc;
    var op = this.fetch16();
    this.count++;
    var hi = op >> 12;
    try {
      switch (hi) {
        case 0: this.op0(op); break;
        case 1: case 2: case 3: this.opMove(op); break;
        case 4: this.op4(op); break;
        case 5: this.op5(op); break;
        case 6: this.op6(op); break;
        case 7: // MOVEQ
          if (op & 0x100) this.exception(4, start);
          else { var r = (op >> 9) & 7; this.d[r] = sx8(op & 0xff) >>> 0; this.flagsLogic(this.d[r], 4); }
          break;
        case 8: this.op8(op); break;
        case 9: this.opAddSub(op, false); break;
        case 10: this.exception(10, start); break;
        case 11: this.opB(op); break;
        case 12: this.opC(op); break;
        case 13: this.opAddSub(op, true); break;
        case 14: this.opShift(op); break;
        case 15: this.exception(11, start); break;
      }
    } catch (e) {
      if (e && e.illegal) { this.pc = start; this.exception(4, start); }
      else throw e;
    }
  };

  function illegal() { var e = new Error('illégal'); e.illegal = true; throw e; }

  /* groupe 0 : immédiats, bits, MOVEP */
  CPU.prototype.op0 = function (op) {
    var mode = (op >> 3) & 7, reg = op & 7;
    if (op & 0x100 || (op & 0xf00) === 0x800) {
      // opérations de bit
      if ((op & 0x138) === 0x108) { // MOVEP
        var dr = (op >> 9) & 7, addr = (this.a[reg] + sx16(this.fetch16())) >>> 0, om = (op >> 6) & 3;
        if (om === 0) this.d[dr] = ((this.d[dr] & 0xffff0000) | (this.r8(addr) << 8) | this.r8(addr + 2)) >>> 0;
        else if (om === 1) this.d[dr] = ((this.r8(addr) << 24) | (this.r8(addr + 2) << 16) | (this.r8(addr + 4) << 8) | this.r8(addr + 6)) >>> 0;
        else if (om === 2) { this.w8(addr, this.d[dr] >>> 8); this.w8(addr + 2, this.d[dr]); }
        else { this.w8(addr, this.d[dr] >>> 24); this.w8(addr + 2, this.d[dr] >>> 16); this.w8(addr + 4, this.d[dr] >>> 8); this.w8(addr + 6, this.d[dr]); }
        return;
      }
      var bit = (op & 0x100) ? this.d[(op >> 9) & 7] : this.fetch16() & 0xff;
      var type = (op >> 6) & 3;
      if (mode === 0) {
        bit &= 31;
        var v = this.d[reg];
        this.z = (v >>> bit) & 1 ? 0 : 1;
        if (type === 1) this.d[reg] = (v ^ (1 << bit)) >>> 0;
        else if (type === 2) this.d[reg] = (v & ~(1 << bit)) >>> 0;
        else if (type === 3) this.d[reg] = (v | (1 << bit)) >>> 0;
      } else {
        bit &= 7;
        var e = this.ea(mode, reg, 1), b = this.get(e, 1);
        this.z = (b >> bit) & 1 ? 0 : 1;
        if (type === 1) this.set(e, 1, b ^ (1 << bit));
        else if (type === 2) this.set(e, 1, b & ~(1 << bit));
        else if (type === 3) this.set(e, 1, b | (1 << bit));
      }
      return;
    }
    var kind = (op >> 9) & 7, szb = (op >> 6) & 3;
    if (szb === 3) illegal();
    var sz = SIZE_STD[szb];
    // vers CCR / SR
    if (mode === 7 && reg === 4 && (kind === 0 || kind === 1 || kind === 5)) {
      var im = this.fetch16();
      if (sz === 1) {
        var ccr = this.getSR() & 0xff;
        ccr = kind === 0 ? ccr | im : kind === 1 ? ccr & im : ccr ^ im;
        this.setCCR(ccr);
      } else {
        var sr = this.getSR();
        sr = kind === 0 ? sr | im : kind === 1 ? sr & im : sr ^ im;
        this.setSR(sr);
      }
      return;
    }
    var imm = sz === 4 ? this.fetch32() : sz === 2 ? this.fetch16() : this.fetch16() & 0xff;
    var e2 = this.ea(mode, reg, sz), d = this.get(e2, sz), r;
    switch (kind) {
      case 0: this.set(e2, sz, this.flagsLogic(d | imm, sz)); break;           // ORI
      case 1: this.set(e2, sz, this.flagsLogic(d & imm, sz)); break;           // ANDI
      case 2: r = this.sub(imm, d, sz); this.z = r === 0 ? 1 : 0; this.set(e2, sz, r); break;   // SUBI
      case 3: r = this.add(imm, d, sz); this.z = r === 0 ? 1 : 0; this.set(e2, sz, r); break;   // ADDI
      case 5: this.set(e2, sz, this.flagsLogic(d ^ imm, sz)); break;           // EORI
      case 6: { var x = this.x; r = this.sub(imm, d, sz); this.x = x; this.z = r === 0 ? 1 : 0; break; } // CMPI
      default: illegal();
    }
  };

  /* MOVE / MOVEA */
  CPU.prototype.opMove = function (op) {
    var sz = [0, 1, 4, 2][op >> 12];
    var src = this.ea((op >> 3) & 7, op & 7, sz), v = this.get(src, sz);
    var dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
    if (dmode === 1) { this.a[dreg] = sz === 2 ? sx16(v) >>> 0 : v >>> 0; return; }
    var dst = this.ea(dmode, dreg, sz);
    this.set(dst, sz, v);
    this.flagsLogic(v, sz);
  };

  /* groupe 4 : divers */
  CPU.prototype.op4 = function (op) {
    var mode = (op >> 3) & 7, reg = op & 7, e, v, sz, r, i;
    if ((op & 0x1c0) === 0x1c0) { // LEA
      e = this.ea(mode, reg, 4); this.a[(op >> 9) & 7] = e.addr >>> 0; return;
    }
    if ((op & 0x1c0) === 0x180) { // CHK.W
      e = this.ea(mode, reg, 2); var bound = sx16(this.get(e, 2)), dv = sx16(this.d[(op >> 9) & 7] & 0xffff);
      if (dv < 0) { this.n = 1; this.exception(6); } else if (dv > bound) { this.n = 0; this.exception(6); }
      return;
    }
    switch ((op >> 8) & 0xf) {
      case 0x0: // NEGX / MOVE from SR
        if ((op & 0xc0) === 0xc0) { e = this.ea(mode, reg, 2); this.set(e, 2, this.getSR()); return; }
        sz = SIZE_STD[(op >> 6) & 3]; e = this.ea(mode, reg, sz); v = this.get(e, sz);
        { var z = this.z; r = this.sub(v, 0, sz, this.x); this.z = r === 0 ? z : 0; }
        this.set(e, sz, r); return;
      case 0x2: // CLR
        if ((op & 0xc0) === 0xc0) illegal();
        sz = SIZE_STD[(op >> 6) & 3]; e = this.ea(mode, reg, sz); this.get(e, sz); this.set(e, sz, 0);
        this.n = 0; this.z = 1; this.v = 0; this.c = 0; return;
      case 0x4: // NEG / MOVE to CCR
        if ((op & 0xc0) === 0xc0) { e = this.ea(mode, reg, 2); this.setCCR(this.get(e, 2)); return; }
        sz = SIZE_STD[(op >> 6) & 3]; e = this.ea(mode, reg, sz); v = this.get(e, sz);
        r = this.sub(v, 0, sz); this.z = r === 0 ? 1 : 0; this.set(e, sz, r); return;
      case 0x6: // NOT / MOVE to SR
        if ((op & 0xc0) === 0xc0) { e = this.ea(mode, reg, 2); this.setSR(this.get(e, 2)); return; }
        sz = SIZE_STD[(op >> 6) & 3]; e = this.ea(mode, reg, sz); v = this.get(e, sz);
        this.set(e, sz, this.flagsLogic(~v, sz)); return;
      case 0x8:
        switch ((op >> 6) & 3) {
          case 0: // NBCD
            e = this.ea(mode, reg, 1); v = this.get(e, 1);
            r = this.bcdSub(v, 0); this.set(e, 1, r); return;
          case 1:
            if (mode === 0) { // SWAP
              v = this.d[reg]; this.d[reg] = ((v >>> 16) | (v << 16)) >>> 0; this.flagsLogic(this.d[reg], 4); return;
            }
            e = this.ea(mode, reg, 4); // PEA
            this.a[7] = (this.a[7] - 4) >>> 0; this.w32(this.a[7], e.addr); return;
          case 2:
            if (mode === 0) { v = sx8(this.d[reg] & 0xff) & 0xffff; this.d[reg] = ((this.d[reg] & 0xffff0000) | v) >>> 0; this.flagsLogic(v, 2); return; } // EXT.W
            this.movemToMem(op, 2); return;
          case 3:
            if (mode === 0) { v = sx16(this.d[reg] & 0xffff) >>> 0; this.d[reg] = v; this.flagsLogic(v, 4); return; } // EXT.L
            this.movemToMem(op, 4); return;
        }
        break;
      case 0xa: // TST / TAS / ILLEGAL
        if (op === 0x4afc) illegal();
        if ((op & 0xc0) === 0xc0) { e = this.ea(mode, reg, 1); v = this.get(e, 1); this.flagsLogic(v, 1); this.set(e, 1, v | 0x80); return; }
        sz = SIZE_STD[(op >> 6) & 3]; e = this.ea(mode, reg, sz); this.flagsLogic(this.get(e, sz), sz); return;
      case 0xc: // MOVEM vers registres
        if ((op & 0x80) === 0) illegal();
        this.movemToReg(op, (op & 0x40) ? 4 : 2); return;
      case 0xe:
        if ((op & 0xfff0) === 0x4e40) { this.exception(32 + (op & 15)); return; } // TRAP
        if ((op & 0xfff8) === 0x4e50) { // LINK
          var disp = sx16(this.fetch16());
          this.a[7] = (this.a[7] - 4) >>> 0; this.w32(this.a[7], this.a[reg]);
          this.a[reg] = this.a[7]; this.a[7] = (this.a[7] + disp) >>> 0; return;
        }
        if ((op & 0xfff8) === 0x4e58) { this.a[7] = this.a[reg]; this.a[reg] = this.r32(this.a[7]); this.a[7] = (this.a[7] + 4) >>> 0; return; } // UNLK
        if ((op & 0xfff8) === 0x4e60) { this.usp = this.a[reg]; return; }  // MOVE An,USP
        if ((op & 0xfff8) === 0x4e68) { this.a[reg] = this.usp; return; }  // MOVE USP,An
        switch (op) {
          case 0x4e70: return;                                      // RESET
          case 0x4e71: return;                                      // NOP
          case 0x4e72: this.setSR(this.fetch16()); this.stopped = true; return;   // STOP
          case 0x4e73: v = this.r16(this.a[7]); this.pc = this.r32(this.a[7] + 2); this.a[7] = (this.a[7] + 6) >>> 0; this.setSR(v); return; // RTE
          case 0x4e75: this.pc = this.r32(this.a[7]); this.a[7] = (this.a[7] + 4) >>> 0; return;     // RTS
          case 0x4e76: if (this.v) this.exception(7); return;       // TRAPV
          case 0x4e77: this.setCCR(this.r16(this.a[7])); this.pc = this.r32(this.a[7] + 2); this.a[7] = (this.a[7] + 6) >>> 0; return; // RTR
        }
        if ((op & 0xffc0) === 0x4e80) { e = this.ea(mode, reg, 4); this.a[7] = (this.a[7] - 4) >>> 0; this.w32(this.a[7], this.pc); this.pc = e.addr; return; } // JSR
        if ((op & 0xffc0) === 0x4ec0) { e = this.ea(mode, reg, 4); this.pc = e.addr; return; } // JMP
        illegal();
    }
    illegal();
  };

  CPU.prototype.movemToMem = function (op, sz) {
    var mask = this.fetch16(), mode = (op >> 3) & 7, reg = op & 7, i, addr;
    if (mode === 4) { // -(An) : ordre inversé (bit 0 = A7)
      addr = this.a[reg] >>> 0;
      for (i = 0; i < 16; i++) if (mask & (1 << i)) {
        var r = 15 - i; addr = (addr - sz) >>> 0;
        var v = r < 8 ? this.d[r] : this.a[r - 8];
        this.wr(addr, sz, v);
      }
      this.a[reg] = addr; return;
    }
    var e = this.ea(mode, reg, sz); addr = e.addr;
    for (i = 0; i < 16; i++) if (mask & (1 << i)) {
      this.wr(addr, sz, i < 8 ? this.d[i] : this.a[i - 8]); addr = (addr + sz) >>> 0;
    }
  };
  CPU.prototype.movemToReg = function (op, sz) {
    var mask = this.fetch16(), mode = (op >> 3) & 7, reg = op & 7, i, addr;
    if (mode === 3) addr = this.a[reg] >>> 0;
    else addr = this.ea(mode, reg, sz).addr;
    for (i = 0; i < 16; i++) if (mask & (1 << i)) {
      var v = this.rd(addr, sz); if (sz === 2) v = sx16(v) >>> 0;
      if (i < 8) this.d[i] = v >>> 0; else this.a[i - 8] = v >>> 0;
      addr = (addr + sz) >>> 0;
    }
    if (mode === 3) this.a[reg] = addr;
  };

  /* groupe 5 : ADDQ / SUBQ / Scc / DBcc */
  CPU.prototype.op5 = function (op) {
    var mode = (op >> 3) & 7, reg = op & 7, e, v, r;
    if ((op & 0xc0) === 0xc0) {
      var cc = (op >> 8) & 15;
      if (mode === 1) { // DBcc
        var disp = sx16(this.fetch16()), base = (this.pc - 2) >>> 0;
        if (!this.cond(cc)) {
          var w = ((this.d[reg] & 0xffff) - 1) & 0xffff;
          this.d[reg] = ((this.d[reg] & 0xffff0000) | w) >>> 0;
          if (w !== 0xffff) this.pc = (base + disp) >>> 0;
        }
        return;
      }
      e = this.ea(mode, reg, 1); this.set(e, 1, this.cond(cc) ? 0xff : 0); return;  // Scc
    }
    var q = (op >> 9) & 7 || 8, sz = SIZE_STD[(op >> 6) & 3];
    if (mode === 1) { // adresse : pas de drapeaux, opération sur 32 bits
      this.a[reg] = (op & 0x100) ? (this.a[reg] - q) >>> 0 : (this.a[reg] + q) >>> 0; return;
    }
    e = this.ea(mode, reg, sz); v = this.get(e, sz);
    r = (op & 0x100) ? this.sub(q, v, sz) : this.add(q, v, sz);
    this.z = r === 0 ? 1 : 0;
    this.set(e, sz, r);
  };

  /* groupe 6 : branchements */
  CPU.prototype.op6 = function (op) {
    var cc = (op >> 8) & 15, disp = sx8(op & 0xff), base = this.pc;
    if (disp === 0) disp = sx16(this.fetch16());
    if (cc === 1) { // BSR
      this.a[7] = (this.a[7] - 4) >>> 0; this.w32(this.a[7], this.pc); this.pc = (base + disp) >>> 0; return;
    }
    if (this.cond(cc)) this.pc = (base + disp) >>> 0;
  };

  /* groupe 8 : OR / DIVU / DIVS / SBCD */
  CPU.prototype.op8 = function (op) {
    var mode = (op >> 3) & 7, reg = op & 7, dr = (op >> 9) & 7, e, v;
    if ((op & 0x1c0) === 0xc0 || (op & 0x1c0) === 0x1c0) { // DIVU / DIVS
      e = this.ea(mode, reg, 2); var div = this.get(e, 2), num = this.d[dr] >>> 0, q, rem;
      if (div === 0) { this.exception(5); return; }
      if (op & 0x100) {
        div = sx16(div); num = num | 0;
        q = (num / div) | 0; if (num / div < 0 && q !== num / div) { /* troncature vers 0 */ }
        q = Math.trunc(num / div); rem = num - q * div;
        if (q > 32767 || q < -32768) { this.v = 1; this.c = 0; return; }
      } else {
        q = Math.floor(num / div); rem = num - q * div;
        if (q > 0xffff) { this.v = 1; this.c = 0; return; }
      }
      this.d[dr] = (((rem & 0xffff) << 16) | (q & 0xffff)) >>> 0;
      this.n = (q & 0x8000) ? 1 : 0; this.z = (q & 0xffff) === 0 ? 1 : 0; this.v = 0; this.c = 0;
      return;
    }
    if ((op & 0x1f0) === 0x100) { // SBCD
      var s, d2;
      if (op & 8) { this.a[reg] = (this.a[reg] - (reg === 7 ? 2 : 1)) >>> 0; s = this.r8(this.a[reg]); this.a[dr] = (this.a[dr] - (dr === 7 ? 2 : 1)) >>> 0; d2 = this.r8(this.a[dr]); this.w8(this.a[dr], this.bcdSub(s, d2)); }
      else { s = this.d[reg] & 0xff; d2 = this.d[dr] & 0xff; this.d[dr] = ((this.d[dr] & 0xffffff00) | this.bcdSub(s, d2)) >>> 0; }
      return;
    }
    var sz = SIZE_STD[(op >> 6) & 3];
    e = this.ea(mode, reg, sz);
    if (op & 0x100) { v = this.get(e, sz) | this.d[dr]; this.set(e, sz, this.flagsLogic(v, sz)); }
    else { v = this.get(e, sz) | this.d[dr]; this.set({ k: 0, r: dr }, sz, this.flagsLogic(v, sz)); }
  };

  CPU.prototype.bcdSub = function (s, d) {   // d - s - X
    var lo = (d & 15) - (s & 15) - this.x, hi = (d >> 4) - (s >> 4), c = 0;
    if (lo < 0) { lo += 10; hi--; }
    if (hi < 0) { hi += 10; c = 1; }
    var r = ((hi << 4) | (lo & 15)) & 0xff;
    this.c = this.x = c; if (r) this.z = 0; this.n = (r & 0x80) ? 1 : 0;
    return r;
  };
  CPU.prototype.bcdAdd = function (s, d) {
    var lo = (d & 15) + (s & 15) + this.x, hi = (d >> 4) + (s >> 4), c = 0;
    if (lo > 9) { lo -= 10; hi++; }
    if (hi > 9) { hi -= 10; c = 1; }
    var r = ((hi << 4) | (lo & 15)) & 0xff;
    this.c = this.x = c; if (r) this.z = 0; this.n = (r & 0x80) ? 1 : 0;
    return r;
  };

  /* groupes 9 / D : SUB / ADD (+A, +X) */
  CPU.prototype.opAddSub = function (op, isAdd) {
    var mode = (op >> 3) & 7, reg = op & 7, dr = (op >> 9) & 7, opm = (op >> 6) & 7, e, s, d, r;
    if (opm === 3 || opm === 7) { // ADDA / SUBA
      var sz = opm === 3 ? 2 : 4;
      e = this.ea(mode, reg, sz); s = this.get(e, sz); if (sz === 2) s = sx16(s);
      this.a[dr] = isAdd ? (this.a[dr] + s) >>> 0 : (this.a[dr] - s) >>> 0; return;
    }
    var sz2 = SIZE_STD[opm & 3];
    if ((opm & 4) && mode < 2) { // ADDX / SUBX
      var z = this.z;
      if (mode === 0) {
        s = (this.d[reg] & MASK[sz2]) >>> 0; d = (this.d[dr] & MASK[sz2]) >>> 0;
        r = isAdd ? this.add(s, d, sz2, this.x) : this.sub(s, d, sz2, this.x);
        this.set({ k: 0, r: dr }, sz2, r);
      } else {
        var es = this.ea(4, reg, sz2); s = this.get(es, sz2); var ed = this.ea(4, dr, sz2); d = this.get(ed, sz2);
        r = isAdd ? this.add(s, d, sz2, this.x) : this.sub(s, d, sz2, this.x);
        this.set(ed, sz2, r);
      }
      this.z = r === 0 ? z : 0;
      return;
    }
    e = this.ea(mode, reg, sz2);
    if (opm & 4) { // <ea> = <ea> op Dn
      s = (this.d[dr] & MASK[sz2]) >>> 0; d = this.get(e, sz2);
      r = isAdd ? this.add(s, d, sz2) : this.sub(s, d, sz2); this.z = r === 0 ? 1 : 0; this.set(e, sz2, r);
    } else { // Dn = Dn op <ea>
      s = this.get(e, sz2); d = (this.d[dr] & MASK[sz2]) >>> 0;
      r = isAdd ? this.add(s, d, sz2) : this.sub(s, d, sz2); this.z = r === 0 ? 1 : 0; this.set({ k: 0, r: dr }, sz2, r);
    }
  };

  /* groupe B : CMP / CMPA / CMPM / EOR */
  CPU.prototype.opB = function (op) {
    var mode = (op >> 3) & 7, reg = op & 7, dr = (op >> 9) & 7, opm = (op >> 6) & 7, e, s, d, r, x = this.x;
    if (opm === 3 || opm === 7) { // CMPA
      var sz = opm === 3 ? 2 : 4; e = this.ea(mode, reg, sz); s = this.get(e, sz); if (sz === 2) s = sx16(s) >>> 0;
      r = this.sub(s >>> 0, this.a[dr] >>> 0, 4); this.x = x; this.z = r === 0 ? 1 : 0; return;
    }
    var sz2 = SIZE_STD[opm & 3];
    if (opm & 4) {
      if (mode === 1) { // CMPM
        var es = this.ea(3, reg, sz2); s = this.get(es, sz2); var ed = this.ea(3, dr, sz2); d = this.get(ed, sz2);
        r = this.sub(s, d, sz2); this.x = x; this.z = r === 0 ? 1 : 0; return;
      }
      e = this.ea(mode, reg, sz2); // EOR
      this.set(e, sz2, this.flagsLogic(this.get(e, sz2) ^ this.d[dr], sz2)); return;
    }
    e = this.ea(mode, reg, sz2); s = this.get(e, sz2); d = (this.d[dr] & MASK[sz2]) >>> 0;
    r = this.sub(s, d, sz2); this.x = x; this.z = r === 0 ? 1 : 0;
  };

  /* groupe C : AND / MULU / MULS / ABCD / EXG */
  CPU.prototype.opC = function (op) {
    var mode = (op >> 3) & 7, reg = op & 7, dr = (op >> 9) & 7, e, v, t;
    if ((op & 0x1c0) === 0xc0 || (op & 0x1c0) === 0x1c0) { // MULU / MULS
      e = this.ea(mode, reg, 2); var s = this.get(e, 2), d = this.d[dr] & 0xffff, r;
      if (op & 0x100) r = Math.imul(sx16(s), sx16(d)) >>> 0; else r = (s * d) >>> 0;
      this.d[dr] = r; this.flagsLogic(r, 4); return;
    }
    if ((op & 0x1f0) === 0x100) { // ABCD
      var a1, b1;
      if (op & 8) { this.a[reg] = (this.a[reg] - (reg === 7 ? 2 : 1)) >>> 0; a1 = this.r8(this.a[reg]); this.a[dr] = (this.a[dr] - (dr === 7 ? 2 : 1)) >>> 0; b1 = this.r8(this.a[dr]); this.w8(this.a[dr], this.bcdAdd(a1, b1)); }
      else { a1 = this.d[reg] & 0xff; b1 = this.d[dr] & 0xff; this.d[dr] = ((this.d[dr] & 0xffffff00) | this.bcdAdd(a1, b1)) >>> 0; }
      return;
    }
    if ((op & 0x1f8) === 0x140) { t = this.d[dr]; this.d[dr] = this.d[reg]; this.d[reg] = t; return; }   // EXG Dx,Dy
    if ((op & 0x1f8) === 0x148) { t = this.a[dr]; this.a[dr] = this.a[reg]; this.a[reg] = t; return; }   // EXG Ax,Ay
    if ((op & 0x1f8) === 0x188) { t = this.d[dr]; this.d[dr] = this.a[reg]; this.a[reg] = t; return; }   // EXG Dx,Ay
    var sz = SIZE_STD[(op >> 6) & 3];
    e = this.ea(mode, reg, sz);
    v = this.get(e, sz) & this.d[dr];
    if (op & 0x100) this.set(e, sz, this.flagsLogic(v, sz)); else this.set({ k: 0, r: dr }, sz, this.flagsLogic(v, sz));
  };

  /* groupe E : décalages et rotations */
  CPU.prototype.opShift = function (op) {
    var sz, cnt, type, left = (op & 0x100) !== 0, e, v, isMem = (op & 0xc0) === 0xc0, reg = op & 7;
    if (isMem) {
      type = (op >> 9) & 3; sz = 2; cnt = 1;
      e = this.ea((op >> 3) & 7, reg, 2); v = this.get(e, 2);
      this.set(e, 2, this.shift(type, left, v, 1, 2));
      return;
    }
    sz = SIZE_STD[(op >> 6) & 3]; type = (op >> 3) & 3;
    cnt = (op & 0x20) ? this.d[(op >> 9) & 7] & 63 : ((op >> 9) & 7) || 8;
    v = (this.d[reg] & MASK[sz]) >>> 0;
    this.set({ k: 0, r: reg }, sz, this.shift(type, left, v, cnt, sz));
  };
  CPU.prototype.shift = function (type, left, v, cnt, sz) {
    var bits = sz * 8, m = MASK[sz], msb = MSB[sz], i, c = 0, ovf = 0;
    v = v >>> 0;
    if (cnt === 0) {
      this.c = type === 2 ? this.x : 0; this.v = 0;
      this.n = (v & msb) ? 1 : 0; this.z = v === 0 ? 1 : 0; return v;
    }
    switch (type) {
      case 0: // AS
        if (left) {
          for (i = 0; i < cnt; i++) {
            c = (v & msb) ? 1 : 0; var nv = (v << 1) & m; if ((nv & msb) !== (v & msb)) ovf = 1; v = nv >>> 0;
          }
          this.c = this.x = c; this.v = ovf;
        } else {
          for (i = 0; i < cnt; i++) { c = v & 1; v = ((v >>> 1) | (v & msb)) >>> 0; }
          this.c = this.x = c; this.v = 0;
        }
        break;
      case 1: // LS
        if (left) { for (i = 0; i < cnt; i++) { c = (v & msb) ? 1 : 0; v = ((v << 1) & m) >>> 0; } }
        else { for (i = 0; i < cnt; i++) { c = v & 1; v = v >>> 1; } }
        this.c = this.x = c; this.v = 0;
        break;
      case 2: // ROX
        var x = this.x;
        for (i = 0; i < cnt; i++) {
          if (left) { c = (v & msb) ? 1 : 0; v = (((v << 1) & m) | x) >>> 0; }
          else { c = v & 1; v = ((v >>> 1) | (x ? msb : 0)) >>> 0; }
          x = c;
        }
        this.x = x; this.c = x; this.v = 0;
        break;
      case 3: // RO
        for (i = 0; i < cnt; i++) {
          if (left) { c = (v & msb) ? 1 : 0; v = (((v << 1) & m) | c) >>> 0; }
          else { c = v & 1; v = ((v >>> 1) | (c ? msb : 0)) >>> 0; }
        }
        this.c = c; this.v = 0;
        break;
    }
    v = (v & m) >>> 0;
    this.n = (v & msb) ? 1 : 0; this.z = v === 0 ? 1 : 0;
    return v;
  };

  /* exécute un sous-programme jusqu'à son RTS (adresse de retour sentinelle) */
  CPU.prototype.call = function (addr, maxSteps) {
    var SENT = 0xfffff0;
    this.a[7] = (this.a[7] - 4) >>> 0; this.w32(this.a[7], SENT);
    this.pc = addr;
    var n = maxSteps || 5e6;
    while (this.pc !== SENT) {
      this.step();
      if (--n <= 0) throw new Error('sous-programme $' + addr.toString(16) + ' : trop long (pc=$' + this.pc.toString(16) + ')');
      if (this.stopped) throw new Error('STOP dans un sous-programme');
    }
  };

  return CPU;
})();
