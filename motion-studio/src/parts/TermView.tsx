// The driftwire CLI's own render output (src/terminal-*.json), made to move the way the CLI
// moves: dots travel the path toward the receiver at about 12 steps a second and freeze while
// it reconnects (docs/design/28 "From a terminal"). Also renames a computer inside a card so
// one send reads the same on both screens, keeping every frame square.
import React from 'react';
import {Section, Span, TermLine} from './Terminal';
import wide from '../terminal-90.json';
import narrow from '../terminal-44.json';

export const sections = (cols: 90 | 44) => (cols === 90 ? wide : narrow) as Section[];

export const lines = (cols: 90 | 44, title: string): Span[][] => {
  const s = sections(cols).find((x) => x.title === title);
  if (!s) throw new Error(`no gallery section "${title}"`);
  return s.lines;
};

const clone = (l: Span[][]) => l.map((line) => line.map((s) => ({...s})));

// Replace `from` with `to` and give the width back to (or take it from) the line's padding:
// the last run of spaces, or for a card's title border, the run of ─.
export function rename(input: Span[][], from: string, to: string): Span[][] {
  const out = clone(input);
  for (const line of out) {
    let delta = 0;
    let hit = -1;
    line.forEach((s, si) => {
      while (s.text.includes(from)) {
        s.text = s.text.replace(from, to);
        delta += from.length - to.length;
        hit = si;
      }
    });
    if (delta === 0) continue;
    const pad = (re: RegExp, ch: string) => {
      for (let i = line.length - 1; i >= 0; i--) {
        const m = line[i].text.match(re);
        if (!m || m.index === undefined) continue;
        const run = m[0];
        if (delta < 0 && run.length <= -delta) continue;
        const next = delta > 0 ? run + ch.repeat(delta) : run.slice(0, run.length + delta);
        line[i].text = line[i].text.slice(0, m.index) + next + line[i].text.slice(m.index + run.length);
        return true;
      }
      return false;
    };
    // No padding run (a span the text fills exactly): a shorter name pads its own span.
    if (!pad(/ {2,}(?=\S*$)/, ' ') && !pad(/─{3,}/, '─') && delta > 0) line[hit].text += ' '.repeat(delta);
  }
  return out;
}

// Move the path's dots: every ● becomes ━ again, then two dots per run of ━ step along it.
export function moveDots(input: Span[][], frame: number, frozen: boolean): Span[][] {
  if (frozen) return input;
  const out = clone(input);
  const step = Math.floor(frame / 2.5); // ~12 steps a second at 30 fps
  for (const line of out) {
    const chars: {span: number; idx: number; ch: string}[] = [];
    line.forEach((s, si) => Array.from(s.text).forEach((ch, ci) => chars.push({span: si, idx: ci, ch})));
    if (!chars.some((c) => c.ch === '━')) continue;
    for (const c of chars) if (c.ch === '●') c.ch = '━';
    let i = 0;
    while (i < chars.length) {
      if (chars[i].ch !== '━') { i++; continue; }
      let j = i;
      while (j < chars.length && chars[j].ch === '━') j++;
      const len = j - i;
      // Two dots on a long run, one on a short one (the 44-column path's runs are 2–4 long).
      const dots = len >= 6 ? 2 : 1;
      const gap = Math.max(3, Math.floor(len / 2));
      for (let k = 0; k < dots; k++) chars[i + ((step + k * gap) % len)].ch = '●';
      i = j;
    }
    const bySpan: string[][] = line.map(() => []);
    for (const c of chars) bySpan[c.span].push(c.ch);
    line.forEach((s, si) => { s.text = bySpan[si].join(''); });
  }
  return out;
}

export const Block: React.FC<{lines: Span[][]}> = ({lines: ls}) => (
  <>
    {ls.map((l, i) => <TermLine key={i} spans={l} />)}
  </>
);
