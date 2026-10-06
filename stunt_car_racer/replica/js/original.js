/*
 * original.js — mode « Original » : le jeu d'origine complet (menus, course, rendu, tableau
 * de bord) exécuté par le CPU 68000 JavaScript à partir de la disquette de l'utilisateur.
 * Les entrées sont injectées directement dans la mémoire du jeu (table des touches et
 * octet joystick), l'image est l'écran ST (320×200) agrandi.
 */
var SCR = window.SCR || (window.SCR = {});

SCR.OriginalMode = (function () {
  'use strict';

  function OriginalMode(canvas, params) {
    this.canvas = canvas;
    this.P = params;
    this.ctx = canvas.getContext('2d');
    this.off = document.createElement('canvas');
    this.off.width = 320; this.off.height = 200;
    this.offCtx = this.off.getContext('2d');
    this.img = this.offCtx.createImageData(320, 200);
    this.engine = null;
    this.running = false;
    this.acc = 0; this.last = 0;
    this.keys = {};
    var self = this;
    this.onKey = function (e) {
      if (!self.running) return;
      var down = e.type === 'keydown';
      if (/^(Arrow|Space|Tab|Backspace|F\d)/.test(e.code)) e.preventDefault();
      self.keys[e.code] = down;
      var sc = SCR.Engine.SCANCODES[e.code];
      if (sc && self.engine) self.engine.setKey(sc, down);
    };
    window.addEventListener('keydown', this.onKey);
    window.addEventListener('keyup', this.onKey);
  }

  /* bytes : image .st (ou GAME.PUT).  practiceTrack : 0..7 pour aller directement à
     l'entraînement sur ce circuit, ou null pour démarrer le jeu complet (écran titre). */
  OriginalMode.prototype.start = function (bytes, practiceTrack) {
    var e = this.engine = new SCR.Engine(bytes);
    try { this.trackDefs = SCR.data.tracksFromFile(bytes); } catch (err) { this.trackDefs = null; }
    this.tracks = {};
    this.prev = this.cur = null;
    if (!this.renderer) this.renderer = new SCR.Renderer(this.canvas, this.P);
    if (practiceTrack === null || practiceTrack === undefined) {
      var c = e.cpu;
      c.pc = SCR.Engine.ADDR.boot; c.s = 0; c.a[7] = 0x103da; c.ssp = 0x7000; c.ipl = 0;
      e.tuning = this.P.physOrig;
      e.passChecksum();
    } else {
      e.tuning = this.P.physOrig;
      e.boot();
      e.startPractice(practiceTrack);
    }
    this.running = true;
    this.acc = 0; this.last = 0;
  };

  OriginalMode.prototype.stop = function () { this.running = false; };

  OriginalMode.prototype.joystick = function () {
    var k = this.keys, j = { up: k.ArrowUp, down: k.ArrowDown, left: k.ArrowLeft, right: k.ArrowRight,
                             fire: k.Space || k.ControlLeft || k.ControlRight || k.ShiftLeft };
    var gp = navigator.getGamepads ? navigator.getGamepads()[0] : null;
    if (gp) {
      if (gp.axes[0] < -0.4) j.left = true;
      if (gp.axes[0] > 0.4) j.right = true;
      if (gp.axes[1] < -0.4) j.up = true;
      if (gp.axes[1] > 0.4) j.down = true;
      if (gp.buttons[12] && gp.buttons[12].pressed) j.up = true;
      if (gp.buttons[13] && gp.buttons[13].pressed) j.down = true;
      if (gp.buttons[14] && gp.buttons[14].pressed) j.left = true;
      if (gp.buttons[15] && gp.buttons[15].pressed) j.right = true;
      if (gp.buttons[0] && gp.buttons[0].pressed) j.fire = true;
      // gâchettes : accélérer = haut, freiner = bas (pratique sur manette moderne)
      if (gp.buttons[7] && gp.buttons[7].value > 0.3) j.up = true;
      if (gp.buttons[6] && gp.buttons[6].value > 0.3) j.down = true;
    }
    return j;
  };

  OriginalMode.prototype.frame = function (now) {
    if (!this.running || !this.engine) return;
    if (!this.last) this.last = now;
    var dt = Math.min(0.25, (now - this.last) / 1000);
    this.last = now;
    var hz = 50 * (this.P.original ? this.P.original.speed : 1);
    this.acc += dt * hz;
    var e = this.engine, n = 0;
    var key = JSON.stringify(this.P.physOrig);
    if (key !== this.tuneKey) { this.tuneKey = key; e.setTuning(this.P.physOrig); }
    while (this.acc >= 1 && n < 20) {
      e.setInput(this.joystick());
      e.runFrame();
      this.acc -= 1; n++;
      var st = this.carState();
      if (!this.cur || st.key !== this.cur.key) { this.prev = this.cur; this.cur = st; this.tickAt = now; }
    }
    var view = this.P.original ? this.P.original.view | 0 : 0;
    if (view && this.trackDefs && this.cur) this.drawModern(now, view);
    else this.draw();
  };

  /* état de la voiture du joueur dans la mémoire du jeu (voir docs/RETRO_INGENIERIE.md) */
  OriginalMode.prototype.carState = function () {
    var e = this.engine, A = 2 * Math.PI / 65536;
    var x = e.l(0x10ac2) / 65536, y = e.l(0x10ac6) / 65536, z = e.l(0x10aca) / 65536;
    return { x: x, y: y, z: z, yaw: e.w(0x10ad0) * A, pitch: e.sw(0x10ace) * A, roll: e.sw(0x10ad2) * A,
             track: e.b(0x1112d) & 7, key: e.l(0x10ac2) + ',' + e.l(0x10ac6) + ',' + e.l(0x10aca) + ',' + e.w(0x10ad0) };
  };

  function lerpAngle(a, b, t) {
    var d = b - a;
    while (d > Math.PI) d -= 2 * Math.PI;
    while (d < -Math.PI) d += 2 * Math.PI;
    return a + d * t;
  }

  /* scène redessinée par le moteur 3D de la réplique à partir de l'état du jeu d'origine */
  OriginalMode.prototype.drawModern = function (now, view) {
    var P = this.P, s = this.cur, p = this.prev || s;
    var ti = s.track;
    if (!this.tracks[ti]) { this.tracks[ti] = new SCR.Track(this.trackDefs[ti], P); this.tracks[ti].baseHeightWorld = 0; }
    var tr = this.tracks[ti], r = this.renderer;
    // interpolation entre les deux derniers ticks (0,12 s à vitesse 1)
    var dur = 120 / (P.original.speed || 1), t = Math.max(0, Math.min(1, (now - this.tickAt) / dur));
    var X = p.x + (s.x - p.x) * t, Y = p.y + (s.y - p.y) * t, Z = p.z + (s.z - p.z) * t;
    var yaw = lerpAngle(p.yaw, s.yaw, t), pitch = lerpAngle(p.pitch, s.pitch, t), roll = lerpAngle(p.roll, s.roll, t);
    // repère du jeu -> repère de la réplique : x,z ×16 ; y = hauteur brute/32 -> (brute - base) × échelle
    var upm = P.world.unitsPerMeter, vs = tr.vscale;
    var pos = [X * 16, (Y * 32 - tr.baseY) * vs + P.track.baseHeight + P.view.eyeHeight * upm, Z * 16];
    var surf = tr.surface(pos[0], pos[2], this.seg || 0, pos[1], tr.n);
    if (surf) this.seg = surf.seg;
    r.resize();
    r.setCamera(pos, yaw, P.original.pitchSign * pitch, P.original.rollSign * roll);
    r.drawBackground();
    r.drawWorld(tr, [], this.seg || 0);
    if (view === 2) {
      // vignette : l'image d'origine, pour comparer
      this.engine.screenRGBA(this.img.data);
      this.offCtx.putImageData(this.img, 0, 0);
      var g = r.ctx, w = r.W * 0.32, h = w * 200 / 320;
      g.drawImage(this.off, r.W - w - 4, 4, w, h);
      g.strokeStyle = '#ff0'; g.strokeRect(r.W - w - 4, 4, w, h);
    }
  };

  OriginalMode.prototype.draw = function () {
    if (!this.engine) return;
    this.engine.screenRGBA(this.img.data);
    this.offCtx.putImageData(this.img, 0, 0);
    var c = this.canvas, g = this.ctx;
    g.imageSmoothingEnabled = !!(this.P.original && this.P.original.smooth);
    g.drawImage(this.off, 0, 0, c.width, c.height);
  };

  return OriginalMode;
})();
