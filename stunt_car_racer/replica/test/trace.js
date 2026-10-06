// trace détaillée d'un tour : node test/trace.js DISQUE.st indexCircuit secondes [pas]
global.window = global; global.localStorage = { getItem() { return null; }, setItem() {} };
const fs = require('fs'), path = require('path');
for (const f of ['scrdata', 'params', 'track', 'physics']) require(path.join(__dirname, '..', 'js', f + '.js'));
const P = SCR.defaultParams();
const defs = SCR.data.tracksFromFile(new Uint8Array(fs.readFileSync(process.argv[2])));
const def = defs[+process.argv[3]], secs = +(process.argv[4] || 20), every = +(process.argv[5] || 0.25);
const upm = P.world.unitsPerMeter;
const t = new SCR.Track(def, P); t.baseHeightWorld = P.track.baseHeight;
const car = new SCR.Car(t, P), pilot = new SCR.AutoPilot(car, P), dt = 1 / 120;
for (let i = 0; i < secs / dt; i++) {
  pilot.update(); car.step(dt);
  const s = t.surface(car.pos[0], car.pos[2], car.seg, car.pos[1]);
  const ev = car.events.map(e => e.type + (typeof e.value === 'number' ? '=' + e.value.toFixed(1) : e.value && e.value.why ? '=' + e.value.why : '')).join(',');
  car.events.length = 0;
  if (ev || (i % Math.round(every / dt)) === 0)
    console.log((i * dt).toFixed(2), 'seg', car.seg, 'pc', t.sec(car.seg).piece, 'lat', s ? s.lat.toFixed(2) : '----', 'y', (car.pos[1] / upm).toFixed(1),
      'sy', s ? (s.y / upm).toFixed(1) : '--', 'v', (car.speed / upm * 3.6).toFixed(0), 'vy', (car.vel[1] / upm).toFixed(1), car.grounded ? 'G' : 'A', 'st', car.steer.toFixed(2), ev);
}
