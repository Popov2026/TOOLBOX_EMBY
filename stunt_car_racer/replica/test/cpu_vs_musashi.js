// Compare le CPU JavaScript à Musashi : exécute une routine du jeu depuis un instantané
// (RAM + registres) pris par stemu --snap à l'entrée, puis compare avec l'instantané de sortie.
// usage : node test/cpu_vs_musashi.js PREFIXE_ENTREE PREFIXE_SORTIE ROUTINE_HEX [n]
global.window = global;
const fs = require('fs'), path = require('path');
require(path.join(__dirname, '..', 'js', 'cpu68k.js'));
const [pin, pout, routine, nmax] = process.argv.slice(2);
const IGNORE = [[0x4ec20, 4], [0x4edd0, 1], [0x4ed0e, 2], [0x4e1fc, 1], [0x6f02c, 0x80], [0x106a6, 1], [0x10668, 2]];
let ok = 0, bad = 0;
for (let k = 0; k < (+nmax || 60); k++) {
  const fi = `${pin}_${String(k).padStart(3, '0')}`, fo = `${pout}_${String(k).padStart(3, '0')}`;
  if (!fs.existsSync(fi + '.ram') || !fs.existsSync(fo + '.ram')) break;
  const cpu = new SCR.CPU(0x100000);
  cpu.mem.set(fs.readFileSync(fi + '.ram'));
  const r = fs.readFileSync(fi + '.regs', 'utf8').trim().split(/\s+/).map(Number);
  for (let i = 0; i < 8; i++) { cpu.d[i] = r[i] >>> 0; cpu.a[i] = r[8 + i] >>> 0; }
  cpu.s = (r[16] >> 13) & 1; cpu.setCCR(r[16] & 0xff); cpu.ipl = (r[16] >> 8) & 7;
  cpu.usp = r[18]; cpu.ssp = r[19];
  cpu.io = (a, sz, v) => (v === undefined ? 0 : undefined);
  const sp0 = cpu.a[7];
  cpu.call(parseInt(routine, 16));
  const exp = fs.readFileSync(fo + '.ram'), ro = fs.readFileSync(fo + '.regs', 'utf8').trim().split(/\s+/).map(Number);
  const diffs = [];
  for (let a = 0x400; a < 0x100000; a++) {
    if (cpu.mem[a] === exp[a]) continue;
    if (a >= sp0 - 0x400 && a < sp0 + 8) continue;                 // pile
    if (IGNORE.some(([b, n]) => a >= b && a < b + n)) continue;     // touché par les interruptions
    if (a >= 0x56c7c && a < 0x6697c) continue;                      // écrans
    diffs.push(a);
  }
  const regd = [];
  for (let i = 0; i < 7; i++) { if ((cpu.d[i] >>> 0) !== (ro[i] >>> 0)) regd.push('D' + i); if ((cpu.a[i] >>> 0) !== (ro[8 + i] >>> 0)) regd.push('A' + i); }
  if (diffs.length || regd.length) {
    bad++;
    console.log(`#${k} : ${diffs.length} octets différents ${diffs.slice(0, 12).map(a => a.toString(16) + ':' + cpu.mem[a].toString(16) + '/' + exp[a].toString(16)).join(' ')} regs ${regd.join(',')} (${cpu.count} instr.)`);
  } else ok++;
}
console.log(`identiques : ${ok}, différents : ${bad}`);
