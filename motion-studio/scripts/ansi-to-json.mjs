// Turns the driftwire CLI gallery capture (assets/terminal/*.ansi, real render code with
// made-up data) into spans the Terminal component draws. Sections start at the gallery's
// dim "· title" lines. The gallery's made-up owner name is swapped for an invented one and the
// card's padding re-balanced so frames stay square.
import fs from 'node:fs';

const [, , src, dst] = process.argv;
// The banner's version is the gallery's placeholder; show the release that is out (driftwire 0.1.38).
// The computer is "ThinkPad" everywhere, as the CLI's own path line already calls it.
const SWAP = [["Tristan's ThinkPad", "ThinkPad"], ["1.0.0", "0.1.38"]];

function parseLine(raw) {
  const spans = [];
  let st = {fg: null, bold: false, dim: false};
  const re = /\x1b\[([0-9;]*)m/g;
  let last = 0, m;
  const push = (text) => { if (text) spans.push({text, ...st}); };
  while ((m = re.exec(raw))) {
    push(raw.slice(last, m.index));
    last = re.lastIndex;
    const p = m[1].split(';').filter(Boolean).map(Number);
    if (p.length === 0) st = {fg: null, bold: false, dim: false};
    for (let i = 0; i < p.length; i++) {
      const c = p[i];
      if (c === 0) st = {fg: null, bold: false, dim: false};
      else if (c === 1) st = {...st, bold: true};
      else if (c === 2) st = {...st, dim: true};
      else if (c === 38 && p[i + 1] === 2) {
        st = {...st, fg: '#' + p.slice(i + 2, i + 5).map((v) => v.toString(16).padStart(2, '0')).join('')};
        i += 4;
      }
    }
  }
  push(raw.slice(last));
  return spans;
}

function swapNames(spans) {
  let delta = 0;
  for (const s of spans) for (const [from, to] of SWAP) {
    if (s.text.includes(from)) { s.text = s.text.replace(from, to); delta += from.length - to.length; }
  }
  if (delta !== 0) {
    // Give the width back to (or take it from) the last run of padding before the closing border.
    for (let i = spans.length - 1; i >= 0; i--) {
      const mm = spans[i].text.match(/ {2,}$/);
      if (mm && i < spans.length - 1) {
        spans[i].text = delta > 0 ? spans[i].text + ' '.repeat(delta) : spans[i].text.slice(0, delta);
        break;
      }
    }
  }
  return spans;
}

const lines = fs.readFileSync(src, 'utf8').replace(/\r/g, '').split('\n');
const sections = [];
let cur = null;
for (const raw of lines) {
  const spans = swapNames(parseLine(raw));
  const plain = spans.map((s) => s.text).join('');
  if (plain.startsWith('· ') && spans[0]?.dim) {
    cur = {title: plain.slice(2), lines: []};
    sections.push(cur);
    continue;
  }
  if (!cur) continue;
  cur.lines.push(spans);
}
for (const s of sections) while (s.lines.length && s.lines.at(-1).every((x) => !x.text.trim())) s.lines.pop();
fs.writeFileSync(dst, JSON.stringify(sections));
console.log(sections.length, 'sections →', dst);
