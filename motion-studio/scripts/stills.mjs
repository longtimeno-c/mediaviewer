// Renders one still per scene and format into reviews/<gate>/ (default gate-3).
// Usage: npm run stills [-- gate-3] [-- B05-code]
import {execFileSync} from 'node:child_process';
import fs from 'node:fs';

const STILLS = [
  ['B05-code', 285],
  ['B06-locks', 200],
  ['B14-terminal', 165],
  ['A12-close', 140],
  ['B15-close', 110],
];
const gate = process.argv[2] ?? 'gate-3';
const only = process.argv[3];
fs.mkdirSync(`reviews/${gate}`, {recursive: true});
for (const [id, frame] of STILLS) {
  if (only && id !== only) continue;
  for (const fmt of ['16x9', '9x16']) {
    const comp = `${id}-${fmt}`;
    execFileSync('npx', ['remotion', 'still', 'src/index.ts', comp, `reviews/${gate}/${comp}.png`, `--frame=${frame}`, '--log=error'], {stdio: 'inherit'});
    console.log(`reviews/${gate}/${comp}.png  (frame ${frame})`);
  }
}
