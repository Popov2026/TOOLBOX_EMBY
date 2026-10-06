/*
 * track.js — modèle de circuit : sections transversales (bord gauche / bord droit),
 * requêtes de surface (hauteur, normale), progression le long du tracé.
 * Unités : unités du jeu original (une case de grille = 0x800 = 2048 unités).
 */
var SCR = window.SCR || (window.SCR = {});

SCR.Track = (function () {
  'use strict';

  function Track(def, params) {
    this.name = def.name;
    this.vscale = params.track.verticalScale;
    var secs = [];
    def.pieces.forEach(function (p, pi) {
      p.sections.forEach(function (s, k) {
        if (pi > 0 && k === 0) return;           // section partagée avec la pièce précédente
        secs.push({ L: [s[0], s[1], s[2]], R: [s[3], s[4], s[5]], flag: s[6] || 0, piece: pi });
      });
    });
    // circuit fermé : la dernière section coïncide avec la première
    var a = secs[0], b = secs[secs.length - 1];
    if (Math.abs(a.L[0] - b.L[0]) + Math.abs(a.L[2] - b.L[2]) < 64) secs.pop();
    // la ligne de départ est sur la pièce header[2] (variable $11116 du jeu) : on fait
    // commencer le tableau des sections à cet endroit
    if (def.header && def.header[2] < def.pieces.length) {
      var sp = def.header[2], first = 0;
      while (first < secs.length && secs[first].piece !== sp) first++;
      if (first < secs.length) secs = secs.slice(first).concat(secs.slice(0, first));
    }
    var vs = this.vscale, y0 = Infinity;
    secs.forEach(function (s) { y0 = Math.min(y0, s.L[1], s.R[1]); });
    this.groundY = 0;
    this.baseY = y0;
    secs.forEach(function (s) {
      s.L[1] = (s.L[1] - y0) * vs + params.track.baseHeight;
      s.R[1] = (s.R[1] - y0) * vs + params.track.baseHeight;
      s.C = [(s.L[0] + s.R[0]) / 2, (s.L[1] + s.R[1]) / 2, (s.L[2] + s.R[2]) / 2];
    });
    this.secs = secs;
    this.n = secs.length;
    // longueurs cumulées le long de l'axe
    var cum = [0], total = 0;
    for (var i = 0; i < this.n; i++) {
      var c0 = secs[i].C, c1 = secs[(i + 1) % this.n].C;
      total += Math.hypot(c1[0] - c0[0], c1[2] - c0[2]);
      cum.push(total);
    }
    this.cum = cum;
    this.length = total;
    // boîte englobante
    var mnx = Infinity, mxx = -Infinity, mnz = Infinity, mxz = -Infinity, mxy = 0;
    secs.forEach(function (s) {
      [s.L, s.R].forEach(function (p) {
        mnx = Math.min(mnx, p[0]); mxx = Math.max(mxx, p[0]);
        mnz = Math.min(mnz, p[2]); mxz = Math.max(mxz, p[2]); mxy = Math.max(mxy, p[1]);
      });
    });
    this.bounds = { minX: mnx, maxX: mxx, minZ: mnz, maxZ: mxz, maxY: mxy };
  }

  Track.prototype.sec = function (i) { return this.secs[((i % this.n) + this.n) % this.n]; };

  /* test point / triangle dans le plan XZ, renvoie les coordonnées barycentriques */
  function bary(px, pz, a, b, c) {
    var v0x = b[0] - a[0], v0z = b[2] - a[2], v1x = c[0] - a[0], v1z = c[2] - a[2];
    var v2x = px - a[0], v2z = pz - a[2];
    var den = v0x * v1z - v1x * v0z;
    if (Math.abs(den) < 1e-9) return null;
    var u = (v2x * v1z - v1x * v2z) / den, v = (v0x * v2z - v2x * v0z) / den;
    return [1 - u - v, u, v];
  }

  /*
   * Surface du circuit sous (x, z). `hint` = segment probable (le précédent).
   * Renvoie { seg, y, nx, ny, nz, lat (-1 gauche .. +1 droite), along (0..1) } ou null.
   * Si plusieurs étages se superposent (croisements), garde la surface la plus proche
   * en dessous de `yRef`.
   */
  Track.prototype.surface = function (x, z, hint, yRef, range) {
    range = range || 12;
    var best = null, bestScore = Infinity;
    for (var d = -range; d <= range; d++) {
      var i = ((hint + d) % this.n + this.n) % this.n;
      var s0 = this.secs[i], s1 = this.secs[(i + 1) % this.n];
      var tris = [[s0.L, s0.R, s1.R, 0], [s0.L, s1.R, s1.L, 1]];
      for (var t = 0; t < 2; t++) {
        var T = tris[t], w = bary(x, z, T[0], T[1], T[2]);
        if (!w || w[0] < -1e-6 || w[1] < -1e-6 || w[2] < -1e-6) continue;
        var y = w[0] * T[0][1] + w[1] * T[1][1] + w[2] * T[2][1];
        var score = (yRef === undefined) ? Math.abs(d) : Math.abs(yRef - y) + Math.abs(d) * 2 + (y > yRef + 120 ? 1e5 : 0);
        if (score < bestScore) {
          bestScore = score;
          // normale du triangle
          var ax = T[1][0] - T[0][0], ay = T[1][1] - T[0][1], az = T[1][2] - T[0][2];
          var bx = T[2][0] - T[0][0], by = T[2][1] - T[0][1], bz = T[2][2] - T[0][2];
          var nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
          if (ny < 0) { nx = -nx; ny = -ny; nz = -nz; }
          var nl = Math.hypot(nx, ny, nz) || 1;
          // position latérale / longitudinale (approx. bilinéaire)
          var lat, along;
          if (t === 0) { along = w[2]; lat = w[1] + w[2]; } else { along = w[1] + w[2]; lat = w[1]; }
          best = { seg: i, y: y, nx: nx / nl, ny: ny / nl, nz: nz / nl, lat: lat * 2 - 1, along: along };
        }
      }
    }
    return best;
  };

  /* position le long du circuit (distance depuis la ligne de départ) */
  Track.prototype.progress = function (seg, along) {
    var i = ((seg % this.n) + this.n) % this.n;
    return this.cum[i] + (this.cum[i + 1] - this.cum[i]) * (along || 0);
  };

  /* point de l'axe + direction à la distance s */
  Track.prototype.pointAt = function (s) {
    s = ((s % this.length) + this.length) % this.length;
    var lo = 0, hi = this.n;
    while (hi - lo > 1) { var mid = (lo + hi) >> 1; if (this.cum[mid] <= s) lo = mid; else hi = mid; }
    var a = this.secs[lo], b = this.secs[(lo + 1) % this.n];
    var f = (s - this.cum[lo]) / Math.max(1e-6, this.cum[lo + 1] - this.cum[lo]);
    var C = [a.C[0] + (b.C[0] - a.C[0]) * f, a.C[1] + (b.C[1] - a.C[1]) * f, a.C[2] + (b.C[2] - a.C[2]) * f];
    var dx = b.C[0] - a.C[0], dz = b.C[2] - a.C[2];
    return { seg: lo, along: f, pos: C, heading: Math.atan2(dx, dz),
             width: Math.hypot(a.R[0] - a.L[0], a.R[2] - a.L[2]) };
  };

  /* Construit un circuit « maison » à partir d'une liste de commandes (circuits personnalisés). */
  Track.fromCommands = function (name, cmds, opts) {
    opts = opts || {};
    var width = opts.width || 384, step = opts.step || 256;
    var x = opts.x || 0, z = opts.z || 0, h = opts.h || 1280, head = 0;   // head : 0 = +Z
    var bank = 0, secs = [];
    function push(flag) {
      var cx = Math.cos(head), sx = Math.sin(head);
      // gauche = -X local
      var lx = x - cx * width / 2, lz = z + sx * width / 2, rx = x + cx * width / 2, rz = z - sx * width / 2;
      secs.push([lx, h + bank, lz, rx, h - bank, rz, flag || 0]);
    }
    push();
    cmds.forEach(function (c) {
      var n = Math.max(1, Math.round((c.len || 2048) / step));
      var turn = (c.turn || 0) * Math.PI / 180 / n, rise = (c.rise || 0) / n, bank1 = (c.bank || 0);
      var bank0 = bank;
      for (var i = 1; i <= n; i++) {
        head += turn;
        x += Math.sin(head) * step; z += Math.cos(head) * step;
        var f = i / n;
        h += c.curve === 'smooth' ? rise * 3 * f * (1 - f) * 2 : rise;
        bank = bank0 + (bank1 - bank0) * f;
        if (c.gap && i === n) { push(1); h += c.gap; }
        push();
      }
    });
    return { name: name, pieces: [{ sections: secs }] };
  };

  return Track;
})();
