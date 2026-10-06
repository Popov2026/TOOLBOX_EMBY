// Part d'un instantané de l'émulateur (haut de boucle), exécute N ticks dans le moteur JS
// (rendu court-circuité) et compare l'état de la voiture avec les instantanés suivants.
global.window = global;
const fs = require('fs'), path = require('path');
for (const f of ['scrdata', 'cpu68k', 'engine']) require(path.join(__dirname, '..', 'js', f + '.js'));
const [pre, disk, k0s, ns] = process.argv.slice(2);
const k0 = +(k0s || 0), N = +(ns || 100);
const name = k => `${pre}_${String(k).padStart(3, '0')}`;
const e = new SCR.Engine(new Uint8Array(fs.readFileSync(disk)), { renderOriginal: !!process.env.RENDER });
const c = e.cpu;
c.mem.set(fs.readFileSync(name(k0) + '.ram'));
const r = fs.readFileSync(name(k0) + '.regs', 'utf8').trim().split(/\s+/).map(Number);
for (let i = 0; i < 8; i++) { c.d[i] = r[i] >>> 0; c.a[i] = r[8 + i] >>> 0; }
// l'instantané vient de stemu : ses « ROM » TOS sont en $FC00xx -> on les redirige vers le RTE du moteur
for (let v = 2; v < 256; v++) { const t = c.r32(v * 4); if (t >= 0xfc0000 && t < 0xff0000) c.w32(v * 4, 0xffe00); }
for (const a of [0x106a2, 0x10698]) { const t = c.r32(a); if (t >= 0xfc0000 && t < 0xff0000) c.w32(a, 0xffe00); }
for (let i = 0; i < 16; i++) { c.mem[0xffe00 + 2 * i] = 0x4e; c.mem[0xffe01 + 2 * i] = 0x73; }
c.s = (r[16] >> 13) & 1; c.setCCR(r[16] & 0xff); c.ipl = (r[16] >> 8) & 7; c.usp = r[18]; c.ssp = r[19]; c.pc = r[17];
// variables comparées : $10900-$11200 sauf temporaires du rendu
const TMP = new Set([0x10903, 0x10904, 0x10905, 0x10963, 0x1096f, 0x109e0, 0x109e1, 0x109e2, 0x109e3, 0x109e4, 0x109e5, 0x10a76, 0x10a77, 0x10a7a, 0x10a7b, 0x10aba, 0x10abb, 0x109de, 0x109df, 0x10a1e, 0x10a1f]);
let firstBad = -1;
for (let k = k0 + 1; k <= k0 + N && fs.existsSync(name(k) + '.ram'); k++) {
  // l'émulateur a reçu le joystick par l'ACIA : on recopie l'octet d'entrée de l'instantané
  const exp = fs.readFileSync(name(k) + '.ram');
  c.mem[0x106a6] = fs.readFileSync(name(k - 1) + '.ram')[0x106a6];
  const prev = fs.readFileSync(name(k - 1) + '.ram');
  const nv = ((exp[0x462] << 24 | exp[0x463] << 16 | exp[0x464] << 8 | exp[0x465]) - (prev[0x462] << 24 | prev[0x463] << 16 | prev[0x464] << 8 | prev[0x465])) | 0;
  e.tick(nv);
  const d = [];
  for (let a = 0x10900; a < 0x11200; a++) if (!TMP.has(a) && c.mem[a] !== exp[a]) d.push(a);
  if (d.length) {
    if (firstBad < 0) firstBad = k;
    console.log('tick', k, d.length, 'différences', d.slice(0, 16).map(a => a.toString(16) + ':' + c.mem[a].toString(16) + '/' + exp[a].toString(16)).join(' '));
    if (k - firstBad > 3) break;
  }
}
console.log(firstBad < 0 ? 'IDENTIQUE sur toute la séquence' : 'première divergence au tick ' + firstBad);
