// Contact sheet for a film: one still per beat (at 60 % of it) plus fast transitions, rendered
// from one bundle, tiled into reviews/<dir>/<film>-<fmt>-sheet.png. Usage:
//   node scripts/sheet.mjs FilmA 16x9 [reviews dir, default gate-4]
import {bundle} from '@remotion/bundler';
import {renderStill, selectComposition} from '@remotion/renderer';
import {execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const [, , film, fmt = '16x9', dir = 'gate-4'] = process.argv;
const src = fs.readFileSync('src/films/timeline.ts', 'utf8');
const key = film === 'FilmA' ? 'FILM_A' : 'FILM_B';
const block = src.slice(src.indexOf(`export const ${key}`)).split('];')[0];
const beats = [...block.matchAll(/id: '(\w+)', start: ([\d.]+), end: ([\d.]+)/g)].map((m) => ({id: m[1], start: +m[2], end: +m[3]}));

const out = path.resolve(`reviews/${dir}/${film}-${fmt}`);
fs.mkdirSync(out, {recursive: true});
const serveUrl = await bundle({entryPoint: path.resolve('src/index.ts')});
const composition = await selectComposition({serveUrl, id: `${film}-${fmt}`});
const frames = beats.map((b) => ({id: b.id, frame: Math.round((b.start + (b.end - b.start) * 0.6) * 30)}));
for (const extra of process.argv.slice(5)) frames.push({id: `t${extra}`, frame: Math.round(+extra * 30)});
let i = 0;
for (const fr of frames) {
  const file = `${out}/${String(i++).padStart(2, '0')}-${fr.id}.png`;
  await renderStill({composition, serveUrl, output: file, frame: fr.frame});
  console.log(file, fr.frame);
}
const cols = fmt === '16x9' ? 4 : 7;
execFileSync('/opt/homebrew/bin/ffmpeg', ['-hide_banner', '-loglevel', 'error', '-y', '-pattern_type', 'glob', '-i', `${out}/*.png`,
  '-vf', `scale=${fmt === '16x9' ? 480 : 270}:-1,tile=${cols}x${Math.ceil(frames.length / cols)}:padding=6:color=0x111111`, '-frames:v', '1', `${out}-sheet.png`]);
console.log(`${out}-sheet.png`);
