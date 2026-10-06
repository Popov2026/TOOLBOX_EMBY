// Effet des réglages de la physique d'origine : node test/tuning_test.js DISQUE.st
global.window = global;
const fs = require('fs'), path = require('path');
for (const f of ['scrdata', 'cpu68k', 'engine']) require(path.join(__dirname, '..', 'js', f + '.js'));
const disk = new Uint8Array(fs.readFileSync(process.argv[2]));
function run(tuning) {
  const e = new SCR.Engine(disk); e.tuning = tuning;
  e.boot(); e.startPractice(0);
  let ymin = 1e9, xs = [];
  for (let t = 0; t < 160; t++) { e.setInput({ up: t > 85 }); e.tick(); if (t < 85) ymin = Math.min(ymin, e.l(0x10ac6) / 65536); if (t % 25 === 0) xs.push(Math.round(e.l(0x10ac2) / 65536)); }
  return 'x ' + xs.join(',') + '  ymin(grue) ' + ymin.toFixed(1) + '  dégâts ' + [0x10939, 0x1093a, 0x1093b].map(a => e.b(a)).join('/');
}
for (const [n, t] of [['original', {}], ['poussée ×2', { thrust: 2 }], ['gravité ×2', { gravity: 2 }], ['pas ×0.5', { timeStep: 0.5 }]])
  console.log(n.padEnd(12), run(t));
