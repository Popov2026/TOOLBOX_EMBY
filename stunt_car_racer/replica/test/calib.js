// Vue fixe pour calibrer la caméra : node test/calib.js DISQUE.st circuit s_metres lat_m sortie.png '{"view":{"fov":50}}'
const { chromium } = require('playwright');
const path = require('path');
(async () => {
  const [disk, idx, s, lat, out, json] = process.argv.slice(2);
  const b = await chromium.launch();
  const p = await b.newPage({ viewport: { width: 1400, height: 760 } });
  p.on('pageerror', e => console.log('PAGE ERROR:', e.message));
  await p.goto('file://' + path.resolve(__dirname, '..', 'index.html'));
  await p.setInputFiles('#file', disk);
  await p.selectOption('#track', String(idx));
  await p.evaluate(([s, lat, json]) => {
    const P = SCR.params, o = JSON.parse(json || '{}');
    Object.keys(o).forEach(g => Object.assign(P[g], o[g]));
    P.ai.enabled = 0; P.view.renderScale = 2;
    const G = SCR.game; G.load(G.trackDef); G.state = 'race'; G.time = 0;
    G.player.reset(+s * P.world.unitsPerMeter, +lat);
    G.paused = true; G.draw();
  }, [s, lat, json]);
  await p.waitForTimeout(200);
  const c = await p.$('#screen'); await c.screenshot({ path: out });
  await b.close();
})();
