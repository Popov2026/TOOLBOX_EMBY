// encrypt_disk.mjs — chiffre une image disque pour le chargement par code de licence (version navigateur).
// Usage : node encrypt_disk.mjs DISQUE.st NOM1 [NOM2 ...]      exemple : node encrypt_disk.mjs scr.st moi auteur
// Un code au hasard par nom. Produit stuntcar.st.enc (dossier courant) et affiche, pour chaque nom, le code
// à donner et la ligne « licence = NOM EMPREINTE » à mettre dans scr_relay.ini : le journal du serveur
// relais dira alors quel code a servi.
// Chiffrement : clé de données AES-256 au hasard (disquette en AES-GCM), enveloppée une fois par code
// (PBKDF2-SHA256 250 000 tours + AES-GCM). Sans un des codes, le fichier est inutilisable.
// Format : "SCRE2" | nombre de codes (1 octet) | par code : sel (16) IV (12) clé enveloppée (48) | IV (12) | disquette chiffrée
import { readFileSync, writeFileSync } from 'node:fs';
import { webcrypto as crypto } from 'node:crypto';

const [disk, ...names] = process.argv.slice(2);
if (!disk || !names.length || names.length > 20 || names.some((n) => !/^[\w-]{1,24}$/.test(n))) {
  console.error('usage : node encrypt_disk.mjs DISQUE.st NOM1 [NOM2 ...]   (noms : lettres, chiffres, - et _)'); process.exit(1);
}
const ALPHA = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';   // sans 0/O ni 1/I
const newCode = () => Array.from(crypto.getRandomValues(new Uint8Array(20)), (b) => ALPHA[b % 32]).join('').replace(/(.{4})(?!$)/g, '$1-');
const norm = (c) => c.toUpperCase().replace(/[^A-Z0-9]/g, '');
const hex = (b) => Buffer.from(b).toString('hex');

const dataKeyRaw = crypto.getRandomValues(new Uint8Array(32));
const dataKey = await crypto.subtle.importKey('raw', dataKeyRaw, 'AES-GCM', false, ['encrypt']);
const parts = [Buffer.from('SCRE2'), Buffer.from([names.length])];
const lines = [];
for (const name of names) {
  const code = newCode();
  const salt = crypto.getRandomValues(new Uint8Array(16)), iv = crypto.getRandomValues(new Uint8Array(12));
  const base = await crypto.subtle.importKey('raw', new TextEncoder().encode(norm(code)), 'PBKDF2', false, ['deriveKey']);
  const kek = await crypto.subtle.deriveKey({ name: 'PBKDF2', salt, iterations: 250000, hash: 'SHA-256' }, base,
    { name: 'AES-GCM', length: 256 }, false, ['encrypt']);
  const wrapped = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv }, kek, dataKeyRaw));
  parts.push(Buffer.from(salt), Buffer.from(iv), Buffer.from(wrapped));
  const fp = hex(await crypto.subtle.digest('SHA-256', new TextEncoder().encode('SCR-LIC:' + norm(code)))).slice(0, 16);
  lines.push({ name, code, fp });
}
const iv = crypto.getRandomValues(new Uint8Array(12));
const ct = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv }, dataKey, readFileSync(disk)));
parts.push(Buffer.from(iv), Buffer.from(ct));
writeFileSync('stuntcar.st.enc', Buffer.concat(parts));
console.log('stuntcar.st.enc écrit\n');
for (const l of lines) console.log(l.name.padEnd(12) + ' code : ' + l.code);
console.log('\nà ajouter dans scr_relay.ini (puis relancer le serveur relais) :');
for (const l of lines) console.log('licence = ' + l.name + ' ' + l.fp);
