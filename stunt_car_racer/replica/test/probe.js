// Sonde de l'état de la voiture dans le moteur fidèle : node test/probe.js DISQUE.st [circuit]
global.window = global;
const fs = require('fs'), path = require('path');
for (const f of ['scrdata', 'cpu68k', 'engine']) require(path.join(__dirname, '..', 'js', f + '.js'));
const e = new SCR.Engine(new Uint8Array(fs.readFileSync(process.argv[2])));
e.boot(); e.startPractice(+(process.argv[3] || 0));
const deg = a => (a / 65536 * 360).toFixed(1);
let px, pz;
for (let t = 0; t < 140; t++) {
  e.setInput({ up: t > 20 && t < 100, left: t >= 110 && t < 125 });
  e.tick();
  const x = e.l(0x10ac2) / 65536, z = e.l(0x10aca) / 65536;
  if (t >= 95 && t % 3 === 0) console.log(t, 'cap(vitesse)', (Math.atan2(x - px, z - pz) * 180 / Math.PI).toFixed(1),
    'lacet', deg(e.w(0x10ad0)), 'tangage', deg(e.w(0x10ace)), 'roulis', deg(e.w(0x10ad2)), 'y', (e.l(0x10ac6) / 65536).toFixed(2));
  px = x; pz = z;
}
