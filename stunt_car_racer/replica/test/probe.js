// Sonde des variables de la voiture : node test/probe.js DISQUE.st circuit
global.window = global;
const fs = require('fs'), path = require('path');
for (const f of ['scrdata', 'cpu68k', 'engine']) require(path.join(__dirname, '..', 'js', f + '.js'));
const [disk, track] = process.argv.slice(2);
const e = new SCR.Engine(new Uint8Array(fs.readFileSync(disk)));
e.boot(); e.startPractice(+(track || 0));
const hist = [];
for (let t = 0; t < 260; t++) {
  e.setInput({ up: t > 20, left: t > 150 && t < 175 });
  e.tick();
  const snap = e.cpu.mem.slice(0x10900, 0x11200);
  hist.push(snap);
  if (t % 10 === 0) console.log(t, 'pc', e.b(0x10906), 'x', e.sw(0x10ac2), 'y', e.sw(0x10ac6), 'z', e.sw(0x10aca), 'ad6', e.sw(0x10ad6), 'ad8', e.sw(0x10ad8), 'ade', e.sw(0x10ade), 'ae2', e.sw(0x10ae2), 'ae4', e.sw(0x10ae4), 'b16', e.sw(0x10b16));
}
// variables corrélées au braquage (ticks 150-175)
const ch = [];
for (let i = 0; i < 0x900; i += 2) {
  const a = hist[148][i] << 8 | hist[148][i + 1], b = hist[176][i] << 8 | hist[176][i + 1], c = hist[200][i] << 8 | hist[200][i + 1];
  if (a !== b && b === c) ch.push((0x10900 + i).toString(16) + ':' + a.toString(16) + '>' + b.toString(16));
}
console.log('changés par le virage puis stables :', ch.join(' '));
