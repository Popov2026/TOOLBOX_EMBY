/*
 * physics.js — dynamique de la voiture (modèle paramétrable, inspiré du comportement
 * de l'original : suspension, sauts, chutes, dégâts à l'atterrissage, grue de remise en piste).
 * Toutes les positions sont en unités du jeu ; les paramètres sont en unités SI.
 */
var SCR = window.SCR || (window.SCR = {});

SCR.Car = (function () {
  'use strict';

  function dot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
  function norm(a) { var l = Math.hypot(a[0], a[1], a[2]) || 1; return [a[0] / l, a[1] / l, a[2] / l]; }
  function cross(a, b) { return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]; }

  function Car(track, params, name) {
    this.track = track;
    this.P = params;
    this.name = name || 'joueur';
    this.input = { throttle: 0, brake: 0, steer: 0, boost: 0 };
    this.reset(0, 0);
    this.damage = 0;
    this.wrecked = false;
    this.boostLeft = params.car.boostCapacity;
    this.events = [];
  }

  /* place la voiture sur l'axe du circuit à la distance s, décalée latéralement de lat (m) */
  Car.prototype.reset = function (s, lat) {
    var t = this.track, pt = t.pointAt(s), upm = this.P.world.unitsPerMeter;
    var cx = Math.cos(pt.heading), sx = Math.sin(pt.heading);
    var off = (lat || 0) * upm;
    this.pos = [pt.pos[0] + cx * off, pt.pos[1] + this.P.car.rideHeight * upm, pt.pos[2] - sx * off];
    this.vel = [0, 0, 0];
    this.heading = pt.heading;
    this.pitch = 0; this.roll = 0;
    this.steer = 0;
    this.seg = pt.seg;
    this.grounded = true;
    this.airTime = 0;
    this.lastGood = s;
    this.crane = 0;
    this.speed = 0;
    this.normal = [0, 1, 0];
  };

  Car.prototype.forward = function () { return [Math.sin(this.heading), 0, Math.cos(this.heading)]; };

  Car.prototype.emit = function (type, value) { this.events.push({ type: type, value: value }); };

  Car.prototype.addDamage = function (amount, why) {
    if (!this.P.damage.enabled || amount <= 0) return;
    this.damage += amount;
    this.emit('damage', { amount: amount, why: why });
    if (this.damage >= this.P.damage.maxDamage) { this.wrecked = true; this.emit('wrecked'); }
  };

  /* chute hors piste : la grue replace la voiture après une pénalité */
  Car.prototype.fallOff = function (why) {
    if (this.crane > 0) return;
    this.crane = this.P.damage.crashPenalty;
    this.addDamage(this.P.damage.wallHit, why || 'chute');
    this.emit('crash', why || 'chute');
  };

  Car.prototype.step = function (dt) {
    var P = this.P, C = P.car, upm = P.world.unitsPerMeter, g = P.world.gravity * upm, t = this.track;
    if (this.crane > 0) {                       // grue : on attend puis on repose la voiture
      this.crane -= dt;
      if (this.crane <= 0) { this.crane = 0; this.reset(this.lastGood, 0); this.pos[1] += 6 * upm; this.emit('crane'); }
      else return;
    }
    var inp = this.input;
    // braquage progressif, réduit avec la vitesse
    var vkmh = Math.abs(this.speed) / upm * 3.6;
    var maxSteer = C.maxSteer * (1 - C.steerSpeedFalloff * Math.min(1, vkmh / C.topSpeed));
    var target = inp.steer * maxSteer;
    var ds = C.steerRate * dt;
    this.steer += Math.max(-ds, Math.min(ds, target - this.steer));

    var surf = t.surface(this.pos[0], this.pos[2], this.seg, this.pos[1]);
    var ride = C.rideHeight * upm;
    var acc = [0, -g, 0];
    var fwd = this.forward();

    if (surf) {
      this.seg = surf.seg;
      var n = [surf.nx, surf.ny, surf.nz];
      var gap = this.pos[1] - (surf.y + ride);
      // paroi : la surface est nettement au-dessus de la voiture (bord de rampe, réception ratée)
      if (gap < -1.2 * upm && surf.ny < 1 / Math.hypot(1, C.maxClimb)) {
        this.hitWall(surf);
      } else if (gap < -1.2 * upm && !this.grounded) {
        this.hitWall(surf);
      } else if (gap <= (this.grounded ? 0.3 : 0.05) * upm) {
        // contact (hystérésis : on reste « au sol » sur les petites bosses)
        if (!this.grounded) {
          var vn = -dot(this.vel, n) / upm;     // vitesse d'impact (m/s)
          if (surf.ny < 0.6 && vn > P.damage.landingThreshold) { this.hitWall(surf); return; }
          if (this.airTime > 0.25 && vn > P.damage.landingThreshold)
            this.addDamage((vn - P.damage.landingThreshold) * P.damage.landingFactor, 'atterrissage');
          if (this.airTime > 0.25) this.emit('land', vn);
          this.airTime = 0;
        }
        this.grounded = true;
        // suspension (ressort + amortisseur le long de la normale)
        var vnorm = dot(this.vel, n);
        var springA = -C.suspension * gap - C.damping * vnorm;
        if (springA < 0) springA = 0;
        acc[0] += n[0] * springA; acc[1] += n[1] * springA; acc[2] += n[2] * springA;
        // repère de la route
        var f = norm([fwd[0] - n[0] * dot(fwd, n), fwd[1] - n[1] * dot(fwd, n), fwd[2] - n[2] * dot(fwd, n)]);
        var side = cross(n, f);
        var v = dot(this.vel, f), vl = dot(this.vel, side);
        // traction / freinage / boost
        var top = C.topSpeed / 3.6 * upm;
        var a = 0;
        if (inp.throttle > 0) a += C.enginePower * upm * inp.throttle * Math.max(0, 1 - (v / top) * (v / top));
        if (inp.boost && this.boostLeft > 0) { a += C.boostPower * upm; this.boostLeft = Math.max(0, this.boostLeft - dt); }
        if (inp.brake > 0) {
          if (v > 0.5 * upm) a -= C.brake * upm * inp.brake;
          else a -= C.enginePower * 0.6 * upm * inp.brake * Math.max(0, 1 + v / (top * 0.25));   // marche arrière
        }
        a -= Math.sign(v) * C.rolling * upm;
        a -= C.drag * v * Math.abs(v) / upm;
        acc[0] += f[0] * a; acc[1] += f[1] * a; acc[2] += f[2] * a;
        // adhérence latérale : on annule la vitesse transversale dans la limite du grip
        var maxLat = C.grip * upm * dt, cl = Math.max(-maxLat, Math.min(maxLat, vl));
        this.vel[0] -= side[0] * cl; this.vel[1] -= side[1] * cl; this.vel[2] -= side[2] * cl;
        // rotation (modèle bicyclette, limité par l'adhérence)
        var yawRate = v * Math.tan(this.steer) / (C.wheelbase * upm);
        var maxYaw = C.grip * upm / Math.max(1, Math.abs(v));
        yawRate = Math.max(-maxYaw, Math.min(maxYaw, yawRate));
        this.heading += yawRate * dt;
        // assiette visuelle : tangage / roulis d'après la normale
        var tp = Math.atan2(dot(n, fwd), n[1]);
        var right = [Math.cos(this.heading), 0, -Math.sin(this.heading)];
        var tr = Math.atan2(dot(n, right), n[1]);
        this.pitch += (-tp - this.pitch) * Math.min(1, dt * 12);
        this.roll += (tr - this.roll) * Math.min(1, dt * 12);
        this.lastGood = t.progress(surf.seg, surf.along);
        // sortie latérale : bord de route dépassé
        if (Math.abs(surf.lat) > 1.0) this.grounded = false;
      } else {
        this.grounded = false;
      }
    } else {
      this.grounded = false;
    }

    if (!this.grounded) {
      this.airTime += dt;
      this.heading += this.steer * C.airControl * dt;
      this.pitch += (-0.25 - this.pitch) * dt * 0.6;
      if (!surf && this.pos[1] < t.baseHeightWorld + 2 * upm) this.fallOff('sortie de piste');
      if (this.pos[1] < -50 * upm) this.fallOff('chute');
    }

    // intégration
    this.vel[0] += acc[0] * dt; this.vel[1] += acc[1] * dt; this.vel[2] += acc[2] * dt;
    this.pos[0] += this.vel[0] * dt; this.pos[1] += this.vel[1] * dt; this.pos[2] += this.vel[2] * dt;
    if (this.pos[1] < 0) { this.pos[1] = 0; this.fallOff('sol'); }
    this.speed = dot(this.vel, this.forward());
    if (this.wrecked) { this.vel[0] *= 0.9; this.vel[2] *= 0.9; }
  };

  Car.prototype.hitWall = function (surf) {
    var upm = this.P.world.unitsPerMeter, sp = Math.hypot(this.vel[0], this.vel[2]) / upm;
    this.emit('wall', sp);
    this.fallOff('choc contre une paroi');
  };

  return Car;
})();

