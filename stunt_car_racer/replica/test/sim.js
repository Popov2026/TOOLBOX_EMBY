// Simulation sans affichage : un pilote automatique parcourt chaque circuit.
// usage : node test/sim.js DISQUE.st [secondes] [skill]
global.window = global; global.localStorage = { getItem() { return null; }, setItem() {} };
const fs = require('fs'), path = require('path');
for (const f of ['scrdata', 'params', 'track', 'physics']) require(path.join(__dirname, '..', 'js', f + '.js'));
const P = SCR.defaultParams();
const secs = +(process.argv[3] || 120);
if (process.argv[4]) P.ai.skill = +process.argv[4];
let defs = [];
if (process.argv[2]) defs = SCR.data.tracksFromFile(new Uint8Array(fs.readFileSync(process.argv[2])));
const upm = P.world.unitsPerMeter;
for (const def of defs) {
  const t = new SCR.Track(def, P); t.baseHeightWorld = P.track.baseHeight;
  const car = new SCR.Car(t, P), pilot = new SCR.AutoPilot(car, P);
  let lapT = [], crashes = [], laps = 0, prev = 0, half = false, maxV = 0, dt = 1 / 120, airMax = 0;
  for (let i = 0; i < secs / dt; i++) {
    pilot.update(); car.step(dt);
    for (const e of car.events) if (e.type === 'crash') crashes.push((i * dt).toFixed(1) + 's@' + Math.round(t.progress(car.seg, 0) / t.length * 100) + '%:' + e.value);
    car.events.length = 0;
    const p = t.progress(car.seg, 0);
    if (p > t.length * 0.4 && p < t.length * 0.6) half = true;
    if (half && prev > t.length * 0.8 && p < t.length * 0.2) { laps++; half = false; lapT.push((i * dt).toFixed(0)); }
    prev = p; maxV = Math.max(maxV, car.speed); airMax = Math.max(airMax, car.airTime);
  }
  console.log(def.name.padEnd(16), 'long', (t.length / upm).toFixed(0) + 'm', 'tours', laps, '[' + lapT.join(',') + ']', 'vmax', (maxV / upm * 3.6).toFixed(0) + 'km/h',
    'vol max', airMax.toFixed(1) + 's', 'dégâts', car.damage.toFixed(0), 'chutes', crashes.length, crashes.slice(0, 4).join(' '));
}
