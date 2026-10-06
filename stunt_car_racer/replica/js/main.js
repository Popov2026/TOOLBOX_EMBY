/*
 * main.js — interface : chargement de la disquette, choix du circuit, panneau de réglages.
 */
(function () {
  'use strict';
  var P = SCR.loadParams();
  var canvas = document.getElementById('screen');
  var game = new SCR.Game(canvas, P);
  var tracks = [];
  var current = -1;
  var diskBytes = null;
  var mode = 'remake';
  var orig = new SCR.OriginalMode(canvas, P);
  var modeSel = document.getElementById('mode');
  window.SCR.game = game;
  window.SCR.params = P;

  /* circuit de démonstration (création originale, utilisable sans la disquette) */
  function demoTracks() {
    var T = SCR.Track.fromCommands;
    return [
      T('DÉMO : OVALE À BOSSES', [
        { len: 6144, rise: 0 }, { len: 4096, rise: 1600, curve: 'smooth' }, { len: 2048, rise: -1600, curve: 'smooth' },
        { len: 6144, turn: 180 }, { len: 4096 }, { len: 1536, rise: 900 }, { len: 1024, gap: -900 }, { len: 3584 },
        { len: 6144, turn: 180 }
      ], { x: 4096, z: 4096 }),
      T('DÉMO : HUIT RELEVÉ', [
        { len: 4096 }, { len: 9000, turn: 270, bank: 220, rise: 2400 }, { len: 4096, bank: 0, rise: -2400 },
        { len: 9000, turn: -270, bank: -220 }, { len: 2900, bank: 0 }
      ], { x: 8192, z: 4096 })
    ];
  }

  var sel = document.getElementById('track');
  function refreshTrackList() {
    sel.innerHTML = '';
    tracks.forEach(function (t, i) {
      var o = document.createElement('option'); o.value = i; o.textContent = (t.index !== undefined ? (t.index + 1) + '. ' : '') + t.name;
      sel.appendChild(o);
    });
    if (current >= 0 && current < tracks.length) sel.value = current;
  }

  function setTracks(list, keepDemo) {
    tracks = (keepDemo ? [] : []).concat(list, demoTracks());
    current = 0;
    refreshTrackList();
    game.load(tracks[0]);
  }

  function status(t, err) {
    var s = document.getElementById('status');
    s.textContent = t; s.className = err ? 'err' : '';
  }

  function loadBytes(bytes, name) {
    try {
      var list;
      if (bytes[0] === 0x7b) {                         // JSON (scr_tool.py tracks)
        list = JSON.parse(new TextDecoder().decode(bytes)).tracks;
      } else {
        list = SCR.data.tracksFromFile(bytes);
        diskBytes = bytes;                             // pour le mode Original
        try { localStorage.setItem('scr.disk', toB64(bytes)); } catch (e) { /* quota */ }
      }
      setTracks(list);
      status('Circuits originaux chargés depuis ' + name + ' (' + list.length + ')');
      try { localStorage.setItem('scr.tracks', JSON.stringify(list)); } catch (e) { /* trop gros ou indisponible */ }
    } catch (e) {
      status('Erreur : ' + e.message, true);
    }
  }

  document.getElementById('file').addEventListener('change', function (e) {
    var f = e.target.files[0];
    if (!f) return;
    f.arrayBuffer().then(function (b) { loadBytes(new Uint8Array(b), f.name); });
  });
  document.body.addEventListener('dragover', function (e) { e.preventDefault(); });
  document.body.addEventListener('drop', function (e) {
    e.preventDefault();
    var f = e.dataTransfer.files[0];
    if (f) f.arrayBuffer().then(function (b) { loadBytes(new Uint8Array(b), f.name); });
  });

  function toB64(u8) { var s = ''; for (var i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000)); return btoa(s); }
  function fromB64(b) { var s = atob(b), u = new Uint8Array(s.length); for (var i = 0; i < s.length; i++) u[i] = s.charCodeAt(i); return u; }

  function setMode(m) {
    mode = m; modeSel.value = m;
    game.active = m === 'remake';
    if (m === 'remake') orig.stop();
  }
  function startOriginal(track) {
    if (!diskBytes) { status('Mode Original : chargez d\'abord votre image disque (.st ou GAME.PUT)', true); return; }
    try {
      setMode('original');
      orig.start(diskBytes, track);
      status(track === null ? 'Jeu original : écran titre' : 'Jeu original : entraînement sur ' + SCR.data.NAMES[track]);
    } catch (e) { status('Mode Original : ' + e.message, true); }
  }
  modeSel.addEventListener('change', function () {
    if (modeSel.value === 'original') {
      var t = tracks[current];
      startOriginal(t && t.index !== undefined ? t.index : 0);
    } else setMode('remake');
    modeSel.blur();
  });

  sel.addEventListener('change', function () {
    current = +sel.value; sel.blur();
    var t = tracks[current];
    if (mode === 'original' && t && t.index !== undefined) startOriginal(t.index);
    else { setMode('remake'); game.load(t); }
  });
  document.getElementById('start').addEventListener('click', function () {
    this.blur();
    var t = tracks[current];
    if (mode === 'original') { if (t && t.index !== undefined) startOriginal(t.index); return; }
    if (game.track) { game.load(t); game.startRace(); }
  });
  document.getElementById('full').addEventListener('click', function () { this.blur(); startOriginal(null); });

  /* ------------------------------------------------------------ réglages */
  var panel = document.getElementById('params');
  var inputs = [];
  SCR.PARAM_SPEC.forEach(function (g) {
    var fs = document.createElement('details');
    if (g.group === 'car') fs.open = true;
    var sm = document.createElement('summary'); sm.textContent = g.label; fs.appendChild(sm);
    g.items.forEach(function (it) {
      var row = document.createElement('label'); row.className = 'row';
      var span = document.createElement('span'); span.textContent = it[1];
      var inp = document.createElement('input'); inp.type = 'range'; inp.min = it[3]; inp.max = it[4]; inp.step = it[5];
      var num = document.createElement('input'); num.type = 'number'; num.min = it[3]; num.max = it[4]; num.step = it[5];
      function set(v) {
        P[g.group][it[0]] = +v; inp.value = v; num.value = v; SCR.saveParams(P);
        if (g.group === 'track' && game.trackDef && it[0] !== 'laps') game.load(game.trackDef);
      }
      inp.addEventListener('input', function () { set(inp.value); });
      num.addEventListener('change', function () { set(num.value); });
      inputs.push(function () { inp.value = P[g.group][it[0]]; num.value = P[g.group][it[0]]; });
      row.appendChild(span); row.appendChild(inp); row.appendChild(num);
      fs.appendChild(row);
    });
    panel.appendChild(fs);
  });
  function syncInputs() { inputs.forEach(function (f) { f(); }); }
  syncInputs();

  document.getElementById('reset').addEventListener('click', function () {
    var d = SCR.defaultParams();
    Object.keys(d).forEach(function (g) { Object.keys(d[g]).forEach(function (k) { P[g][k] = d[g][k]; }); });
    SCR.saveParams(P); syncInputs(); if (game.trackDef) game.load(game.trackDef);
  });
  document.getElementById('export').addEventListener('click', function () {
    var a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([JSON.stringify(P, null, 2)], { type: 'application/json' }));
    a.download = 'stunt-car-racer-reglages.json'; a.click();
  });
  document.getElementById('import').addEventListener('change', function (e) {
    var f = e.target.files[0]; if (!f) return;
    f.text().then(function (t) {
      try {
        var j = JSON.parse(t);
        Object.keys(j).forEach(function (g) { if (P[g]) Object.keys(j[g]).forEach(function (k) { if (k in P[g]) P[g][k] = +j[g][k]; }); });
        SCR.saveParams(P); syncInputs(); if (game.trackDef) game.load(game.trackDef);
        status('Réglages importés');
      } catch (err) { status('Fichier de réglages invalide', true); }
    });
  });

  /* démarrage : circuits mémorisés ou démo */
  var saved = null;
  try { saved = JSON.parse(localStorage.getItem('scr.tracks') || 'null'); } catch (e) { saved = null; }
  try { var d = localStorage.getItem('scr.disk'); if (d) diskBytes = fromB64(d); } catch (e) { diskBytes = null; }
  if (saved && saved.length) { setTracks(saved); status('Circuits originaux (mémorisés dans ce navigateur)'); }
  else { setTracks([]); status('Aucune disquette chargée : circuits de démonstration. Glissez votre image .st ici.'); }
  function loop(t) {
    if (mode === 'original') orig.frame(t); else game.frame(t);
    requestAnimationFrame(loop);
  }
  requestAnimationFrame(loop);
})();
