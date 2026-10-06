/*
 * game.js — boucle de jeu : présentation du circuit, départ, course, tours, arrivée.
 */
var SCR = window.SCR || (window.SCR = {});

SCR.Game = (function () {
  'use strict';

  var DT = 1 / 120;

  function Game(canvas, params) {
    this.P = params;
    this.renderer = new SCR.Renderer(canvas, params);
    this.keys = {};
    this.state = 'idle';
    this.trackDef = null;
    this.message = '';
    this.msgTime = 0;
    this.acc = 0;
    this.last = 0;
    this.onFinish = null;
    var self = this;
    window.addEventListener('keydown', function (e) {
      if (/^(Arrow|Space)/.test(e.code)) e.preventDefault();
      self.keys[e.code] = true;
      if (e.code === 'KeyC') { self.P.view.chaseCam = self.P.view.chaseCam ? 0 : 1; }
      if (e.code === 'KeyP') { self.paused = !self.paused; }
      if (e.code === 'KeyR' && self.player) { self.player.fallOff('remise en piste'); }
      if ((e.code === 'Enter' || e.code === 'Space') && self.state === 'preview') self.startRace();
    });
    window.addEventListener('keyup', function (e) { self.keys[e.code] = false; });
  }

  Game.prototype.load = function (trackDef) {
    this.trackDef = trackDef;
    this.track = new SCR.Track(trackDef, this.P);
    this.track.baseHeightWorld = this.P.track.baseHeight;
    this.player = new SCR.Car(this.track, this.P, 'joueur');
    this.cars = [this.player];
    this.opponent = null;
    if (this.P.ai.enabled) {
      this.opponent = new SCR.Car(this.track, this.P, 'adversaire');
      this.pilot = new SCR.AutoPilot(this.opponent, this.P);
      this.cars.push(this.opponent);
    }
    this.placeCars();
    this.state = 'preview';
    this.say('Entrée : départ — C : caméra — R : grue', 1e9);
  };

  Game.prototype.placeCars = function () {
    var upm = this.P.world.unitsPerMeter;
    this.player.reset(0, -1.2);
    if (this.opponent) this.opponent.reset(this.P.ai.startGap * upm, 1.2);
    this.cars.forEach(function (c) {
      c.lap = 0; c.halfway = false; c.finished = false; c.prevProgress = 0; c.damage = 0; c.wrecked = false;
      c.boostLeft = c.P.car.boostCapacity; c.bestLap = null; c.lapStart = 0; c.events = [];
    });
  };

  Game.prototype.startRace = function () {
    this.placeCars();
    var upm = this.P.world.unitsPerMeter;
    // la grue dépose les voitures
    this.cars.forEach(function (c) { c.pos[1] += 5 * upm; });
    this.state = 'countdown';
    this.message = ''; this.msgTime = 0;
    this.countdown = 3;
    this.time = 0;
  };

  Game.prototype.say = function (txt, dur) { this.message = txt; this.msgTime = dur || 2; };

  Game.prototype.readInput = function () {
    var k = this.keys, inp = this.player.input;
    var gp = navigator.getGamepads ? navigator.getGamepads()[0] : null;
    var left = k.ArrowLeft || k.KeyA, right = k.ArrowRight || k.KeyD;
    var steer = (right ? 1 : 0) - (left ? 1 : 0);
    var thr = (k.ArrowUp || k.KeyW) ? 1 : 0, brk = (k.ArrowDown || k.KeyS) ? 1 : 0;
    var boost = (k.Space || k.ShiftLeft || k.ShiftRight) ? 1 : 0;
    if (gp) {
      if (Math.abs(gp.axes[0]) > 0.15) steer = gp.axes[0];
      if (gp.buttons[0] && gp.buttons[0].pressed) thr = 1;
      if (gp.buttons[7] && gp.buttons[7].value > 0.1) thr = gp.buttons[7].value;
      if (gp.buttons[6] && gp.buttons[6].value > 0.1) brk = gp.buttons[6].value;
      if (gp.buttons[1] && gp.buttons[1].pressed) brk = 1;
      if ((gp.buttons[2] && gp.buttons[2].pressed) || (gp.buttons[5] && gp.buttons[5].pressed)) boost = 1;
    }
    inp.steer = steer; inp.throttle = thr; inp.brake = brk; inp.boost = boost;
  };

  Game.prototype.updateLaps = function (car) {
    var t = this.track, p = t.progress(car.seg, 0), L = t.length;
    if (p > L * 0.4 && p < L * 0.6) car.halfway = true;
    if (car.halfway && car.prevProgress > L * 0.8 && p < L * 0.2) {
      car.halfway = false;
      car.lap++;
      var lt = this.time - car.lapStart; car.lapStart = this.time;
      if (car.bestLap === null || lt < car.bestLap) car.bestLap = lt;
      if (car === this.player) this.say('Tour ' + car.lap + ' : ' + SCR.Renderer.fmtTime(lt), 2.5);
      if (car.lap >= this.P.track.laps && !car.finished) {
        car.finished = true;
        if (!this.winner) this.winner = car;
        if (car === this.player) this.finish();
      }
    }
    car.prevProgress = p;
  };

  Game.prototype.finish = function () {
    this.state = 'finished';
    var won = this.winner === this.player;
    this.say(this.player.wrecked ? 'ÉPAVE !' : (won ? 'VICTOIRE ! ' : 'Arrivée : ') + SCR.Renderer.fmtTime(this.time), 1e9);
    if (this.onFinish) this.onFinish({ time: this.time, won: won, best: this.player.bestLap });
  };

  Game.prototype.collideCars = function () {
    if (!this.opponent) return;
    var a = this.player, b = this.opponent, upm = this.P.world.unitsPerMeter;
    var dx = b.pos[0] - a.pos[0], dy = b.pos[1] - a.pos[1], dz = b.pos[2] - a.pos[2];
    var d = Math.hypot(dx, dy, dz), R = 2.2 * upm;
    if (d < R && d > 1e-3) {
      var nx = dx / d, ny = dy / d, nz = dz / d, push = (R - d) / 2;
      a.pos[0] -= nx * push; a.pos[2] -= nz * push; b.pos[0] += nx * push; b.pos[2] += nz * push;
      var rv = (b.vel[0] - a.vel[0]) * nx + (b.vel[1] - a.vel[1]) * ny + (b.vel[2] - a.vel[2]) * nz;
      if (rv < 0) {
        var j = -rv * 0.8;
        a.vel[0] -= nx * j / 2; a.vel[2] -= nz * j / 2; b.vel[0] += nx * j / 2; b.vel[2] += nz * j / 2;
        var sp = j / upm;
        if (sp > 3) { a.addDamage(sp * 0.8, 'collision'); b.addDamage(sp * 0.8, 'collision'); }
      }
    }
  };

  Game.prototype.step = function (dt) {
    var self = this;
    if (this.state === 'countdown') {
      this.countdown -= dt;
      this.cars.forEach(function (c) { c.input.throttle = 0; c.input.steer = 0; c.input.boost = 0; c.input.brake = 1; c.step(dt); });
      var n = Math.ceil(this.countdown);
      this.message = n > 0 ? String(n) : 'PARTEZ !';
      this.msgTime = 1;
      if (this.countdown <= 0) { this.state = 'race'; this.say('PARTEZ !', 1); this.time = 0; }
      return;
    }
    if (this.state !== 'race' && this.state !== 'finished') return;
    if (this.state === 'race') this.time += dt;
    this.readInput();
    if (this.player.wrecked || this.state === 'finished') { this.player.input.throttle = 0; this.player.input.boost = 0; this.player.input.brake = 1; }
    if (this.pilot) this.pilot.update();
    this.cars.forEach(function (c) { c.step(dt); self.updateLaps(c); });
    this.collideCars();
    this.player.events.forEach(function (e) {
      if (e.type === 'crash') self.say('CHUTE : ' + e.value, 2);
      if (e.type === 'damage' && e.value.amount > 5) self.say('Dégâts : ' + e.value.why, 1.2);
      if (e.type === 'wrecked') { self.say('VOITURE DÉTRUITE', 1e9); self.finish(); }
    });
    this.cars.forEach(function (c) { c.events.length = 0; });
  };

  Game.prototype.frame = function (now) {
    if (!this.last) this.last = now;
    var dt = Math.min(0.1, (now - this.last) / 1000);
    this.last = now;
    if (!this.paused) {
      this.acc += dt;
      while (this.acc >= DT) { this.step(DT); this.acc -= DT; }
      if (this.msgTime < 1e8) this.msgTime -= dt;
    }
    this.draw();
  };

  Game.prototype.draw = function () {
    var r = this.renderer, P = this.P, upm = P.world.unitsPerMeter;
    r.resize();
    if (!this.track) { r.ctx.fillStyle = '#000'; r.ctx.fillRect(0, 0, r.W, r.H); return; }
    if (this.state === 'preview') { r.drawMap(this.track, this.cars); this.drawMessage(); return; }
    var car = this.player, pos, yaw = car.heading, pitch = car.pitch, roll = car.roll;
    if (P.view.chaseCam) {
      var back = 9 * upm, up = 3.2 * upm;
      pos = [car.pos[0] - Math.sin(yaw) * back, car.pos[1] + up, car.pos[2] - Math.cos(yaw) * back];
      pitch = -0.18; roll = 0;
      car.visible = true;
    } else {
      var eye = P.car.rideHeight * upm * 0 + P.view.eyeHeight * upm;
      pos = [car.pos[0], car.pos[1] + eye - P.car.rideHeight * upm * 0.5, car.pos[2]];
      car.visible = false;
    }
    r.setCamera(pos, yaw, pitch, roll);
    r.drawBackground();
    r.drawWorld(this.track, this.cars, car.seg);
    r.drawCockpit({
      speedKmh: car.speed / upm * 3.6, lap: Math.min(P.track.laps, car.lap + 1), laps: P.track.laps,
      time: this.time || 0, boost: car.boostLeft, damage: car.damage, maxDamage: P.damage.maxDamage,
      message: this.msgTime > 0 ? this.message : ''
    });
  };

  Game.prototype.drawMessage = function () {
    var r = this.renderer, g = r.ctx, s = r.s;
    g.font = 'bold ' + (9 * s) + 'px monospace';
    g.fillStyle = 'rgb(255,255,0)';
    var t = this.track.name + ' — Entrée pour partir';
    g.fillText(t, (r.W - g.measureText(t).width) / 2, r.H - 14 * s);
  };

  Game.prototype.run = function () {
    var self = this;
    function loop(t) { self.frame(t); requestAnimationFrame(loop); }
    requestAnimationFrame(loop);
  };

  return Game;
})();
