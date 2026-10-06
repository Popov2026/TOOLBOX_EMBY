/*
 * physics_orig.js — physique de Stunt Car Racer, décompilée en JavaScript lisible.
 *
 * Chaque fonction reproduit EXACTEMENT une routine 68000 du jeu (adresse en commentaire),
 * en virgule fixe 16 bits, sur la même mémoire de variables (M = vue sur la mémoire du jeu).
 * Exactitude vérifiée en « ombre » : test/shadow_test.js exécute l'original et le port
 * depuis le même état et compare toutes les écritures mémoire.
 *
 * Seules les CONSTANTES (pas de temps, gravité, amortisseur…) proviennent du programme
 * d'origine lu sur la disquette : elles sont passées dans `K` (voir SCR.OrigPhysics.constants).
 */
var SCR = window.SCR || (window.SCR = {});

SCR.OrigPhysics = (function () {
  'use strict';

  /* ------------------------------------------------------------ accès mémoire */
  function Mem(bytes) { this.m = bytes; }
  Mem.prototype.b = function (a) { return this.m[a]; };
  Mem.prototype.sb = function (a) { return (this.m[a] << 24) >> 24; };
  Mem.prototype.wb = function (a, v) { this.m[a] = v; };
  Mem.prototype.w = function (a) { return (this.m[a] << 8) | this.m[a + 1]; };
  Mem.prototype.sw = function (a) { return ((this.m[a] << 24) >> 16) | this.m[a + 1]; };
  Mem.prototype.ww = function (a, v) { this.m[a] = (v >> 8) & 0xff; this.m[a + 1] = v & 0xff; };
  Mem.prototype.l = function (a) { return ((this.m[a] << 24) | (this.m[a + 1] << 16) | (this.m[a + 2] << 8) | this.m[a + 3]) | 0; };
  Mem.prototype.wl = function (a, v) { this.m[a] = (v >>> 24) & 0xff; this.m[a + 1] = (v >>> 16) & 0xff; this.m[a + 2] = (v >>> 8) & 0xff; this.m[a + 3] = v & 0xff; };

  function s16(v) { return (v << 16) >> 16; }
  /* (a × b) >> 8 sur 32 bits, puis mot de poids faible (muls.w ; asr.l #8) */
  function mulAsr8(a, b) { return Math.floor((a * b) / 256) | 0; }
  /* multiplication Q15 du jeu : muls.w ; asl.l #1 ; swap -> mot haut de 2·a·b */
  function q15(a, b) { return ((a * b * 2) | 0) >> 16; }

  /* ------------------------------------------------------------ variables */
  var V = {
    posX: 0x10ac2, posY: 0x10ac6, posZ: 0x10aca,           // 16.16 ; 128 unités par case
    pitch: 0x10ace, yaw: 0x10ad0, roll: 0x10ad2,         // 0x10000 = 360°
    velX: 0x10ad4, velY: 0x10ad6, velZ: 0x10ad8,         // vitesse dans le repère monde
    angVelPitch: 0x10ada, angVelYaw: 0x10adc, angVelRoll: 0x10ade,
    accX: 0x10ae0, accY: 0x10ae2, accZ: 0x10ae4,
    angAccPitch: 0x10ae6, angAccYaw: 0x10ae8, angAccRoll: 0x10aea,
    angRate: 0x10b24,                                    // 3 mots : dérivées des angles
    matrix: 0x6eedc                                      // matrice d'orientation (Q15)
  };

  /* constantes lues dans le programme d'origine */
  function constants(mem) {
    var M = new Mem(mem);
    return {
      dt: M.b(0x4efaa + 3),                    // facteur de pas 0xEE (/256)
      gravity: M.w(0x4e88c),                   // 0x13D
      damping: M.w(0x4ee64),                   // 0x114
      limits: [M.w(0x4f128), M.w(0x4f12a), M.w(0x4f12c), M.w(0x4f12e)],
      sinTable: 0x11130                        // quart d'onde, 513 mots (remplie au démarrage)
    };
  }

  /* ------------------------------------------------------------ $51A88 / $51A90 : cos / sin */
  function trig(M, K, angle, phase) {
    var d0 = angle & 0xffff, d3 = d0 & 0x3fff;
    if (((d0 & 0x4000) ^ phase) === 0) d3 = ((d3 ^ 0x3fff) + 1) & 0xffff;
    d3 = ((d3 >>> 5) | (d3 << 11)) & 0xffff;              // ror.w #5
    var d4 = d3 & 0x3fe;
    var t0 = M.w(K.sinTable + d4), t1 = M.w(K.sinTable + d4 + 2);
    var d6 = (t0 - t1) & 0xffff;
    d3 = ((d3 >>> 1) | (d3 << 15)) & 0xfc00;               // ror.w #1 ; and #$fc00
    d6 = ((d6 * d3) >>> 16) & 0xffff;                      // mulu.w ; swap
    var d7 = ((t0 - d6) & 0xffff) >>> 1;
    if (((d0 ^ (((d0 & phase) << 1) & 0xffff)) & 0x8000) !== 0) d7 = (-d7) & 0xffff;
    return s16(d7);
  }
  function cos(M, K, a) { return trig(M, K, a, 0); }
  function sin(M, K, a) { return trig(M, K, a, 0x4000); }

  /* ------------------------------------------------------------ $4E8B2 : matrice d'orientation */
  function buildMatrix(M, K) {
    var A = V.matrix, i;
    var yaw = M.w(0x10ad0), pitch = M.w(0x10ace), roll = M.w(0x10ad2);
    var cy = cos(M, K, yaw), sy = sin(M, K, yaw);
    [4, 0xc, 0xe, 0x14, 0x16].forEach(function (o) { M.ww(A + o, cy); });
    [6, 0x10, 0x12, 0x18, 0x1a].forEach(function (o) { M.ww(A + o, sy); });
    var d = (yaw - M.w(0x10b44)) & 0xffff;
    var cd = cos(M, K, d), sd = sin(M, K, d);
    [0x34, 0x42, 0x44].forEach(function (o) { M.ww(A + o, cd); });
    [0x38, 0x3e, 0x46].forEach(function (o) { M.ww(A + o, sd); });
    M.ww(A + 8, cos(M, K, pitch));
    var sp = sin(M, K, pitch);
    [0xa, 0x1c, 0x1e].forEach(function (o) { M.ww(A + o, sp); });
    M.ww(A + 0x22, sin(M, K, roll));
    M.ww(A + 0x20, cos(M, K, roll));
    function scale(from, to, step, by) {
      var f = M.sw(A + by);
      for (var o = from; o <= to; o += step) M.ww(A + o, q15(M.sw(A + o), f));
    }
    scale(0xc, 0x12, 2, 8);       // × cos(tangage)
    scale(0x34, 0x38, 4, 8);
    M.ww(A, M.w(A + 0xc)); M.ww(A + 2, M.w(A + 0x10));
    scale(4, 6, 2, 0xa);          // × sin(tangage)
    scale(0x44, 0x46, 2, 0xa);
    scale(0xc, 0x1c, 4, 0x20);    // × cos(roulis)
    scale(0x34, 0x38, 4, 0x20);
    scale(0xe, 0x1e, 4, 0x22);    // × sin(roulis)
    scale(0x3e, 0x42, 4, 0x22);
    M.ww(A + 0x28, (M.w(A + 0x18) - M.w(A + 0xe)) & 0xffff);
    M.ww(A + 0x2a, (-M.w(A + 0x12) - M.w(A + 0x14)) & 0xffff);
    M.ww(A + 0x2c, (M.w(A + 0x1a) + M.w(A + 0xc)) & 0xffff);
    M.ww(A + 0x2e, (M.w(A + 0x10) - M.w(A + 0x16)) & 0xffff);
    M.ww(A + 0x30, (-M.w(A + 0x1c)) & 0xffff);
    M.ww(A + 0x24, (-M.w(A + 0x20)) & 0xffff);
  }

  /* ------------------------------------------------------------ $4E88E : élément de matrice × valeur (Q15) */
  function mat(M, idx, value) { return q15(value, M.sw(V.matrix + 2 * idx)); }

  /* ------------------------------------------------------------ $4EB30 : gravité dans le repère voiture */
  function gravity(M, K) {
    M.ww(0x10afa, mat(M, 0xf, s16(-K.gravity)) & 0xffff);
    M.ww(0x10afc, mat(M, 0x4, s16(-K.gravity)) & 0xffff);
    M.ww(0x10af8, mat(M, 0xe, K.gravity) & 0xffff);
  }

  /* ------------------------------------------------------------ produits matrice × vecteur
     table d'indices $13334 : 3 lignes de 3 indices (+ variantes)                         */
  function idx(M, o) { return M.b(0x13334 + o); }
  /* $4EAD6 : vitesse monde -> repère voiture ($10B16/18/1A) */
  function localVelocity(M) {
    for (var r = 2; r >= 0; r -= 2) {                      // lignes 2 et 0 seulement (subq #2)
      var s = (mat(M, idx(M, r), M.sw(0x10ad4)) + mat(M, idx(M, 3 + r), M.sw(0x10ad6)) + mat(M, idx(M, 6 + r), M.sw(0x10ad8))) & 0xffff;
      M.ww(0x10b16 + 2 * r, s);
    }
  }
  /* $4EB62 : forces locales ($10B1C/1E/20) -> accélérations monde ($10AE0/2/4) */
  function worldAcceleration(M) {
    for (var r = 2; r >= 0; r--) {
      var s = (mat(M, idx(M, 9 + r), M.sw(0x10b1c)) + mat(M, idx(M, 12 + r), M.sw(0x10b1e)) + mat(M, idx(M, 15 + r), M.sw(0x10b20))) & 0xffff;
      M.ww(0x10ae0 + 2 * r, s);
    }
  }
  /* $4EBBC : vitesses angulaires -> dérivées des angles ($10B24/26/28) */
  function angleRates(M) {
    for (var r = 1; r >= 0; r--) {
      var s = (mat(M, idx(M, 0x12 + r), M.sw(0x10ada)) + mat(M, idx(M, 0x14 + r), M.sw(0x10adc))) & 0xffff;
      M.ww(0x10b24 + 2 * r, s);
    }
    M.ww(0x10b28, (mat(M, 4, M.sw(0x10b26)) + M.sw(0x10ade)) & 0xffff);
  }

  /* ------------------------------------------------------------ $4F130 / $4F17A : v += a·Δt, ω += α·Δt */
  function integrateVelocity(M, K) {
    for (var i = 0; i < 3; i++) {
      M.ww(0x10ad4 + 2 * i, (M.w(0x10ad4 + 2 * i) + mulAsr8(M.sw(0x10ae0 + 2 * i), K.dt)) & 0xffff);
    }
  }
  function integrateAngularVelocity(M, K) {
    for (var i = 0; i < 3; i++) {
      M.ww(0x10ada + 2 * i, (M.w(0x10ada + 2 * i) + mulAsr8(M.sw(0x10ae6 + 2 * i), K.dt)) & 0xffff);
    }
  }

  /* ------------------------------------------------------------ $4EFA4 : position et angles */
  function integratePosition(M, K) {
    var shifts = [64, 128, 64];                            // x,z : 2^6 ; y : 2^7
    for (var i = 0; i < 3; i++) {
      var dv = s16(mulAsr8(M.sw(0x10ad4 + 2 * i), K.dt));
      M.wl(0x10ac2 + 4 * i, (M.l(0x10ac2 + 4 * i) + dv * shifts[i]) | 0);
    }
    if (M.sw(0x10ac6) >= 1000) M.ww(0x10ac6, 1000);        // plafond d'altitude
    for (var j = 0; j < 3; j++) {
      M.ww(0x10ace + 2 * j, (M.w(0x10ace + 2 * j) + mulAsr8(M.sw(0x10b24 + 2 * j), K.dt)) & 0xffff);
    }
    // tangage et roulis bornés (table $4F128) ; la vitesse angulaire s'annule en butée
    var k = (M.sb(0x1095f) < 0 && M.b(0x10984) === 0xe0) ? 1 : 0;
    var hi = K.limits[k], lo = K.limits[2 + k];
    function clamp(angle, rate) {
      var a = M.w(angle), lim;
      if (a & 0x8000) { lim = lo; if (lim < a) return; } else { lim = hi; if (lim >= a) return; }
      M.ww(angle, lim);
      if (((lim ^ M.w(rate)) & 0x8000) === 0) M.ww(rate, 0);
    }
    clamp(0x10ace, 0x10ada);
    clamp(0x10ad2, 0x10ade);
    var f = M.b(0x10995) & 0x7f, hp = M.sb(0x10ace);
    if (hp < 0) hp = (((-hp) << 24) >> 24);
    if (hp >= 15) f |= 0x80;
    M.wb(0x10995, f);
    M.ww(0x10a2c, (-M.w(0x10ace)) & 0xffff);
  }


  /* ------------------------------------------------------------ $4FB7E : générateur pseudo-aléatoire */
  function random(M) {
    var d0 = M.w(0x4fbae) >>> 4, d3 = M.w(0x4fbac) >>> 1;
    d0 = (d0 & 0xff00) | ((d0 ^ d3) & 0xff);
    M.wl(0x4fbac, ((M.l(0x4fbac) << 8) | M.b(0x4fbb0)) | 0);
    M.wb(0x4fbb0, d0 & 0xff);
    return d0;
  }

  /* ------------------------------------------------------------ $4ED14 : effet sonore n (mélangeur $4EDD0) */
  function sound(M, K, n) {
    var t = 0x4edd2 + (n & 7) * 8, d4 = M.b(t) & 3;
    var m = M.b(0x4edd0) | (1 << d4) | (1 << (d4 + 3));
    M.wb(0x4edd0, m & M.b(t + 7));
    if (K.onSound) K.onSound(n);
  }

  /* ------------------------------------------------------------ $4EE62 : ressort + amortisseur */
  function spring(K, delta, compression) { return s16(mulAsr8(delta, K.damping) + compression); }

  /* ------------------------------------------------------------ $4FA2E / $4E7EC : partage d'adhérence */
  function gripShare(M, v) {
    v = s16(v); if (v < 0) v = s16(-v);
    var d1 = v < 0x100 ? v & 0xff : 0xff;
    M.wb(0x10915, d1);
    M.wb(0x10917, M.b(0x135b8 + (d1 >>> 1)));        // table √(1−x²)
  }
  function scaleByte(M, d0) {                         // ($10904 × d0) >> 8
    var p = (d0 & 0xff) * M.b(0x10904);
    M.wb(0x10905, p & 0xff);
    return (p >>> 8) & 0xff;
  }

  /* ------------------------------------------------------------ $4F8E6 : répartition des forces de contact */
  function contactForces(M) {
    M.ww(0x10b34, 0);
    var d0 = ((((M.l(0x10a9a) + M.l(0x10a9e)) | 0) >> 1) - M.l(0x10aa2)) | 0;
    d0 = d0 >> 4;
    M.ww(0x10b36, (d0 ^ 0x8000) & 0xffff);
    gripShare(M, d0);
    M.wb(0x10916, M.b(0x10917)); M.wb(0x10b3c, M.b(0x10915));
    d0 = ((M.l(0x10a9a) - M.l(0x10a9e)) | 0) >> 3;
    M.ww(0x10b32, d0 & 0xffff);
    gripShare(M, d0);
    M.wb(0x10904, M.b(0x10916));
    M.wb(0x10b3a, scaleByte(M, M.b(0x10917)));
    M.wb(0x10b38, scaleByte(M, M.b(0x10915)));
    function force(mag, signByte, dest) {
      M.wb(0x10904, M.b(mag)); M.wb(0x109a5, M.b(signByte));
      var d3 = M.b(0x10904); if (M.sb(0x109a5) < 0) d3 = -d3;
      d3 = s16(d3 << 7);
      M.ww(dest, q15(M.sw(0x10b22), d3) & 0xffff);
    }
    force(0x10b38, 0x10b32, 0x10b2a);
    force(0x10b3a, 0x10b34, 0x10b2c);
    force(0x10b3c, 0x10b36, 0x10b2e);
  }

  /* ------------------------------------------------------------ $50C34 : impulsion de collision (adversaire) */
  function collisionImpulse(M, K) {
    if (!M.b(0x10930)) return;
    M.wb(0x10930, 0);
    var d0 = s16(M.w(0x109d8) - M.w(0x10b42)); if (d0 < 0) d0 = 0;
    M.ww(0x109d8, d0 & 0xffff);
    d0 = M.sw(0x10b40) >> 4;
    [0x10b60, 0x10b62, 0x10b64].forEach(function (a) { M.ww(a, (M.w(a) - d0) & 0xffff); });
    M.ww(0x10b2a, (M.w(0x10b2a) + M.w(0x10b3e)) & 0xffff);
    M.ww(0x10b2c, (M.w(0x10b2c) + M.w(0x10b40)) & 0xffff);
    M.ww(0x10b2e, (M.w(0x10b2e) + M.w(0x10b42)) & 0xffff);
    M.ww(0x10b3e, 0); M.ww(0x10b40, 0); M.ww(0x10b42, 0);
    sound(M, K, 2);
  }

  /* ------------------------------------------------------------ grue ($48878 et dépendances) */
  function linkToggle(M) {                            // $45D9E (jeu à deux par câble)
    if (!M.b(0x4537a) || M.sb(0x109ae) >= 0 || M.sb(0x109ca) >= 0) return;
    var d0 = (M.b(0x109ca) << 1) & 0xff;
    if (M.b(0x10906) !== M.b(0x10907)) return;
    if (((d0 ^ M.b(0x109cb)) & 0x80) === 0) M.wb(0x109cb, M.b(0x109cb) ^ 0x80);
  }
  function craneLift(M, d0) {                         // $489BC : tire la voiture vers le haut
    var d3 = (M.w(0x10aba) - M.w(0x10a4a) - ((d0 << 8) & 0xffff)) & 0xffff;
    var t = ((s16(d3) >> 3) - 0x100) & 0xffff;
    if ((t & 0x8000) && t < 0xfe00) t = 0xfe00;
    M.ww(0x10b2c, (M.w(0x10b2c) - t) & 0xffff);
    return ((d3 >>> 8) + 2) & 0xff;
  }
  function craneSwing(M, K, d0) {                     // $489F2 : balancement (impose le roulis)
    var d4 = 0x10;
    if (M.sb(0x109cb) < 0) { d0 = (-d0) & 0xff; d4 = 0xf0; }
    var step = mulAsr8(s16((d0 << 8) & 0xffff), K.dt);
    var d3 = (M.w(0x10a46) << 5) & 0xffff;
    if (M.b(0x109ea) !== d4) M.ww(0x109ea, (M.w(0x109ea) + step) & 0xffff);
    M.ww(0x10ad2, (M.w(0x109ea) - d3) & 0xffff);
    M.ww(0x10b10, 0);
    return M.b(0x109ea) === d4;
  }
  function crane(M, K) {
    var st = M.b(0x109c9);
    if (st === 0) return;
    if (st >= 0xe6) {                                 // la grue soulève
      linkToggle(M);
      M.wb(0x109ea, M.sb(0x109cb) < 0 ? 0xd4 : 0x2c); M.wb(0x109eb, 0);
      M.wb(0x109c9, st - 1); return;
    }
    if (st === 0xe5) {
      craneSwing(M, K, 0);
      if (!(craneLift(M, 3) & 0x80)) M.wb(0x109c9, st - 1);
      return;
    }
    if (st === 0xe4) {
      craneLift(M, 4);
      if (!craneSwing(M, K, 0xff)) return;
      var d0 = ((random(M) & 0x1f) + 0xa0) & 0xff;
      var d2 = M.sb(0x109ae) < 0 ? 0x3c : 0x2c;
      if (M.sb(0x1111c) >= 0) d0 = 0x8c;
      M.wb(0x109c9, d0);
      if (M.b(0x1095e)) M.wb(0x1095e, 0x32);
      M.wb(0x10978, 0x80); M.wb(0x4bbdf, d2); M.wb(0x4bbde, 4);   // $4BA70 : message n°4
      return;
    }
    craneSwing(M, K, 0); craneLift(M, 2);
    if (M.sb(0x109b7) >= 0) {
      M.wb(0x109c9, M.b(0x109c9) - 1);
      if (M.b(0x109c9) === 0) M.wb(0x109c9, 1);
    }
    if (M.b(0x109ae)) { if (M.b(0x1095a)) return; }
    else if (M.sb(0x109c9) < 0) return;
    M.wb(0x109c9, 0); M.wb(0x10986, 0); M.wb(0x10978, 0); M.wb(0x109ae, 0x80);
  }

  /* ------------------------------------------------------------ $4F220 : suspension (3 roues) */
  function suspension(M, K) {
    M.wb(0x10967, 0); M.wb(0x10a24, 0);
    for (var i = 0; i < 3; i++) {
      var c = (M.l(0x10a8e + 4 * i) - M.l(0x10a7e + 4 * i) - M.l(0x10a8a)) | 0;   // compression
      M.wl(0x10a9a + 4 * i, c);
      if (c < 0) { if (c < -0x300) c = -0x300; } else if (c >= 0x1400) c = 0x1400;
      M.ww(0x10afe + 2 * i, c & 0xffff);
      var f = spring(K, s16(c - M.w(0x10b04 + 2 * i)), s16(c));
      var force = 0x10b0a + 2 * i;
      if (f < 0) { M.ww(force, 0); M.wb(0x10940, 0); }
      else {
        var old = M.sw(force);
        M.ww(force, f & 0xffff);
        if (f >= 0x400 && old < 0x200) M.wb(0x10967, M.b(0x10967) + 1);      // choc : bruit
        var d0 = s16(M.w(force) - (M.b(0x108eb) << 8));
        if (d0 < 0x700) M.wb(0x10940, 0);
        else {                                                               // dégâts
          if ((d0 & 0xffff) >= M.w(0x10a24)) M.ww(0x10a24, d0 & 0xffff);
          d0 = (d0 - 0x600) & 0xffff;
          if (M.sb(0x109b7) >= 0) {
            M.wb(0x10940, M.b(0x10940) + 1);
            if (M.sb(0x10940) < M.sb(0x50ae8)) {
              var h = d0 >>> 8, add = (h + (h >>> 1)) & 0xff, sum = add + M.b(0x10939 + i);
              M.wb(0x10939 + i, sum > 0xff ? 0xff : sum);
              M.wb(0x1093e, 0x80);
            }
          }
          if (M.w(force) >= 0x1200) M.ww(force, 0x11ff);
        }
      }
      M.ww(0x10b04 + 2 * i, M.w(0x10afe + 2 * i));
    }
    var a = s16(M.w(0x10b0a) + M.w(0x10b0c)) >> 1;
    M.ww(0x109e0, a & 0xffff);
    M.ww(0x10b22, (s16(a + M.w(0x10b0e)) >> 1) & 0xffff);
    contactForces(M);
    var d = s16(M.w(0x10b0a) - M.w(0x10b0c)), t3 = s16(d * 3);
    var m = t3 < 0 ? s16(-t3) : t3; if (m >= 0x1000) m = 0x1000;
    M.ww(0x10b12, (d < 0 ? -m : m) & 0xffff);
    M.ww(0x10b10, (M.w(0x109e0) - M.w(0x10b0e)) & 0xffff);
    M.wb(0x10968, M.b(0x10b22) | M.b(0x10b23));
    if (!M.b(0x10968) && !M.b(0x109c9)) {             // en l'air : couple de tangage
      var d3 = 0xff80, ok = true, p = M.sw(0x10ace);
      if (p < 0) { var tr = M.b(0x1112d); if (tr === 4) d3 = 0xfff8; else if (tr !== 7) ok = false; }
      else if (p >= 0x1000) d3 = 0xff00;
      if (ok) {
        d3 = (d3 - M.w(0x10b10)) & 0xffff;
        if (d3 & 0x8000) { var b = M.sb(0x10ada); if (b >= 0 || b === -1) M.ww(0x10b10, d3); }
      }
    }
    crane(M, K);
    M.ww(0x10b30, M.w(0x10b2e));
    collisionImpulse(M, K);
    if (M.b(0x10967)) sound(M, K, 3);
  }


  /* ------------------------------------------------------------ circuit : paramètres de la pièce ($4D3D2) */
  function ptr6502(raw) { return 0x13670 + ((((raw & 0xff) << 8) | (raw >>> 8)) + 0x4f00 & 0xffff); }
  function pieceParams(M, d1) {
    var bba = M.b(0x10bba + d1);
    M.wb(0x10963, bba);
    M.ww(0x10a76, M.w(0x13690 + ((bba << 1) & 0xff)));
    var c1e = M.b(0x10c1e + d1);
    M.wb(0x109c6, (c1e >> 7) * 2);
    M.ww(0x10a7a, M.w(0x13690 + ((c1e << 1) & 0xff)));
    M.ww(0x109f8, M.w(0x10d4a + ((d1 << 1) & 0xff)));
    M.ww(0x109fa, M.w(0x10e12 + ((d1 << 1) & 0xff)));
    var ce6 = M.b(0x10ce6 + d1);
    M.wb(0x10a34, ce6 & 0xc0);
    M.wb(0x10a1c, ((ce6 & 0x10) << 3) & 0xff);
    var t = ce6 & 0xf;
    M.wb(0x10970, t);
    M.ww(0x10aa6, M.w(0x13670 + 2 * t));
    var a = ptr6502(M.w(0x10aa6));
    M.wb(0x10937, M.b(a + 1));
    var d2 = M.b(a), n = M.b(a + d2); d2++;
    M.wb(0x10981, n); M.wb(0x10943, (n << 1) & 0xff);
    M.wb(0x10982, (n - 2) & 0xff); M.wb(0x10944, ((n - 2) << 1) & 0xff);
    M.wb(0x10954, ((n >> 1) - 1) & 0xff);
    M.wb(0x10a2e, (M.b(a + d2) & 1) << 7); d2++;
    M.wb(0x10965, M.b(a + d2)); d2++;
    M.wb(0x109c3, M.b(a + d2)); d2 += 3;
    M.wb(0x109be, M.b(a + d2));
  }
  function nextPiece(M) { var d = (M.b(0x1096f) + 1) & 0xff; if (((d << 24) >> 24) >= M.sb(0x11114)) d = 0; M.wb(0x1096f, d); return d; }
  function prevPiece(M) { var d = (M.b(0x1096f) - 1) & 0xff; if (d & 0x80) d = (M.b(0x11114) - 1) & 0xff; M.wb(0x1096f, d); return d; }

  /* hauteur de la route à la section d1 (bord gauche si d1 pair, droit si impair), en y-jeu ($495F2) */
  function sectionHeight(M, A4, A5, d1) {
    var v, x;
    if (M.sb(0x10963) < 0) {
      var d2 = d1 & 0xfe, P = (d1 & 1) ? A5 : A4;
      v = (((M.b(P + d2) & 0x7f) << 8) | M.b(P + d2 + 1)) + M.w((d1 & 1) ? 0x109fa : 0x109f8);
    } else {
      var i = (d1 & 0xff) >>> 1;
      x = M.b(((d1 & 1) ? A5 : A4) + i);
      v = (((x & 0xf) << 8) | ((x << 1) & 0xe0)) + M.w((d1 & 1) ? 0x109fa : 0x109f8);
    }
    return s16(v) >> 5;
  }

  /* $499CC : la roue a franchi le bout de la pièce -> pièce suivante / précédente */
  function crossPiece(M) {
    var forward = ((M.b(0x10a2a) ^ M.b(0x10a1c)) & 0x80) === 0, toEnd;
    if (forward) { pieceParams(M, nextPiece(M)); toEnd = M.sb(0x10a1c) < 0; }
    else { pieceParams(M, prevPiece(M)); toEnd = M.sb(0x10a1c) >= 0; }
    if (toEnd) { M.wb(0x1098d, (M.b(0x10981) - 4) & 0xff); if (M.sb(0x10a2a) < 0) return; }
    else { M.wb(0x1098d, 0); if (M.sb(0x10a2a) >= 0) return; }
    var b = (-M.b(0x10a2b)) & 0xff; M.wb(0x10a2b, b || 0xff);
    b = (-M.b(0x10a37)) & 0xff; M.wb(0x10a37, b || 0xff);
  }

  /* $49A9C : interpolation bilinéaire de la hauteur sous la roue -> $10902 (y 24.8) */
  function interpolate(M) {
    var t = M.b(0x10a37);
    var d5 = (s16(M.w(0x109ee) - M.w(0x109ec)) * t + (M.sw(0x109ec) << 8)) | 0;
    var d0 = (s16(M.w(0x109f2) - M.w(0x109f0)) * t + (M.sw(0x109f0) << 8)) | 0;
    var u = M.b(0x10a2b);
    d0 = (d0 - d5) | 0;
    var big = Math.abs(d0) >= 0x8000;
    if (big) d0 = d0 >> 3;
    var lw = d0 & 0xffff, r;
    if (lw & 0x8000) { lw = (-lw) & 0xffff; r = -(((lw * u) >>> 0) & 0xffffff00); }
    else r = (lw * u) >>> 0;
    r = r | 0;
    if (big) r = (r << 3) | 0;
    M.wl(0x10902, ((r >> 8) + d5) | 0);
  }

  /* $49D50 : roue au bord de la route */
  function roadEdge(M) {
    var d0 = M.w(0x10a0c);
    if (!(d0 & 0x8000)) { d0 = (0x180 - d0) & 0xffff; if (d0 & 0x8000) d0 = (-d0) & 0xffff; }
    else d0 = (-d0) & 0xffff;
    var d3 = (M.l(0x10902) - ((d0 & 0xff) << 4) - 0x100) | 0;
    if (s16(d0) > 0x30 || d3 < 0x1000) {            // roue dans le vide : le sol se dérobe
      M.wl(0x10902, 0x1000);
      M.wb(0x10984, (M.b(0x10984) >>> 1) | 0x80);
      return;
    }
    M.wl(0x10902, d3);
    var f = (M.b(0x10a1c) ^ M.b(0x10a0c)) & 0x80;
    M.wb(0x109c4, f || 0x40);
  }

  /* $49B3A : hauteur du sol sous la roue (lissée si la voiture est presque à plat) */
  function groundHeight(M, d1) {
    interpolate(M);
    var off = (d1 << 1) & 0xff, a = 0x10a8e + off;
    var f = M.b(0x1094f); M.wb(0x1094f, f & 0x7f);
    if (f & 0x80) roadEdge(M);
    if (M.sb(0x10b46) >= 10) { M.wl(a, M.l(0x10902)); return; }
    var p = M.sb(0x10ace); if (p < 0) p = (((-p) << 24) >> 24);
    if (p > 5) { M.wl(a, M.l(0x10902)); return; }
    var sum = (M.l(a) >>> 0) + (M.l(0x10902) >>> 0);
    M.wl(a, Math.floor(sum / 2) | 0);
  }

  /* $49718 : position des trois roues sur la route (pièce, section, hauteur) */
  function wheelContacts(M) {
    var d1 = M.b(0x10906); M.wb(0x1096f, d1); pieceParams(M, d1);
    M.wb(0x10984, 0);
    for (var w = 4; w >= 0; w -= 2) {
      M.wb(0x109e3, w);
      if (M.b(0x10906) !== M.b(0x1096f)) { M.wb(0x1096f, M.b(0x10906)); pieceParams(M, M.b(0x10906)); }
      M.wb(0x10904, M.b(0x10965));
      var d0 = (s16(M.w(0x10aec + w)) >> 4) + M.w(0x10a48) & 0xffff, b;
      if (d0 >= 0x180) {                               // hors de la largeur de route
        M.wb(0x1094f, M.b(0x1094f) | 0x80); M.ww(0x10a0c, d0);
        b = (d0 & 0x8000) ? 0 : 0xff;
      } else {
        var q = q15(d0, (M.b(0x10904) << 7) & 0x7fff);
        b = (q >= 0x100 ? M.b(0xff) : q) & 0xff;         // sic : l'original lit l'octet $FF (et non #$FF)
      }
      M.wb(0x10a37, b);
      if (M.sb(0x10a1c) < 0) b ^= 0xff;
      if (w === 4) M.wb(0x1098b, b);
      M.wb(0x10904, M.b(0x109c3));
      var d = q15(s16(M.w(0x10af2 + w)) >> 3, (M.b(0x10904) << 7) & 0x7fff);
      M.ww(0x10a2a, (d + M.w(0x108fa)) & 0xffff);
      var sec = (M.b(0x10a2a) << 1) & 0xff;
      M.wb(0x1098d, sec);
      if ((sec & 0x80) || ((sec << 24) >> 24) >= M.sb(0x10982)) crossPiece(M);
      var A4 = ptr6502(M.w(0x10a76)), A5 = ptr6502(M.w(0x10a7a)), k, h = [0x109ec, 0x109ee, 0x109f0, 0x109f2];
      if (M.sb(0x10a1c) >= 0) { k = M.b(0x1098d); for (var i = 0; i < 4; i++) M.ww(h[i], sectionHeight(M, A4, A5, (k + i) & 0xff) & 0xffff); }
      else { k = (M.b(0x10981) - M.b(0x1098d) - 4) & 0xff; for (var j = 0; j < 4; j++) M.ww(h[3 - j], sectionHeight(M, A4, A5, (k + j) & 0xff) & 0xffff); }
      groundHeight(M, w);
    }
  }

  /* $4EF22 : positions latérales/longitudinales des roues */
  function wheelOffsets(M) {
    var d4 = s16((M.sw(0x6ef1a) >> 1) - (M.sw(0x6ef10) >> 1)) >> 5;
    var d5 = s16((M.sw(0x6ef14) >> 1) - (M.sw(0x6ef1e) >> 1)) >> 5;
    var d0 = M.sw(0x6ef20) >> 5, d3 = M.sw(0x6ef22) >> 5;
    M.ww(0x10af0, -d0 & 0xffff); M.ww(0x10af6, -d3 & 0xffff);
    M.ww(0x10aec, (d0 - d4) & 0xffff); M.ww(0x10aee, (d0 + d4) & 0xffff);
    M.ww(0x10af2, (d3 - d5) & 0xffff); M.ww(0x10af4, (d3 + d5) & 0xffff);
  }

  /* $4F1C4 : hauteur des trois roues (caisse inclinée) */
  function wheelHeights(M, K) {
    var c = cos(M, K, M.w(0x10ace));
    M.ww(0x109e0, c & 0xffff);
    var r = cos(M, K, M.w(0x10ad2)) * 8, d3 = c * 16, y = M.l(0x10ac6);
    M.wl(0x10a86, ((y - d3) | 0) >> 8);
    var d4 = (y + d3) | 0;
    M.wl(0x10a82, ((d4 - r) | 0) >> 8);
    M.wl(0x10a7e, ((d4 + r) | 0) >> 8);
  }

  /* $4E508 : vitesse latérale absolue ; décroissance du compteur $10A4C en l'air */
  function lateralSpeed(M) {
    var v = M.sw(0x10b1a); if (v < 0) v = s16(-v);
    M.ww(0x10b46, v & 0xffff);
    if (!M.b(0x10968)) { M.ww(0x10a4c, (M.w(0x10a4c) - (M.w(0x10a4c) >>> 2)) & 0xffff); return; }
    // au sol : niveau du crissement des pneus
    if (s16(v) < 0x800) M.ww(0x10a4c, (v << 3) & 0xffff);
    else { var t = ((v << 1) & 0xffff) + 0x3000; M.ww(0x10a4c, t > 0xffff ? 0xff00 : t); }
  }

  /* limite d'adhérence = 2 × charge, nulle en l'air ($4F7E4) */
  var gripScale = 1;                                  // réglage de la réplique (1 = original)
  function gripLimit(M) {
    if (!M.b(0x10968)) return 0;
    var g = (M.w(0x10b2c) << 1) & 0xffff;
    return gripScale === 1 ? g : Math.min(0xffff, Math.round(g * gripScale));
  }

  /* $4F6C2 / $4F784 : forces de propulsion et latérale, bornées par l'adhérence */
  function tyreForces(M) {
    M.ww(0x10b1e, (M.w(0x10afa) + M.w(0x10b2c)) & 0xffff);
    var hb = M.b(0x10b14) | M.b(0x10b1a);
    if (!(hb & 0x80) && M.b(0x10b15)) M.ww(0x10b14, (M.w(0x10b14) - hb) & 0xffff);   // résistance
    var t = M.sw(0x10b14); t = t < 0 ? (-t) & 0xffff : t;
    var lim = gripLimit(M);
    if (t >= lim) M.ww(0x10b14, (M.sb(0x10b14) < 0 ? -lim : lim) & 0xffff);         // patinage
    M.ww(0x10b20, (M.w(0x10b14) + M.w(0x10b2e) + M.w(0x10afc)) & 0xffff);
    var d4 = (M.w(0x10af8) + M.w(0x10b2a)) & 0xffff, d3 = s16(d4 - M.w(0x10b16));
    d3 = d3 < 0 ? (-d3) & 0xffff : d3;
    lim = gripLimit(M);
    if (d3 < lim) { M.ww(0x10b1c, (M.w(0x10b2a) - M.w(0x10b16)) & 0xffff); M.wb(0x109ab, 0); }
    else { M.ww(0x10b1c, (d4 - (M.sb(0x10b16) < 0 ? -lim : lim)) & 0xffff); M.wb(0x109ab, 0x80); }   // dérapage
  }

  /* $4F742 : couples de tangage et de roulis */
  function torques(M) {
    var d0 = (M.w(0x10b10) - (M.sw(0x10ada) >> 4)) & 0xffff;
    if (M.b(0x10968)) d0 = (d0 + (M.sw(0x10b20) >> 2)) & 0xffff;
    M.ww(0x10ae6, d0);
    M.ww(0x10aea, (M.w(0x10b12) - (M.sw(0x10ade) >> 4)) & 0xffff);
  }

  /* $4F7FE : frottement (proportionnel à la vitesse) */
  function friction(M) {
    var d7 = 1, d0;
    var heavy = false;
    if (M.b(0x10968)) {
      var b = M.b(0x10b30); if (b & 0x80) b ^= 0xff;
      if (((b << 24) >> 24) >= 3 || M.sb(0x10986) < 0) heavy = true;
      else if (M.b(0x10a8c)) { d7 = 3; heavy = true; }
      else if (M.b(0x109c9)) { d7 = 3; heavy = true; }
    } else if (M.b(0x109c9)) { d7 = 3; heavy = true; }
    if (heavy) d0 = 0x6000;
    else {
      d0 = 0;
      [0x10b16, 0x10b18, 0x10b1a].forEach(function (a) { var v = M.sw(a); v = v < 0 ? s16(-v) : v; if (v > d0 || d0 === 0 && v < 0) d0 = Math.max(d0, v); });
      d0 = Math.max(abs16(M.sw(0x10b16)), abs16(M.sw(0x10b18)), abs16(M.sw(0x10b1a)));
      d7 = 5;
      if (M.sb(0x109b1) < 0 && M.sb(0x109a2) >= 0) { d0 = d0 - 0xa00; if (d0 < 0) d0 = 0; }
    }
    for (var i = 0; i < 3; i++) {
      var p = (M.sw(0x10ad4 + 2 * i) * s16(d0)) >> 16;
      M.ww(0x10ae0 + 2 * i, (M.w(0x10ae0 + 2 * i) - (s16(p) >> d7)) & 0xffff);
    }
  }
  function abs16(v) { return v < 0 ? s16(-v) : v; }

  /* $4E55C : direction (rotation imposée + couple de lacet) */
  function steering(M) {
    var d1 = M.b(0x10906); M.wb(0x1096f, d1); pieceParams(M, d1);
    var d3 = M.w(0x10a1c), d4 = ((M.w(0x10b44) - M.w(0x10ad0)) ^ d3) & 0xffff, d2 = 0;
    if (M.sb(0x10937) < 0) { d2 = 2; if ((M.w(0x10a2e) ^ d3) & 0x8000) d2 = 4; }
    d4 = (d4 + M.w(0x4e7a4 + d2)) & 0xffff;
    var d0 = s16(d4) < 0 ? (-d4) & 0xffff : d4;
    M.ww(0x10a14, d0); M.ww(0x109e0, d4);
    M.ww(0x10a26, d0 >= 0x800 ? 0x7fff : (d0 << 4) & 0xffff);
    if ((((M.b(0x10954) - M.b(0x108f4)) & 0xff) >>> 0) < 2) pieceParams(M, nextPiece(M));
    M.wb(0x10947, M.b(0x10a2e) ^ M.b(0x10a1c));
    var inp = M.b(0x109b0), v, path;
    if (inp) {
      M.wb(0x10905, inp ^ M.b(0x109e0));
      if (M.sb(0x10937) < 0) {
        if (((M.b(0x109b0) ^ M.b(0x10947)) & 0x80) === 0) { v = (M.b(0x109be) + 0x2d) & 0xff; if (M.sb(0x10905) >= 0) v = (v + M.b(0x10a26)) & 0xff; }
        else { M.wb(0x109b0, M.b(0x10947)); v = (M.b(0x109be) - 0x23) & 0xff; }
      } else { v = M.b(0x109be); if (M.sb(0x10905) >= 0) v = (v + M.b(0x10a26)) & 0xff; }
      path = 'turn';
    } else {
      d4 = 0;
      if (M.sb(0x10937) < 0) { M.wb(0x109b0, M.b(0x10947)); v = M.b(0x109be); path = 'turn'; }
      else path = 'align';
    }
    if (path === 'turn') {                            // $4E766
      M.wb(0x10904, v);
      var q = q15(M.sw(0x10b1a), (v << 7) & 0x7fff);
      if (M.sb(0x109b0) < 0) q = s16(-q);
      d4 = (s16(q) >> 3) & 0xffff;
      path = M.b(0x10a14) < 0x1e ? 'torque' : 'align';
    }
    if (path === 'align') {                           // $4E6AA : la voiture s'aligne sur la route
      M.wb(0x109b0, M.b(0x109e0));
      var a = M.w(0x10a14), d2b = a & 0xff, rot, done = false;
      if (M.b(0x10a14)) { a = (a - 0x1e00) & 0xffff; if (!(a & 0x8000)) { rot = a; done = true; } else d2b = 0xff; }
      if (!done) {
        M.wb(0x10904, d2b);
        var s = abs16(M.sw(0x10b1a)); s = (s + 0xa00) & 0xffff; if (s & 0x8000) s = 0x7f00;
        rot = (q15(s16(s), (M.b(0x10904) << 7) & 0x7fff) & 0xffff) >>> 7;
        if ((rot & 0xff) === 0) rot = (rot & 0xff00) | ((rot + 1) & 0xff);
      }
      if (M.sb(0x109e0) < 0) rot = (-rot) & 0xffff;
      M.ww(0x10ad0, (M.w(0x10ad0) + rot) & 0xffff);
    }
    d4 = (d4 - M.w(0x10adc)) & 0xffff;               // $4E71E
    if ((M.l(0x51894) >>> 0) !== 0xc0be145f || !M.b(0x10968)) d4 = 0;   // contrôle anti-piratage
    M.ww(0x10ae8, d4);
  }

  /* $4EEB0 : un pas complet de la physique de la voiture */
  function physicsStep(M, K) {
    gripScale = K.grip === undefined ? 1 : K.grip;
    buildMatrix(M, K); wheelOffsets(M); wheelContacts(M); wheelHeights(M, K);
    localVelocity(M); lateralSpeed(M); gravity(M, K); suspension(M, K);
    if (M.b(0x1095c)) {
      tyreForces(M); steering(M); worldAcceleration(M); friction(M); torques(M);
      integrateAngularVelocity(M, K); angleRates(M);
    }
    integrateVelocity(M, K); integratePosition(M, K);
  }

  /* routines portées, par adresse d'origine */
  var ROUTINES = {
    0x4e8b2: buildMatrix, 0x4eb30: gravity, 0x4ead6: localVelocity, 0x4eb62: worldAcceleration,
    0x4ebbc: angleRates, 0x4f130: integrateVelocity, 0x4f17a: integrateAngularVelocity,
    0x4efa4: integratePosition, 0x4f220: suspension, 0x4f8e6: contactForces, 0x48878: crane,
    0x50c34: collisionImpulse, 0x49718: wheelContacts,
    0x4ef22: wheelOffsets, 0x4f1c4: wheelHeights, 0x4e508: lateralSpeed, 0x4f6c2: tyreForces, 0x4f742: torques,
    0x4f7fe: friction, 0x4e55c: steering, 0x4eeb0: physicsStep
  };

  return { Mem: Mem, V: V, constants: constants, ROUTINES: ROUTINES, cos: cos, sin: sin, physicsStep: physicsStep };
})();
