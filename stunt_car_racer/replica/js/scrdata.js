/*
 * scrdata.js — lecture des données originales de Stunt Car Racer (Atari ST) depuis
 * l'image disque de l'utilisateur : FAT12, décompression JEK Packer (ByteKiller),
 * décodage des 8 circuits.  Port JavaScript de tools/scr_tool.py (validé octet par
 * octet contre la mémoire du jeu tournant dans l'émulateur).
 */
var SCR = window.SCR || (window.SCR = {});

SCR.data = (function () {
  'use strict';

  function u16le(b, o) { return b[o] | (b[o + 1] << 8); }
  function u32be(b, o) { return ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0; }

  /* ---------------------------------------------------------------- FAT12 */
  function fatList(img) {
    var bps = u16le(img, 11), spc = img[13] || 1, res = u16le(img, 14), nfat = img[16];
    var ndir = u16le(img, 17), spf = u16le(img, 22);
    var fat = img.subarray(res * bps, (res + spf) * bps);
    var root = (res + nfat * spf) * bps, data = root + ndir * 32, csize = spc * bps;
    function nxt(n) { var o = (n * 3) >> 1, v = fat[o] | (fat[o + 1] << 8); return (n & 1) ? v >> 4 : v & 0xfff; }
    function chain(cl) {
      var parts = [], len = 0, guard = 0;
      while (cl >= 2 && cl < 0xff0 && guard++ < 4096) {
        var s = img.subarray(data + (cl - 2) * csize, data + (cl - 1) * csize);
        parts.push(s); len += s.length; cl = nxt(cl);
      }
      var out = new Uint8Array(len), p = 0;
      parts.forEach(function (s) { out.set(s, p); p += s.length; });
      return out;
    }
    var files = {};
    function walk(buf, path) {
      for (var i = 0; i + 32 <= buf.length; i += 32) {
        var f = buf[i];
        if (f === 0) break;
        if (f === 0xe5 || f === 0x2e) continue;
        var name = String.fromCharCode.apply(null, buf.subarray(i, i + 8)).replace(/ +$/, '');
        var ext = String.fromCharCode.apply(null, buf.subarray(i + 8, i + 11)).replace(/ +$/, '');
        if (ext) name += '.' + ext;
        var attr = buf[i + 11], cl = u16le(buf, i + 26);
        var size = (buf[i + 28] | (buf[i + 29] << 8) | (buf[i + 30] << 16) | (buf[i + 31] << 24)) >>> 0;
        if (attr & 0x08) continue;
        if (attr & 0x10) walk(chain(cl), path + name + '/');
        else files[path + name] = chain(cl).subarray(0, size);
      }
    }
    walk(img.subarray(root, data), '');
    return files;
  }

  /* ---------------------------------------------------- ByteKiller (JEK Packer) */
  function bytekiller(buf, end) {
    var p = end;
    function rl() { p -= 4; return u32be(buf, p); }
    var length = rl(), crc = rl(), d0 = rl();
    crc = (crc ^ d0) >>> 0;
    var out = new Uint8Array(length), q = length;
    function bit() {
      var c = d0 & 1;
      d0 = d0 >>> 1;
      if (d0 === 0) {
        var n = rl(); crc = (crc ^ n) >>> 0;
        c = n & 1; d0 = ((n >>> 1) | 0x80000000) >>> 0;
      }
      return c;
    }
    function bits(n) { var v = 0; while (n--) v = (v << 1) | bit(); return v; }
    var i, cnt, off, t;
    while (q > 0) {
      if (!bit()) {
        if (!bit()) { cnt = bits(3) + 1; for (i = 0; i < cnt; i++) out[--q] = bits(8); }
        else { off = bits(8); for (i = 0; i < 2; i++) { q--; out[q] = out[q + off]; } }
      } else {
        t = bits(2);
        if (t === 3) { cnt = bits(8) + 9; for (i = 0; i < cnt; i++) out[--q] = bits(8); }
        else {
          if (t === 2) { cnt = bits(8) + 1; off = bits(12); }
          else { cnt = t + 3; off = bits(9 + t); }
          for (i = 0; i < cnt; i++) { q--; out[q] = out[q + off]; }
        }
      }
      if (q < 0) throw new Error('flux compressé invalide');
    }
    return { data: out, ok: crc === 0 };
  }

  function unpackPrg(d) { return bytekiller(d, 0x1c + u32be(d, 2) + u32be(d, 6)); }

  function gameProgram(img) {
    var files = fatList(img), key = null;
    Object.keys(files).forEach(function (k) { if (/GAME\.PUT$/i.test(k)) key = k; });
    if (!key) throw new Error('GAME.PUT introuvable sur cette disquette');
    var r = unpackPrg(files[key]);
    if (!r.ok) throw new Error('GAME.PUT : décompression invalide');
    var out = r.data, tlen = u32be(out, 2);
    for (var i = Math.max(0, 0x1c + tlen - 0x40); i < out.length - 1; i++)
      if (out[i] === 0x60 && out[i + 1] === 0x1a) return out.subarray(i);
    throw new Error('programme du jeu introuvable dans GAME.PUT');
  }

  /* ------------------------------------------------------------- circuits */
  var TEXT_BASE = 0x10100, BLOB = 0x13670, TRACK_TABLE = 0x13790, SHAPE_TABLE = 0x13670,
      PROFILE_TABLE = 0x13690, SPECIAL_BBA = 0x48802, SPECIAL_C1E = 0x48804, CELL = 0x800;
  var NAMES = ['LITTLE RAMP', 'STEPPING STONES', 'HUMP BACK', 'BIG RAMP',
               'SKI JUMP', 'DRAW BRIDGE', 'HIGH JUMP', 'ROLLER COASTER'];

  function Mem(prg) { this.img = prg; this.base = TEXT_BASE - 0x1c; }
  Mem.prototype.b = function (a) { return this.img[a - this.base]; };
  Mem.prototype.le = function (a) { return this.b(a) | (this.b(a + 1) << 8); };
  function p6502(a) { return BLOB + (a - 0xb100); }
  function s16(v) { return v & 0x8000 ? v - 0x10000 : v; }

  function profile(m, ptr, neg, i) {
    var a = p6502(ptr), x;
    if (neg) return ((m.b(a + 2 * i) & 0x7f) << 8) | m.b(a + 2 * i + 1);
    x = m.b(a + i);
    return ((x & 0xf) << 8) | ((x << 1) & 0xe0);
  }
  function profileFlag(m, ptr, neg, i) { return (m.b(p6502(ptr) + (neg ? 2 * i : i)) & 0x80) !== 0; }

  function shapePoints(m, typ) {
    var a = p6502(m.le(SHAPE_TABLE + 2 * (typ & 0xf))), o = m.b(a), n = m.b(a + o), base = a + o + 7, pts = [];
    for (var i = 0; i < n; i++) pts.push([s16(m.le(base + 4 * i)), s16(m.le(base + 4 * i + 2))]);
    return pts;
  }

  /* port de la routine $48390 */
  function decodeTrack(m, track) {
    var ptr = p6502(m.le(TRACK_TABLE + 2 * track)), pos = 0;
    function fetch() { return m.b(ptr + pos++); }
    var hdr = [fetch(), fetch(), fetch(), fetch()];
    var lo = fetch(), hi = fetch(), hl = (hi << 8) | lo, hr = hl;
    var c6 = 0, dist = 0, repeat = 0, prevType = 0, prevCoord = 0, pieces = [];
    while (pieces.length < hdr[0]) {
      var t, ce6, dirb, c, bba, v;
      if (repeat) {
        repeat--; t = prevType; ce6 = dirb = t;
        if (t & 0x10) dirb ^= 0xc0;
        var step = (dirb & 0x40) ? 1 : 0x10;
        c = ((dirb & 0x80) ? prevCoord - step : prevCoord + step) & 0xff;
      } else {
        t = fetch();
        if ((t & 0xf) === 0xf) { repeat = t >> 4; continue; }
        ce6 = dirb = prevType = t; c = fetch();
      }
      prevCoord = c;
      var b905 = (c6 & 2) ? 0x80 : 0;
      if ((dirb & 0xf) >= 12) {
        ce6 &= 0xf0; bba = m.b(SPECIAL_BBA + (dirb & 0xf)); v = m.b(SPECIAL_C1E + (dirb & 0xf));
      } else {
        bba = fetch(); v = (dirb & 0x20) ? bba : fetch();
      }
      var c1e = (v & 0x7f) | b905;
      var pL = m.le(PROFILE_TABLE + ((bba << 1) & 0xff)), pR = m.le(PROFILE_TABLE + ((c1e << 1) & 0xff));
      var neg = (bba & 0x80) !== 0;
      c6 = (c1e & 0x80) ? 2 : 0;
      var shape = p6502(m.le(SHAPE_TABLE + 2 * (ce6 & 0xf)));
      var nsec = m.b(shape + m.b(shape)), segs = ((nsec >> 1) - 1) & 0xff;
      var startL = (hl - profile(m, pL, neg, 0)) & 0xffff; hl = (startL + profile(m, pL, neg, segs)) & 0xffff;
      var startR = (hr - profile(m, pR, neg, 0)) & 0xffff; hr = (startR + profile(m, pR, neg, segs)) & 0xffff;
      pieces.push({ coord: c, x: c & 0xf, z: c >> 4, type: ce6, bba: bba, c1e: c1e, dist: (dist << 5) & 0xffff,
                    hL: startL, hR: startR, segs: segs, profL: pL, profR: pR, neg: neg });
      c6 = (c6 + nsec - 2) & 2;
      dist = (dist + segs) & 0xffff;
    }
    var tail = []; for (var i = 0; i < 6; i++) tail.push(m.b(ptr + pos + i));
    return { header: hdr, pieces: pieces, tail: tail };
  }

  var ROT = [function (x, z) { return [x, z]; }, function (x, z) { return [z, CELL - x]; },
             function (x, z) { return [CELL - x, CELL - z]; }, function (x, z) { return [CELL - z, x]; }];

  function buildTrack(m, track) {
    var t = decodeTrack(m, track), pieces = [];
    t.pieces.forEach(function (p) {
      var typ = p.type, pts = shapePoints(m, typ), secs = [], k;
      for (k = 0; k + 1 < pts.length; k += 2) secs.push([pts[k], pts[k + 1]]);
      var hs = [], fl = [];
      for (k = 0; k < secs.length; k++) {
        hs.push([(p.hL + profile(m, p.profL, p.neg, k)) & 0xffff, (p.hR + profile(m, p.profR, p.neg, k)) & 0xffff]);
        fl.push((profileFlag(m, p.profL, p.neg, k) ? 1 : 0) | (profileFlag(m, p.profR, p.neg, k) ? 2 : 0));
      }
      var rot = ROT[(typ >> 6) & 3];
      if (typ & 0x10) secs = secs.reverse().map(function (s) { return [s[1], s[0]]; });   // parcours inverse : gauche/droite permutés
      var out = secs.map(function (s, k) {
        var l = rot(s[0][0], s[0][1]), r = rot(s[1][0], s[1][1]);
        return [l[0] + p.x * CELL, hs[k][0], l[1] + p.z * CELL, r[0] + p.x * CELL, hs[k][1], r[1] + p.z * CELL, fl[k]];
      });
      pieces.push({ type: typ, shape: typ & 0xf, reversed: !!(typ & 0x10), dir: (typ >> 6) & 3, cell: [p.x, p.z],
                    dist: p.dist, profile_left: p.bba, profile_right: p.c1e, sections: out });
    });
    return { name: NAMES[track], index: track, header: t.header, tail: t.tail, cell_size: CELL, pieces: pieces };
  }

  /* Point d'entrée : Uint8Array d'une image .st (ou de GAME.PUT, ou du programme déjà extrait) */
  function tracksFromFile(bytes) {
    var prg;
    if (bytes[0] === 0x60 && bytes[1] === 0x1a) {
      // exécutable : GAME.PUT compressé, ou programme interne déjà décompressé
      var r = null;
      try { r = unpackPrg(bytes); } catch (e) { r = null; }
      if (r && r.ok) {
        var out = r.data, tlen = u32be(out, 2);
        for (var i = Math.max(0, 0x1c + tlen - 0x40); i < out.length - 1; i++)
          if (out[i] === 0x60 && out[i + 1] === 0x1a) { prg = out.subarray(i); break; }
      } else prg = bytes;
    } else prg = gameProgram(bytes);
    var m = new Mem(prg), tracks = [];
    // contrôle : les noms des circuits doivent être présents à l'adresse attendue
    var sig = ''; for (var k = 0; k < 11; k++) sig += String.fromCharCode(m.b(0x13498 + k));
    if (sig !== 'LITTLE RAMP') throw new Error('version du jeu non reconnue (signature absente)');
    for (var n = 0; n < 8; n++) tracks.push(buildTrack(m, n));
    return tracks;
  }

  /* programme interne (jeu) depuis une image .st, GAME.PUT, ou le programme déjà extrait */
  function innerProgram(bytes) {
    if (bytes[0] === 0x60 && bytes[1] === 0x1a) {
      var r = null;
      try { r = unpackPrg(bytes); } catch (e) { r = null; }
      if (r && r.ok) {
        var out = r.data, tlen = u32be(out, 2);
        for (var i = Math.max(0, 0x1c + tlen - 0x40); i < out.length - 1; i++)
          if (out[i] === 0x60 && out[i + 1] === 0x1a) return out.subarray(i);
      }
      return bytes;
    }
    return gameProgram(bytes);
  }

  /* applique la table de relocation TOS : TEXT+DATA tels qu'en mémoire à `base` */
  function relocate(prg, base) {
    var tlen = u32be(prg, 2), dlen = u32be(prg, 6), blen = u32be(prg, 10), slen = u32be(prg, 14);
    var img = new Uint8Array(prg.subarray(0x1c, 0x1c + tlen + dlen));
    var r = 0x1c + tlen + dlen + slen, off = u32be(prg, r); r += 4;
    if (off) {
      var a = off;
      for (;;) {
        var v = (u32be(img, a) + base) >>> 0;
        img[a] = v >>> 24; img[a + 1] = (v >>> 16) & 0xff; img[a + 2] = (v >>> 8) & 0xff; img[a + 3] = v & 0xff;
        var c = prg[r++];
        if (c === 0) break;
        while (c === 1) { a += 254; c = prg[r++]; }
        a += c;
      }
    }
    return { image: img, bss: blen };
  }

  return { fatList: fatList, bytekiller: bytekiller, unpackPrg: unpackPrg, gameProgram: gameProgram,
           innerProgram: innerProgram, relocate: relocate,
           tracksFromFile: tracksFromFile, NAMES: NAMES, CELL: CELL };
})();
