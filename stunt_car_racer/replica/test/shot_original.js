// Captures du mode Original : node test/shot_original.js DISQUE.st circuit sortie_prefix
const { chromium } = require('playwright');
const path = require('path');
(async () => {
  const [disk, idx, out] = process.argv.slice(2);
  const b = await chromium.launch();
  const p = await b.newPage({ viewport: { width: 1400, height: 760 } });
  p.on('pageerror', e => console.log('PAGE ERROR:', e.message));
  p.on('console', m => { if (m.type() === 'error') console.log('console:', m.text()); });
  await p.goto('file://' + path.resolve(__dirname, '..', 'index.html'));
  await p.setInputFiles('#file', disk);
  await p.selectOption('#track', String(idx || 0));
  await p.selectOption('#mode', 'original');
  await p.waitForTimeout(1500);
  await p.screenshot({ path: out + '_0.png', clip: { x: 0, y: 0, width: 1050, height: 700 } });
  await p.waitForTimeout(3000);
  await p.screenshot({ path: out + '_1.png', clip: { x: 0, y: 0, width: 1050, height: 700 } });
  await p.keyboard.down('ArrowUp');
  await p.waitForTimeout(6000);
  await p.screenshot({ path: out + '_2.png', clip: { x: 0, y: 0, width: 1050, height: 700 } });
  await p.waitForTimeout(6000);
  await p.screenshot({ path: out + '_3.png', clip: { x: 0, y: 0, width: 1050, height: 700 } });
  console.log(await p.$eval('#status', e => e.textContent));
  await b.close();
})();
