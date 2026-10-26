import React from 'react';
import {C, TERM_FONT} from '../tokens';

export type Span = {text: string; fg: string | null; bold: boolean; dim: boolean};
export type Section = {title: string; lines: Span[][]};

// One line of the driftwire CLI as its own renderer drew it (scripts/ansi-to-json.mjs).
// Each character sits in its own 1ch cell, as in a terminal grid, so box-drawing frames stay
// square whatever font supplies a glyph.
export const TermLine: React.FC<{spans: Span[]}> = ({spans}) => (
  <div style={{whiteSpace: 'pre', height: '1.2em', display: 'flex'}}>
    {spans.flatMap((s, i) =>
      Array.from(s.text).map((ch, j) => (
        <span
          key={`${i}.${j}`}
          style={{
            display: 'inline-block',
            width: '1ch',
            textAlign: 'center',
            color: s.fg ?? (s.dim ? C.dim : C.text),
            fontWeight: s.bold ? 700 : 400,
          }}
        >
          {ch}
        </span>
      )),
    )}
  </div>
);

export const plain = (text: string, fg: string | null = null): Span[] => [{text, fg, bold: false, dim: false}];

// A terminal as it sits on a desk: a dark well, a hairline edge, its title. No window chrome.
export const TerminalWell: React.FC<{title: string; fontSize: number; cols: number; children: React.ReactNode; style?: React.CSSProperties}> = ({
  title,
  fontSize,
  cols,
  children,
  style,
}) => (
  <div
    style={{
      background: C.well,
      border: `1px solid ${C.hairline}`,
      borderRadius: 14,
      fontFamily: TERM_FONT,
      fontSize,
      lineHeight: 1.2,
      overflow: 'hidden',
      ...style,
    }}
  >
    <div style={{color: C.dim, fontSize: Math.round(fontSize * 0.75), padding: '14px 22px', borderBottom: `1px solid ${C.hairline}`}}>{title}</div>
    <div style={{padding: '22px 22px 26px', width: `${cols}ch`, boxSizing: 'content-box'}}>{children}</div>
  </div>
);
