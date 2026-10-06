// Captures d'écran de la réplique dans Chromium headless.
// usage : node test/shot.js DISQUE.st indexCircuit sortie_prefix [secondes_de_conduite] [chase]
const { chromium } = require('playwright');
const path = require('path');
(async () => {
  const [disk, idx, out, secs, chase] = process.argv.slice(2);
  const b = await chromium.launch({ executablePath: '/opt/pw-browsers/chromium-1194/chrome-linux/chrome' }).catch(() => chromium.launch());
  const p = await b.newPage({ viewport: { width: 1400, height: 760 } });
  p.on('console', m => console.log('console:', m.text()));
  p.on('pageerror', e => console.log('PAGE ERROR:', e.message));
  await p.goto('file://' + path.resolve(__dirname, '..', 'index.html'));
  if (disk) await p.setInputFiles('#file', disk);
  await p.selectOption('#track', String(idx || 0));
  await p.waitForTimeout(300);
  await p.screenshot({ path: out + '_map.png', clip: { x: 0, y: 0, width: 1050, height: 700 } });
  if (chase) await p.evaluate(() => { SCR.params.view.chaseCam = 1; });
  await p.evaluate(() => { SCR.params.ai.startGap = 25; SCR.game.load(SCR.game.trackDef); SCR.game.startRace(); });
  await p.waitForTimeout(3600);
  await p.screenshot({ path: out + '_0.png', clip: { x: 0, y: 0, width: 1050, height: 700 } });
  await p.keyboard.down('ArrowUp');
  const n = +(secs || 6);
  for (let i = 1; i <= 3; i++) {
    await p.waitForTimeout(n * 1000 / 3);
    await p.screenshot({ path: out + '_' + i + '.png', clip: { x: 0, y: 0, width: 1050, height: 700 } });
  }
  const st = await p.evaluate(() => { const c = SCR.game.player; return { seg: c.seg, v: c.speed, dmg: c.damage, msg: SCR.game.message }; });
  console.log(JSON.stringify(st));
  await b.close();
})();
