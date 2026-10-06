// Démarre le moteur fidèle, lance l'entraînement et roule joystick en avant.
// usage : node test/engine_test.js DISQUE.st [circuit] [ticks]
global.window = global;
const fs = require('fs'), path = require('path');
for (const f of ['scrdata', 'cpu68k', 'engine']) require(path.join(__dirname, '..', 'js', f + '.js'));
const [disk, track, ticks] = process.argv.slice(2);
const e = new SCR.Engine(new Uint8Array(fs.readFileSync(disk)));
e.log = m => console.log('  ' + m);
let t0 = Date.now();
const nb = e.boot();
console.log('boot :', nb, 'instr,', Date.now() - t0, 'ms');
t0 = Date.now();
const ns = e.startPractice(+(track || 0));
console.log('départ :', ns, 'instr,', Date.now() - t0, 'ms', 'pièces', e.b(0x11114), 'départ', e.b(0x11116));
e.setInput({ up: 1 });
for (let i = 0; i < +(ticks || 40); i++) {
  const n = e.tick();
  if (i % 5 === 0) console.log('tick', i, n, 'instr  pièce', e.b(0x10906), 'section?', e.w(0x10a1e), 'pos', e.sw(0x10ac2), e.sw(0x10ac6), e.sw(0x10aca));
}
