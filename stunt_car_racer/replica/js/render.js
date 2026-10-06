/*
 * render.js — moteur de rendu logiciel façon 1989 : polygones pleins, algorithme du peintre,
 * découpage au plan proche, palette 3 bits par composante comme l'Atari ST.
 */
var SCR = window.SCR || (window.SCR = {});

SCR.Renderer = (function () {
  'use strict';

  // couleurs relevées sur les captures de l'original (palette ST 0..7 par composante)
  var ST = {
    sky: [2, 4, 7], hillNear: [2, 3, 3], hillFar: [1, 2, 3], ground: [3, 3, 2],
    roadLight: [5, 5, 4], roadDark: [4, 4, 3], side: [3, 1, 1], sideDark: [2, 0, 0], pillar: [7, 7, 7],
    lineL: [7, 7, 7], lineR: [7, 7, 0], car: [7, 1, 0], carDark: [4, 0, 0], carTop: [1, 1, 1],
    frame: [4, 2, 2], frameLight: [6, 4, 4], frameDark: [2, 0, 0], dash: [1, 2, 3], gauge: [7, 7, 7], text: [7, 7, 7]
  };
  function stc(c, stPal) {
    if (stPal) return 'rgb(' + ((c[0] * 255 / 7) | 0) + ',' + ((c[1] * 255 / 7) | 0) + ',' + ((c[2] * 255 / 7) | 0) + ')';
    return 'rgb(' + Math.min(255, c[0] * 36 + 6) + ',' + Math.min(255, c[1] * 36 + 6) + ',' + Math.min(255, c[2] * 36 + 6) + ')';
  }
  function shade(c, k) { return [Math.max(0, Math.min(7, Math.round(c[0] * k))), Math.max(0, Math.min(7, Math.round(c[1] * k))), Math.max(0, Math.min(7, Math.round(c[2] * k)))]; }

  function Renderer(canvas, params) {
    this.canvas = canvas;
    this.P = params;
    this.ctx = canvas.getContext('2d');
    this.hills = makeHills(1);
    this.resize();
  }

  Renderer.prototype.resize = function () {
    var s = this.P.view.renderScale | 0 || 1;
    this.W = 320 * s; this.H = 200 * s; this.s = s;
    if (this.canvas.width !== this.W) { this.canvas.width = this.W; this.canvas.height = this.H; }
  };

  function makeHills(seed) {
    var r = seed, pts = [[], []];
    function rnd() { r = (r * 16807) % 2147483647; return r / 2147483647; }
    for (var layer = 0; layer < 2; layer++) {
      var h = 0.03;
      for (var i = 0; i <= 64; i++) {
        h += (rnd() - 0.5) * (layer ? 0.03 : 0.05);
        h = Math.max(0.005, Math.min(layer ? 0.06 : 0.1, h));
        pts[layer].push(h);
      }
      pts[layer][64] = pts[layer][0];
    }
    return pts;
  }

  /* caméra : position + angles (lacet, tangage, roulis) */
  Renderer.prototype.setCamera = function (pos, yaw, pitch, roll) {
    this.cam = pos;
    this.cy = Math.cos(yaw); this.sy = Math.sin(yaw);
    this.cp = Math.cos(pitch); this.sp = Math.sin(pitch);
    this.cr = Math.cos(roll); this.sr = Math.sin(roll);
    this.yaw = yaw; this.pitchA = pitch; this.rollA = roll;
    this.f = (this.W / 2) / Math.tan(this.P.view.fov * Math.PI / 360);
  };

  Renderer.prototype.toCam = function (p) {
    var x = p[0] - this.cam[0], y = p[1] - this.cam[1], z = p[2] - this.cam[2];
    var x1 = x * this.cy - z * this.sy, z1 = x * this.sy + z * this.cy;
    var y2 = y * this.cp - z1 * this.sp, z2 = y * this.sp + z1 * this.cp;
    var x3 = x1 * this.cr - y2 * this.sr, y3 = y2 * this.cr + x1 * this.sr;
    return [x3, y3, z2];
  };

  Renderer.prototype.near = function () { return 0.25 * this.P.world.unitsPerMeter; };

  /* découpe un polygone (repère caméra) contre le plan z = near */
  function clipNear(poly, zn) {
    var out = [];
    for (var i = 0; i < poly.length; i++) {
      var a = poly[i], b = poly[(i + 1) % poly.length];
      var ain = a[2] >= zn, bin = b[2] >= zn;
      if (ain) out.push(a);
      if (ain !== bin) {
        var t = (zn - a[2]) / (b[2] - a[2]);
        out.push([a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, zn]);
      }
    }
    return out;
  }

  Renderer.prototype.project = function (c) {
    return [this.W / 2 + this.f * c[0] / c[2], this.H * 0.5 - this.f * c[1] / c[2]];
  };

  /* ajoute un polygone monde à la liste d'affichage */
  Renderer.prototype.addPoly = function (list, pts, color, bias) {
    var cam = [], zmax = -Infinity, zsum = 0;
    for (var i = 0; i < pts.length; i++) {
      var c = this.toCam(pts[i]); cam.push(c);
      if (c[2] > zmax) zmax = c[2];
      zsum += c[2];
    }
    if (zmax < this.near()) return;
    var cl = clipNear(cam, this.near());
    if (cl.length < 3) return;
    list.push({ z: zsum / pts.length + (bias || 0), zmax: zmax, pts: cl.map(this.project, this), color: color });
  };

  Renderer.prototype.fillPoly = function (pts, color) {
    var g = this.ctx;
    g.fillStyle = color;
    g.beginPath();
    g.moveTo(pts[0][0], pts[0][1]);
    for (var i = 1; i < pts.length; i++) g.lineTo(pts[i][0], pts[i][1]);
    g.closePath();
    g.fill();
  };

  Renderer.prototype.drawBackground = function () {
    var g = this.ctx, W = this.W, H = this.H, stPal = !!this.P.view.stPalette;
    g.fillStyle = stc(ST.sky, stPal);
    g.fillRect(0, 0, W, H);
    // sol : grand plan y = 0 autour de la caméra
    var R = 4e6, c = this.cam;
    var list = [];
    this.addPoly(list, [[c[0] - R, 0, c[2] - R], [c[0] + R, 0, c[2] - R], [c[0] + R, 0, c[2] + R], [c[0] - R, 0, c[2] + R]], stc(ST.ground, stPal));
    // collines : panorama attaché au cap (deux couches)
    var dist = 2e6;
    for (var layer = 1; layer >= 0; layer--) {
      var hs = this.hills[layer], col = stc(layer ? ST.hillFar : ST.hillNear, stPal);
      for (var i = 0; i < 64; i++) {
        var a0 = i / 64 * Math.PI * 2, a1 = (i + 1) / 64 * Math.PI * 2;
        var p0 = [c[0] + Math.sin(a0) * dist, 0, c[2] + Math.cos(a0) * dist], p1 = [c[0] + Math.sin(a1) * dist, 0, c[2] + Math.cos(a1) * dist];
        var t0 = [p0[0], hs[i] * dist * (layer ? 1.3 : 1), p0[2]], t1 = [p1[0], hs[i + 1] * dist * (layer ? 1.3 : 1), p1[2]];
        this.addPoly(list, [p0, p1, t1, t0], col);
      }
    }
    // ordre d'insertion conservé : sol, collines lointaines, collines proches
    for (var k = 0; k < list.length; k++) this.fillPoly(list[k].pts, list[k].color);
  };

  /* dessine le circuit + les voitures */
  Renderer.prototype.drawWorld = function (track, cars, focusSeg) {
    var P = this.P, stPal = !!P.view.stPalette, list = [], upm = P.world.unitsPerMeter;
    var light = [0.35, 0.85, -0.4], ll = Math.hypot(light[0], light[1], light[2]);
    light = [light[0] / ll, light[1] / ll, light[2] / ll];
    var from = focusSeg - (P.view.drawBehind | 0), to = focusSeg + (P.view.drawAhead | 0);
    if (to - from >= track.n) { from = 0; to = track.n - 1; }
    var lineW = 0.025, ws = P.track.roadWidthScale;
    for (var i = from; i <= to; i++) {
      var a = track.sec(i), b = track.sec(i + 1);
      var aL = a.L, aR = a.R, bL = b.L, bR = b.R;
      if (ws !== 1) {
        aL = widen(a.C, a.L, ws); aR = widen(a.C, a.R, ws); bL = widen(b.C, b.L, ws); bR = widen(b.C, b.R, ws);
      }
      // segments verticaux (bords de saut) : seulement les parois
      var horiz = Math.hypot(b.C[0] - a.C[0], b.C[2] - a.C[2]);
      // flancs du circuit, jusqu'au sol
      var pid = a.piece;
      var sideCol = stc((i & 3) === 0 ? ST.pillar : ST.side, stPal);
      this.addPoly(list, [aL, bL, [bL[0], 0, bL[2]], [aL[0], 0, aL[2]]], sideCol, 1);
      this.addPoly(list, [bR, aR, [aR[0], 0, aR[2]], [bR[0], 0, bR[2]]], sideCol, 1);
      // dessus de la route, ombré selon la pente
      var e1 = [bR[0] - aL[0], bR[1] - aL[1], bR[2] - aL[2]], e2 = [aR[0] - bL[0], aR[1] - bL[1], aR[2] - bL[2]];
      var n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]];
      var nl = Math.hypot(n[0], n[1], n[2]) || 1;
      if (n[1] < 0) nl = -nl;
      var lum = (n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) / nl;
      var base = (pid & 1) ? ST.roadDark : ST.roadLight;
      var col = stc(shade(base, 0.55 + 0.5 * Math.max(0, lum)), stPal);
      if (horiz < 1) continue;
      this.addPoly(list, [aL, aR, bR, bL], col, 0);
      // lignes de bord (blanche à gauche, jaune à droite)
      var lw = lineW;
      this.addPoly(list, [aL, lerp(aL, aR, lw), lerp(bL, bR, lw), bL], stc(ST.lineL, stPal), -0.5);
      this.addPoly(list, [lerp(aR, aL, lw), aR, bR, lerp(bR, bL, lw)], stc(ST.lineR, stPal), -0.5);
    }
    var self = this;
    cars.forEach(function (car) { if (car.visible !== false) self.addCar(list, car, stPal, upm); });
    list.sort(function (p, q) { return q.z - p.z; });
    for (var k = 0; k < list.length; k++) this.fillPoly(list[k].pts, list[k].color);
  };

  function lerp(a, b, t) { return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t]; }
  function widen(c, p, s) { return [c[0] + (p[0] - c[0]) * s, p[1], c[2] + (p[2] - c[2]) * s]; }

  /* voiture : boîte simple (caisse + habitacle) orientée selon cap/tangage/roulis */
  Renderer.prototype.addCar = function (list, car, stPal, upm) {
    var w = 0.95 * upm, l = 2.1 * upm, h = 0.55 * upm, ride = car.P.car.rideHeight * upm;
    var ch = Math.cos(car.heading), sh = Math.sin(car.heading);
    var cp = Math.cos(car.pitch), sp = Math.sin(car.pitch), cr = Math.cos(car.roll), sr = Math.sin(car.roll);
    function W(x, y, z) {
      // roulis (axe Z local), tangage (axe X local), puis cap
      var x1 = x * cr + y * sr, y1 = -x * sr + y * cr;
      var y2 = y1 * cp + z * sp, z2 = -y1 * sp + z * cp;
      return [car.pos[0] + x1 * ch + z2 * sh, car.pos[1] - ride + y2, car.pos[2] - x1 * sh + z2 * ch];
    }
    var b = [W(-w, 0.1 * upm, -l), W(w, 0.1 * upm, -l), W(w, 0.1 * upm, l), W(-w, 0.1 * upm, l),
             W(-w, h, -l), W(w, h, -l), W(w, h, l * 0.9), W(-w, h, l * 0.9)];
    var faces = [[4, 5, 6, 7, ST.car], [0, 1, 5, 4, ST.carDark], [2, 3, 7, 6, ST.carDark], [1, 2, 6, 5, ST.car], [3, 0, 4, 7, ST.car]];
    var self = this;
    faces.forEach(function (f) { self.addPoly(list, [b[f[0]], b[f[1]], b[f[2]], b[f[3]]], stc(f[4], stPal), -2); });
    var t = [W(-w * 0.7, h, -l * 0.5), W(w * 0.7, h, -l * 0.5), W(w * 0.6, h * 1.7, -l * 0.3), W(-w * 0.6, h * 1.7, -l * 0.3)];
    this.addPoly(list, t, stc(ST.carTop, stPal), -3);
  };

  /* habillage : cockpit et tableau de bord */
  Renderer.prototype.drawCockpit = function (hud) {
    var g = this.ctx, W = this.W, H = this.H, s = this.s, stPal = !!this.P.view.stPalette;
    var frame = stc(ST.frame, stPal), dark = stc(ST.frameDark, stPal), light = stc(ST.frameLight, stPal);
    if (this.P.view.cockpit && !this.P.view.chaseCam) {
      g.fillStyle = frame;
      // montants et bas de pare-brise
      g.fillRect(0, 0, W, 6 * s);
      g.beginPath(); g.moveTo(0, 0); g.lineTo(18 * s, 0); g.lineTo(10 * s, 140 * s); g.lineTo(0, 150 * s); g.fill();
      g.beginPath(); g.moveTo(W, 0); g.lineTo(W - 18 * s, 0); g.lineTo(W - 10 * s, 140 * s); g.lineTo(W, 150 * s); g.fill();
      g.fillStyle = light; g.fillRect(0, 6 * s, W, 1 * s);
      // capot / moteur
      g.fillStyle = dark;
      g.beginPath(); g.moveTo(0, 150 * s); g.lineTo(60 * s, 132 * s); g.lineTo(W - 60 * s, 132 * s); g.lineTo(W, 150 * s); g.lineTo(W, H); g.lineTo(0, H); g.fill();
      g.fillStyle = frame; g.fillRect(0, 158 * s, W, H - 158 * s);
      g.fillStyle = light; g.fillRect(0, 158 * s, W, 1 * s);
      // pare-brise fissuré selon les dégâts
      var dmg = Math.min(1, hud.damage / hud.maxDamage);
      if (dmg > 0) {
        g.strokeStyle = 'rgba(255,255,255,0.85)'; g.lineWidth = Math.max(1, s * 0.6);
        g.beginPath();
        var x = W * 0.5, y = 8 * s;
        g.moveTo(x, y);
        for (var k = 0; k < 14 * dmg; k++) { x += ((k * 37) % 11 - 5) * s * 1.5; y += 9 * s; g.lineTo(x, y); }
        g.stroke();
      }
    } else {
      g.fillStyle = frame; g.fillRect(0, 158 * s, W, H - 158 * s);
    }
    // tableau de bord
    g.fillStyle = stc(ST.dash, stPal); g.fillRect(70 * s, 162 * s, 180 * s, 34 * s);
    var kmh = Math.max(0, hud.speedKmh), mph = kmh / 1.609;
    g.fillStyle = stc(ST.gauge, stPal);
    g.font = (7 * s) + 'px monospace';
    g.textBaseline = 'top';
    for (var v = 50; v <= 230; v += 30) { var gx = 78 * s + (v - 50) / 180 * 160 * s; g.fillText(String(v), gx - 6 * s, 172 * s); g.fillRect(gx, 165 * s, s, 5 * s); }
    var nx = 78 * s + Math.max(0, Math.min(1, (mph - 50) / 180)) * 160 * s;
    g.fillStyle = 'rgb(255,255,0)'; g.fillRect(78 * s, 182 * s, Math.max(0, nx - 78 * s), 4 * s);
    // boîtes gauche / droite
    g.fillStyle = 'rgb(182,182,182)';
    g.fillRect(4 * s, 162 * s, 62 * s, 15 * s); g.fillRect(4 * s, 180 * s, 62 * s, 15 * s);
    g.fillRect(254 * s, 162 * s, 62 * s, 15 * s); g.fillRect(254 * s, 180 * s, 62 * s, 15 * s);
    g.fillStyle = 'rgb(0,0,0)';
    g.fillText('L ' + hud.lap + '/' + hud.laps, 7 * s, 165 * s);
    g.fillText(fmtTime(hud.time), 7 * s, 183 * s);
    g.fillText('B ' + hud.boost.toFixed(1), 257 * s, 165 * s);
    // barre de dégâts
    g.fillStyle = 'rgb(72,0,0)'; g.fillRect(257 * s, 184 * s, 56 * s * Math.min(1, hud.damage / hud.maxDamage), 7 * s);
    g.fillStyle = 'rgb(0,0,0)';
    g.fillText(Math.round(mph) + ' mph', 98 * s, 188 * s);
    if (hud.message) {
      g.font = 'bold ' + (10 * s) + 'px monospace';
      var tw = g.measureText(hud.message).width;
      g.fillStyle = 'rgba(0,0,0,0.6)'; g.fillRect(W / 2 - tw / 2 - 4 * s, 60 * s, tw + 8 * s, 16 * s);
      g.fillStyle = 'rgb(255,255,0)'; g.fillText(hud.message, W / 2 - tw / 2, 63 * s);
    }
  };

  function fmtTime(t) {
    var m = Math.floor(t / 60), s = t - m * 60;
    return m + ':' + (s < 10 ? '0' : '') + s.toFixed(1);
  }
  Renderer.fmtTime = fmtTime;

  /* vue de dessus du circuit (écran de présentation) */
  Renderer.prototype.drawMap = function (track, cars) {
    var g = this.ctx, W = this.W, H = this.H, b = track.bounds;
    g.fillStyle = 'rgb(0,0,0)'; g.fillRect(0, 0, W, H);
    var sc = Math.min((W - 20) / (b.maxX - b.minX), (H - 30) / (b.maxZ - b.minZ));
    var ox = (W - (b.maxX - b.minX) * sc) / 2, oy = 10;
    function X(p) { return ox + (p[0] - b.minX) * sc; }
    function Y(p) { return oy + (b.maxZ - p[2]) * sc; }
    for (var i = 0; i < track.n; i++) {
      var a = track.sec(i), c = track.sec(i + 1), h = a.C[1] / Math.max(1, b.maxY);
      g.fillStyle = 'rgb(' + (80 + 175 * h | 0) + ',' + (80 + 100 * h | 0) + ',' + (200 - 150 * h | 0) + ')';
      g.beginPath(); g.moveTo(X(a.L), Y(a.L)); g.lineTo(X(a.R), Y(a.R)); g.lineTo(X(c.R), Y(c.R)); g.lineTo(X(c.L), Y(c.L)); g.fill();
    }
    (cars || []).forEach(function (car, k) {
      g.fillStyle = k ? 'rgb(255,80,80)' : 'rgb(255,255,0)';
      g.fillRect(X(car.pos) - 3, Y(car.pos) - 3, 6, 6);
    });
  };

  return Renderer;
})();
