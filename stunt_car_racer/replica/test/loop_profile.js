// Profil d'une itération de la boucle de course exécutée dans le CPU JS.
global.window = global;
const fs = require('fs'), path = require('path');
require(path.join(__dirname, '..', 'js', 'cpu68k.js'));
const pre = process.argv[2];
const cpu = new SCR.CPU(0x100000);
cpu.mem.set(fs.readFileSync(pre + '.ram'));
const r = fs.readFileSync(pre + '.regs', 'utf8').trim().split(/\s+/).map(Number);
for (let i = 0; i < 8; i++) { cpu.d[i] = r[i] >>> 0; cpu.a[i] = r[8 + i] >>> 0; }
cpu.s = (r[16] >> 13) & 1; cpu.setCCR(r[16] & 0xff); cpu.ipl = (r[16] >> 8) & 7; cpu.usp = r[18]; cpu.ssp = r[19];
cpu.pc = r[17];
const io = {};
cpu.io = (a, sz, v) => { io[a.toString(16)] = (io[a.toString(16)] || 0) + 1; return v === undefined ? 0 : undefined; };
const sp0 = cpu.a[7];
let cur = null, stats = [], writes = null;
const w8 = cpu.w8.bind(cpu);
cpu.w8 = function (a, v) { if (writes && (a < sp0 - 0x800 || a > sp0 + 16)) { const k = (a >> 12).toString(16); writes[k] = (writes[k] || 0) + 1; } w8(a, v); };
cpu.w16 = function (a, v) { this.w8(a, v >>> 8); this.w8(a + 1, v & 0xff); };
let steps = 0;
while (steps < 3e6) {
  if (cpu.pc === 0x4b0a6) cpu.mem[0x4ec20] = 0;        // attente VBL : on simule la fin de l'attente
  const op = cpu.r16(cpu.pc);
  if (cpu.a[7] === sp0 && op === 0x4eb9) {
    const t = cpu.r32(cpu.pc + 2); writes = {}; cur = { at: cpu.pc, t, n0: cpu.count, writes };
    const ret = cpu.pc + 6;
    while (cpu.pc !== ret) { if (cpu.pc === 0x4b0a6) cpu.mem[0x4ec20] = 0; cpu.step(); steps++; }
    cur.n = cpu.count - cur.n0; stats.push(cur); writes = null;
    continue;
  }
  cpu.step(); steps++;
  if (cpu.pc === 0x4aa74 && steps > 10) break;
}
for (const s of stats) console.log(s.at.toString(16), '->', s.t.toString(16), String(s.n).padStart(7), 'instr  écrit pages', Object.entries(s.writes).map(([k, v]) => k + ':' + v).join(' '));
console.log('E/S :', JSON.stringify(io).slice(0, 400));
