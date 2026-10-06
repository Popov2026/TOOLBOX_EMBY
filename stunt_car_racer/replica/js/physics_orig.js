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

  /* routines portées, par adresse d'origine */
  var ROUTINES = {
    0x4e8b2: buildMatrix, 0x4eb30: gravity, 0x4ead6: localVelocity, 0x4eb62: worldAcceleration,
    0x4ebbc: angleRates, 0x4f130: integrateVelocity, 0x4f17a: integrateAngularVelocity,
    0x4efa4: integratePosition, 0x4f220: suspension, 0x4f8e6: contactForces, 0x48878: crane,
    0x50c34: collisionImpulse
  };

  return { Mem: Mem, V: V, constants: constants, ROUTINES: ROUTINES, cos: cos, sin: sin };
})();