/* Pilote automatique (adversaire) : suit l'axe du circuit avec une vitesse cible. */
SCR.AutoPilot = function (car, params) {
  this.car = car; this.P = params;
};
SCR.AutoPilot.prototype.update = function () {
  var car = this.car, t = car.track, upm = this.P.world.unitsPerMeter;
  var s = t.progress(car.seg, 0);
  var ahead = t.pointAt(s + Math.max(12 * upm, Math.abs(car.speed) * 0.6));
  var dx = ahead.pos[0] - car.pos[0], dz = ahead.pos[2] - car.pos[2];
  var want = Math.atan2(dx, dz), err = want - car.heading;
  while (err > Math.PI) err -= 2 * Math.PI;
  while (err < -Math.PI) err += 2 * Math.PI;
  car.input.steer = Math.max(-1, Math.min(1, err * 3));
  var top = this.P.car.topSpeed / 3.6 * upm * this.P.ai.skill;
  // vitesse limitée par la courbure à venir (v² = adhérence × rayon)
  var look = [20, 40, 70], h0 = t.pointAt(s).heading;
  for (var k = 0; k < look.length; k++) {
    var pa = t.pointAt(s + look[k] * upm), dh = pa.heading - h0;
    while (dh > Math.PI) dh -= 2 * Math.PI;
    while (dh < -Math.PI) dh += 2 * Math.PI;
    if (Math.abs(dh) > 0.05) {
      var R = look[k] * upm / Math.abs(dh), vmax = Math.sqrt(this.P.car.grip * upm * 0.8 * R);
      if (vmax < top) top = vmax;
    }
  }
  car.input.throttle = car.speed < top ? 1 : 0;
  if (car.speed > top * 1.15) car.input.brake = 1;
  if (car.speed <= top * 1.15) car.input.brake = 0;
  car.input.boost = car.speed < top * 0.95 && car.speed > top * 0.5 ? 1 : 0;
};
