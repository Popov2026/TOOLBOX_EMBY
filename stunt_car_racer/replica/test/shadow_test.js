// Validation « en ombre » des routines de physique portées en JS (js/physics_orig.js) :
// l'original s'exécute sur le CPU 68000 ; à chaque appel, le port est rejoué sur une copie
// de la mémoire d'entrée et les deux mémoires de sortie sont comparées.
// usage : node test/shadow_test.js DISQUE.st [circuit] [ticks]
global.window = global;
const fs = require('fs'), path = require('path');
for (const f of ['scrdata', 'cpu68k', 'engine', 'physics_orig']) require(path.join(__dirname, '..', 'js', f + '.js'));
const [disk, track, ticksS] = process.argv.slice(2);
const e = new SCR.Engine(new Uint8Array(fs.readFileSync(disk)));
e.boot(); e.startPractice(+(track || 0));
const c = e.cpu, R = SCR.OrigPhysics.ROUTINES, K = SCR.OrigPhysics.constants(c.mem);
const LO = 0x10000, HI = 0x74000;
const stats = {}; let pending = [];
const step0 = c.step.bind(c);
c.step = function () {
  const fn = R[this.pc];
  if (fn) pending.push({ addr: this.pc, ret: this.r32(this.a[7]), sp: this.a[7], before: this.mem.slice(LO, HI), fn });
  step0();
  for (let i = pending.length - 1; i >= 0; i--) {
    const p = pending[i];
    if (this.pc === p.ret && this.a[7] === ((p.sp + 4) >>> 0)) {
      pending.splice(i, 1);
      const copy = new Uint8Array(this.mem.length); copy.set(p.before, LO);
      p.fn(new SCR.OrigPhysics.Mem(copy), K);
      const st = stats[p.addr] || (stats[p.addr] = { calls: 0, bad: 0, ex: '' });
      st.calls++;
      for (let a = LO; a < HI; a++) if (copy[a] !== this.mem[a] && !(a >= p.sp - 0x100 && a < p.sp + 8)) {
        st.bad++; if (!st.ex) st.ex = '$' + a.toString(16) + ' port=' + copy[a].toString(16) + ' orig=' + this.mem[a].toString(16); break;
      }
    }
  }
};
const N = +(ticksS || 300);
for (let t = 0; t < N; t++) {
  e.setInput({ up: t > 85 && t % 97 < 80, down: t % 97 >= 85, left: (t % 60) < 12 && t > 120, right: (t % 60) >= 30 && (t % 60) < 40, fire: t % 50 < 6 });
  e.tick();
}
let ok = true;
for (const a of Object.keys(R)) {
  const st = stats[a] || { calls: 0, bad: 0 };
  if (st.bad || !st.calls) ok = false;
  console.log('$' + (+a).toString(16).padEnd(6), 'appels', String(st.calls).padStart(5), 'différences', st.bad, st.ex || '');
}
console.log(ok ? 'PORT EXACT' : 'ÉCARTS');
