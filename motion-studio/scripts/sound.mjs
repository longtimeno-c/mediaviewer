// Synthesized sound for a film (style-guide.md "Sound"): every cue comes from
// src/films/timeline.ts, the clock the picture uses. Writes out/<film>.wav (48 kHz stereo).
// Usage: node scripts/sound.mjs FilmA|FilmB
import {execFileSync} from 'node:child_process';
import fs from 'node:fs';

const film = process.argv[2];
fs.mkdirSync('out', {recursive: true});
execFileSync('npx', ['esbuild', 'src/films/timeline.ts', '--format=esm', '--outfile=out/timeline.mjs', '--log-level=error']);
const T = await import(`${process.cwd()}/out/timeline.mjs?${Date.now()}`);
const beats = film === 'FilmA' ? T.FILM_A : T.FILM_B;
const cues = film === 'FilmA' ? T.CUES_A : T.CUES_B;
const dur = T.seconds(beats);

const SR = 48000;
const n = Math.ceil(dur * SR);
const L = new Float32Array(n);
const R = new Float32Array(n);
let seed = 7; // seeded noise: the same film sounds the same every render
const noise = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff) * 2 - 1;
const add = (i, v, pan = 0) => { if (i >= 0 && i < n) { L[i] += v * (1 - pan); R[i] += v * (1 + pan); } };

function tone(t0, len, f0, f1, amp, pan = 0, attack = 0.004) {
  let ph = 0;
  const s0 = Math.floor(t0 * SR), m = Math.floor(len * SR);
  for (let k = 0; k < m; k++) {
    const x = k / m, f = f0 + (f1 - f0) * x;
    ph += (2 * Math.PI * f) / SR;
    const env = Math.min(1, k / (attack * SR)) * Math.exp(-4 * x);
    add(s0 + k, Math.sin(ph) * amp * env, pan);
  }
}
function click(t0, amp) {
  const s0 = Math.floor(t0 * SR), m = Math.floor(0.025 * SR);
  let y = 0;
  for (let k = 0; k < m; k++) { y = y * 0.55 + noise() * 0.45; add(s0 + k, y * amp * Math.exp((-6 * k) / m), 0.1); }
}
function thump(t0) { tone(t0, 0.14, 70, 45, 0.5); const s0 = Math.floor(t0 * SR); for (let k = 0; k < 600; k++) add(s0 + k, noise() * 0.05 * (1 - k / 600)); }

// The bed: two quiet chords, slow attack, under everything (about −24 dB below the cues).
const CHORDS = [[110, 164.8, 220, 261.6], [87.3, 130.8, 174.6, 220]];
for (let i = 0; i < n; i++) {
  const t = i / SR, c = CHORDS[Math.floor(t / 8) % 2];
  const edge = Math.min(1, (t % 8) / 1.5, (8 - (t % 8)) / 1.5);
  let v = 0;
  for (const f of c) v += Math.sin(2 * Math.PI * f * t) + 0.3 * Math.sin(2 * Math.PI * f * 2 * t);
  const fadeIn = Math.min(1, t / 1.5), fadeOut = Math.min(1, (dur - t) / 1.5);
  add(i, v * 0.012 * (0.6 + 0.4 * edge) * fadeIn * fadeOut);
}

// A soft rising shimmer while bytes move (between glide and glideEnd).
const glides = [];
for (const c of cues) if (c.kind === 'glide') glides.push([c.t, dur]); else if (c.kind === 'glideEnd' && glides.length) glides[glides.length - 1][1] = c.t;
for (const [a, b] of glides) {
  let ph = 0;
  for (let i = Math.floor(a * SR); i < Math.floor(b * SR) && i < n; i++) {
    const x = (i / SR - a) / (b - a);
    ph += (2 * Math.PI * (520 + 180 * x)) / SR;
    const env = Math.min(1, (i / SR - a) / 0.6, (b - i / SR) / 0.4);
    add(i, Math.sin(ph) * Math.sin(ph * 0.5) * 0.025 * env, 0.2 * Math.sin(i / SR));
  }
}

for (const c of cues) {
  switch (c.kind) {
    case 'cut': thump(c.t); break;
    case 'key': click(c.t, 0.35); break;
    case 'tick': tone(c.t, 0.05, 880, 880, 0.12); break;
    case 'chime': tone(c.t, 0.35, 659, 659, 0.14); tone(c.t + 0.12, 0.5, 988, 988, 0.12); break;
    case 'drop': tone(c.t, 0.3, 600, 180, 0.16); break;
    case 'chirp': tone(c.t, 0.12, 420, 880, 0.12); tone(c.t + 0.16, 0.12, 520, 1040, 0.12); break;
    case 'swell': {
      const s0 = Math.floor(c.t * SR);
      for (let i = s0; i < n; i++) {
        const t = (i - s0) / SR, x = Math.min(1, t / 2.5), out = Math.min(1, (dur - i / SR) / 1.2);
        let v = 0;
        for (const f of [220, 277.2, 329.6, 440]) v += Math.sin(2 * Math.PI * f * (i / SR));
        add(i, v * 0.03 * x * out);
      }
      break;
    }
  }
}

let peak = 0;
for (let i = 0; i < n; i++) peak = Math.max(peak, Math.abs(L[i]), Math.abs(R[i]));
const gain = 0.7 / peak; // −3 dBFS peak
const buf = Buffer.alloc(44 + n * 4);
buf.write('RIFF', 0); buf.writeUInt32LE(36 + n * 4, 4); buf.write('WAVEfmt ', 8);
buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20); buf.writeUInt16LE(2, 22); buf.writeUInt32LE(SR, 24);
buf.writeUInt32LE(SR * 4, 28); buf.writeUInt16LE(4, 32); buf.writeUInt16LE(16, 34); buf.write('data', 36); buf.writeUInt32LE(n * 4, 40);
for (let i = 0; i < n; i++) {
  buf.writeInt16LE(Math.round(Math.max(-1, Math.min(1, L[i] * gain)) * 32767), 44 + i * 4);
  buf.writeInt16LE(Math.round(Math.max(-1, Math.min(1, R[i] * gain)) * 32767), 46 + i * 4);
}
fs.writeFileSync(`out/${film}.wav`, buf);
console.log(`out/${film}.wav`, dur, 's');
