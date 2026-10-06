/*
 * params.js — tous les réglages de la réplique, avec bornes pour le panneau de réglage.
 * Les valeurs sont modifiables en direct, sauvegardées dans le navigateur, et
 * exportables / importables en JSON.
 */
var SCR = window.SCR || (window.SCR = {});

SCR.PARAM_SPEC = [
  { group: 'track', label: 'Circuit', items: [
    ['verticalScale', 'Échelle verticale (hauteurs du jeu → monde ; 0,5 = physique d\'origine)', 0.5, 0.1, 1.5, 0.01],
    ['baseHeight', 'Hauteur du point le plus bas au-dessus du sol', 0, 0, 2000, 10],
    ['laps', 'Nombre de tours', 3, 1, 20, 1],
    ['roadWidthScale', 'Largeur de route (×)', 1.0, 0.6, 2.0, 0.05] ] },
  { group: 'world', label: 'Monde', items: [
    ['unitsPerMeter', 'Unités du jeu par mètre', 52, 20, 120, 1],
    ['gravity', 'Gravité (m/s²)', 9.81, 2, 30, 0.1] ] },
  { group: 'car', label: 'Voiture', items: [
    ['enginePower', 'Puissance moteur (accélération max, m/s²)', 11.0, 2, 30, 0.1],
    ['topSpeed', 'Vitesse de pointe (km/h)', 240, 80, 500, 5],
    ['boostPower', 'Poussée du boost (m/s²)', 7.0, 0, 30, 0.1],
    ['boostCapacity', 'Réserve de boost (secondes)', 12, 0, 60, 0.5],
    ['brake', 'Freinage (m/s²)', 14, 2, 40, 0.5],
    ['drag', 'Traînée aérodynamique', 0.0003, 0, 0.01, 0.0001],
    ['rolling', 'Résistance au roulement (m/s²)', 0.4, 0, 5, 0.05],
    ['steerRate', 'Vitesse de braquage (rad/s)', 2.2, 0.2, 8, 0.1],
    ['maxSteer', 'Braquage maximal (rad)', 0.42, 0.05, 1.2, 0.01],
    ['steerSpeedFalloff', 'Réduction du braquage avec la vitesse', 0.55, 0, 1, 0.01],
    ['wheelbase', 'Empattement (m)', 2.6, 1, 6, 0.1],
    ['grip', 'Adhérence latérale (m/s²)', 45, 2, 150, 0.5],
    ['airControl', 'Contrôle en l\'air (0..1)', 0.1, 0, 1, 0.01],
    ['suspension', 'Raideur de suspension (1/s²)', 260, 20, 2000, 10],
    ['damping', 'Amortissement (1/s)', 22, 0, 200, 1],
    ['rideHeight', 'Garde au sol (m)', 0.55, 0.1, 2, 0.01],
    ['maxClimb', 'Pente franchissable max (rapport h/l)', 1.6, 0.3, 6, 0.1] ] },
  { group: 'damage', label: 'Dégâts', items: [
    ['enabled', 'Dégâts activés (0/1)', 1, 0, 1, 1],
    ['landingThreshold', 'Vitesse verticale tolérée à l\'atterrissage (m/s)', 9, 1, 40, 0.5],
    ['landingFactor', 'Dégâts par m/s au-delà du seuil', 2.5, 0, 20, 0.1],
    ['wallHit', 'Dégâts contre une paroi', 12, 0, 100, 1],
    ['crashPenalty', 'Pénalité de chute (s)', 4, 0, 30, 0.5],
    ['maxDamage', 'Dégâts avant destruction', 100, 10, 1000, 5] ] },
  { group: 'ai', label: 'Adversaire', items: [
    ['enabled', 'Adversaire actif (0/1)', 1, 0, 1, 1],
    ['skill', 'Niveau (vitesse relative)', 0.82, 0.3, 1.3, 0.01],
    ['startGap', 'Écart au départ (m)', 0, -100, 100, 1] ] },
  { group: 'original', label: 'Mode Original (code du jeu)', items: [
    ['speed', 'Vitesse du jeu (1 = Atari ST, 50 Hz)', 1, 0.25, 3, 0.05],
    ['smooth', 'Lissage de l\'image agrandie (0/1)', 0, 0, 1, 1],
    ['view', 'Affichage : 0 = écran ST, 1 = 3D moderne, 2 = 3D + vignette ST', 0, 0, 2, 1],
    ['pitchSign', 'Signe du tangage (calibrage 3D moderne)', 1, -1, 1, 2],
    ['rollSign', 'Signe du roulis (calibrage 3D moderne)', 1, -1, 1, 2] ] },
  { group: 'physOrig', label: 'Mode Original : physique d\'origine (×)', items: [
    ['gravity', 'Gravité (× ; original 0x13D)', 1, 0.2, 3, 0.05],
    ['thrust', 'Poussée du moteur (× ; original 240, super ligue 320)', 1, 0.2, 4, 0.05],
    ['brake', 'Freinage (× ; original 240)', 1, 0, 4, 0.05],
    ['timeStep', 'Pas de temps de la simulation (× ; original 0xEE/256)', 1, 0.3, 1.07, 0.01],
    ['damping', 'Amortissement de la suspension (× ; original 0x114)', 1, 0, 3, 0.05],
    ['boostUse', 'Consommation du boost (×)', 1, 0, 4, 0.05],
    ['shockTolerance', 'Tolérance aux chocs (+ ; dégâts des réceptions)', 0, -5, 20, 1],
    ['jsPhysics', 'Physique exécutée par le code JS décompilé (0/1)', 0, 0, 1, 1],
    ['grip', 'Adhérence des pneus (× ; seulement avec la physique JS)', 1, 0.2, 4, 0.05] ] },
  { group: 'view', label: 'Affichage', items: [
    ['fov', 'Champ de vision (degrés)', 60, 30, 120, 1],
    ['eyeHeight', 'Hauteur des yeux (m)', 0.8, 0.3, 4, 0.01],
    ['drawAhead', 'Segments dessinés devant', 160, 30, 600, 5],
    ['drawBehind', 'Segments dessinés derrière', 24, 0, 200, 2],
    ['renderScale', 'Résolution interne (1 = 320×200 façon ST)', 3, 1, 6, 1],
    ['cockpit', 'Cockpit affiché (0/1)', 1, 0, 1, 1],
    ['chaseCam', 'Caméra extérieure (0/1)', 0, 0, 1, 1],
    ['stPalette', 'Couleurs façon Atari ST (0/1)', 1, 0, 1, 1] ] }
];

SCR.defaultParams = function () {
  var p = {};
  SCR.PARAM_SPEC.forEach(function (g) {
    p[g.group] = {};
    g.items.forEach(function (it) { p[g.group][it[0]] = it[2]; });
  });
  return p;
};

SCR.loadParams = function () {
  var p = SCR.defaultParams();
  try {
    var saved = JSON.parse(localStorage.getItem('scr.params') || '{}');
    Object.keys(saved).forEach(function (g) {
      if (!p[g]) return;
      Object.keys(saved[g]).forEach(function (k) { if (k in p[g]) p[g][k] = +saved[g][k]; });
    });
  } catch (e) { /* stockage indisponible : valeurs par défaut */ }
  return p;
};

SCR.saveParams = function (p) {
  try { localStorage.setItem('scr.params', JSON.stringify(p)); } catch (e) { /* ignoré */ }
};
