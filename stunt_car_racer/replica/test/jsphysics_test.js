// Partie complète : physique d'origine (CPU 68000) contre physique JS décompilée.
// usage : node test/jsphysics_test.js DISQUE.st [circuit] [ticks]
global.window = global;
const fs = require('fs'), path = require('path');
for (const f of ['scrdata', 'cpu68k', 'engine', 'physics_orig']) require(path.join(__dirname, '..', 'js', f + '.js'));
const [disk, track, ticksS] = process.argv.slice(2);
const bytes = new Uint8Array(fs.readFileSync(disk));
const mk = js => { const e = new SCR.Engine(bytes); e.boot(); e.startPractice(+(track || 0)); if (js) e.useJsPhysics(true); return e; };
const A = mk(false), B = mk(true);
const N = +(ticksS || 400), VAR = [0x10900, 0x11200];
let first = -1;
for (let t = 0; t < N; t++) {
  const inp = { up: t > 85 && t % 97 < 80, down: t % 97 >= 85, left: (t % 60) < 12 && t > 120, right: (t % 60) >= 30 && (t % 60) < 40, fire: t % 50 < 6 };
  A.setInput(inp); B.setInput(inp); A.tick(); B.tick();
  let d = 0, ex = '';
  for (let a = 0x10ac2; a < 0x10ae0; a++) if (A.cpu.mem[a] !== B.cpu.mem[a]) { d++; if (!ex) ex = a.toString(16); }
  if (d && first < 0) { first = t; console.log('divergence état voiture au tick', t, ex); }
}
const p = e => [e.l(0x10ac2) / 65536, e.l(0x10ac6) / 65536, e.l(0x10aca) / 65536].map(v => v.toFixed(2)).join(', ');
console.log('fin  CPU :', p(A), '  JS :', p(B), ' pièce', A.b(0x10906), B.b(0x10906));
console.log(first < 0 ? 'IDENTIQUE sur ' + N + ' ticks' : 'premier écart au tick ' + first);
