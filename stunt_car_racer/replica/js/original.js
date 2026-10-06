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
    if (practiceTrack === null || practiceTrack === undefined) {
      var c = e.cpu;
      c.pc = SCR.Engine.ADDR.boot; c.s = 0; c.a[7] = 0x103da; c.ssp = 0x7000; c.ipl = 0;
    } else {
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
    while (this.acc >= 1 && n < 20) {
      e.setInput(this.joystick());
      e.runFrame();
      this.acc -= 1; n++;
    }
    this.draw();
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
