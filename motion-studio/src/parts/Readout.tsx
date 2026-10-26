// Numbers set like the F3 overlay's readouts: label, then value, left-aligned, monospace.
// Values count up over 10 frames, then hold (style-guide.md "Numbers").
import React from 'react';
import {interpolate, useCurrentFrame} from 'remotion';
import {C, SIZE} from '../tokens';
import {settle} from './motion';

export type Row = {label: string; value: number; decimals?: number; unit?: string; grouped?: boolean; suffix?: string};

const fmt = (v: number, d = 0, grouped = false) =>
  grouped ? Math.round(v).toLocaleString('en-GB') : v.toFixed(d);

export const Readout: React.FC<{rows: Row[]; at?: number; source?: string; size?: number; style?: React.CSSProperties}> = ({
  rows,
  at = 6,
  source,
  size = SIZE.body,
  style,
}) => {
  const frame = useCurrentFrame();
  const s = settle(frame, at);
  return (
    <div
      style={{
        background: 'rgba(25,26,31,0.92)',
        border: `1px solid ${C.hairline}`,
        borderRadius: 12,
        padding: `${size * 0.6}px ${size * 0.8}px`,
        opacity: s,
        ...style,
      }}
    >
      {rows.map((r, i) => {
        const t = interpolate(frame, [at + i * 3, at + i * 3 + 10], [0, 1], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'});
        return (
          <div key={r.label} style={{display: 'flex', gap: size * 0.6, alignItems: 'baseline', fontSize: size, lineHeight: 1.45, whiteSpace: 'nowrap'}}>
            <span style={{color: C.dim, minWidth: size * 5.2}}>{r.label}</span>
            <span style={{color: i === 0 ? C.accent : C.text}}>
              {fmt(r.value * t, r.decimals, r.grouped)}
              {r.suffix ?? ''}
            </span>
            {r.unit ? <span style={{color: C.dim, fontSize: size * 0.67}}>{r.unit}</span> : null}
          </div>
        );
      })}
      {source ? <div style={{color: C.dim, fontSize: SIZE.small, marginTop: 10}}>{source}</div> : null}
    </div>
  );
};

export const Keycap: React.FC<{label: string; at: number; size?: number; style?: React.CSSProperties}> = ({label, at, size = SIZE.body, style}) => {
  const frame = useCurrentFrame();
  const on = frame >= at && frame < at + 8;
  if (frame < at) return null;
  return (
    <div
      style={{
        display: 'inline-flex',
        alignItems: 'center',
        justifyContent: 'center',
        minWidth: size * 1.6,
        height: size * 1.6,
        padding: `0 ${size * 0.4}px`,
        borderRadius: 8,
        border: `2px solid ${on ? C.accent : C.hairline}`,
        background: on ? 'rgba(64,200,224,0.15)' : C.surface,
        color: on ? C.accent : C.text,
        fontSize: size,
        ...style,
      }}
    >
      {label}
    </div>
  );
};
