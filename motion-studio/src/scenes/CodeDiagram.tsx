// Film B beat 5: why a machine in the middle can't fake the code (docs/design/28 "The wire",
// steps 1–5, protocol 3). Each side commits to a nonce before it sees the other's, the receiver
// reveals last, and the six digits come from the handshake and both nonces. Only the receiver
// shows them; the sender's person types them.
import React from 'react';
import {useCurrentFrame, useVideoConfig} from 'remotion';
import {C, SIZE} from '../tokens';
import {Caption, Stage, useVertical} from '../parts/Stage';
import {ramp, settle} from '../parts/motion';

type Msg = {at: number; dir: 1 | -1; label: string; token: string; sealed: boolean; last?: boolean};
const MSGS: Msg[] = [
  {at: 30, dir: 1, label: 'key + sealed nonce', token: 'A', sealed: true},
  {at: 75, dir: -1, label: 'key + sealed nonce', token: 'B', sealed: true},
  {at: 120, dir: 1, label: 'opens nonce', token: 'A', sealed: false},
  {at: 165, dir: -1, label: 'opens nonce, last', token: 'B', sealed: false, last: true},
];
const FORMULA_AT = 210;
const DIGITS_AT = 240;
export const CODE = '342 917';

const Token: React.FC<{letter: string; sealed: boolean; size: number}> = ({letter, sealed, size}) => (
  <span
    style={{
      display: 'inline-flex',
      alignItems: 'center',
      justifyContent: 'center',
      width: size * 1.4,
      height: size * 1.4,
      marginLeft: size * 0.4,
      border: `2px solid ${sealed ? C.dim : C.accent}`,
      borderRadius: 6,
      background: sealed ? C.surface : 'transparent',
      color: sealed ? C.dim : C.accent,
      fontSize: size,
    }}
  >
    {sealed ? '#' : letter}
  </span>
);

const Node: React.FC<{x: number; y: number; name: string; role: string; size: number}> = ({x, y, name, role, size}) => (
  <div style={{position: 'absolute', left: x, top: y, transform: 'translateX(-50%)', textAlign: 'center', whiteSpace: 'nowrap'}}>
    <div style={{fontSize: size}}>{name}</div>
    <div style={{fontSize: SIZE.small, color: C.dim, marginTop: 6}}>{role}</div>
  </div>
);

export const CodeDiagram: React.FC = () => {
  const frame = useCurrentFrame();
  const {width} = useVideoConfig();
  const vertical = useVertical();

  const L = vertical ? 230 : 600;
  const R = vertical ? width - 230 : width - 600;
  const topY = vertical ? 470 : 70;
  const lifeTop = topY + (vertical ? 110 : 100);
  const msgY0 = vertical ? 650 : 250;
  const msgStep = vertical ? 150 : 100;
  const labelSize = vertical ? SIZE.body : SIZE.small;
  const formulaY = msgY0 + msgStep * 4 + (vertical ? 20 : 0);
  const lifeBottom = formulaY - 30;

  // The newest message is teal; earlier ones step back to the text colour.
  const latest = MSGS.reduce((n, m, i) => (frame >= m.at ? i : n), -1);

  return (
    <Stage>
      <Node x={L} y={topY} name="ThinkPad" role="sender" size={vertical ? SIZE.body : SIZE.body} />
      <Node x={R} y={topY} name="Ann's MacBook" role="receiver" size={vertical ? SIZE.body : SIZE.body} />

      <svg width={width} height={lifeBottom + 10} style={{position: 'absolute', left: 0, top: 0}}>
        <line x1={L} y1={lifeTop} x2={L} y2={lifeBottom} stroke={C.hairline} strokeWidth={2} />
        <line x1={R} y1={lifeTop} x2={R} y2={lifeBottom} stroke={C.hairline} strokeWidth={2} />
        {MSGS.map((m, i) => {
          const y = msgY0 + i * msgStep + labelSize * 1.6;
          const p = ramp(frame, m.at, 10);
          if (p <= 0) return null;
          const from = m.dir === 1 ? L : R;
          const to = m.dir === 1 ? R : L;
          const head = from + (to - from) * p;
          const col = i === latest ? C.accent : C.text;
          const a = 14 * m.dir;
          return (
            <g key={i}>
              <line x1={from} y1={y} x2={head} y2={y} stroke={col} strokeWidth={3} />
              {p >= 1 ? <path d={`M ${to - a} ${y - 9} L ${to} ${y} L ${to - a} ${y + 9}`} fill="none" stroke={col} strokeWidth={3} /> : null}
            </g>
          );
        })}
      </svg>

      {MSGS.map((m, i) => {
        const s = settle(frame, m.at);
        if (frame < m.at) return null;
        const y = msgY0 + i * msgStep;
        return (
          <div
            key={i}
            style={{
              position: 'absolute',
              top: y - 8,
              left: L,
              width: R - L,
              display: 'flex',
              justifyContent: 'center',
              alignItems: 'center',
              fontSize: labelSize,
              opacity: s,
              transform: `translateX(${(1 - s) * 24 * m.dir}px)`,
              color: i === latest ? C.text : C.dim,
            }}
          >
            <span style={{color: C.dim, marginRight: labelSize * 0.6}}>{i + 1}</span>
            {m.label}
            <Token letter={m.token} sealed={m.sealed} size={labelSize * 0.8} />
          </div>
        );
      })}

      {frame >= FORMULA_AT ? (
        <Outcome frame={frame} L={L} R={R} y={formulaY} vertical={vertical} />
      ) : null}

      <Caption
        text="Neither side knows the code before the other has finished its part."
        source="Each side seals its nonce first. The receiver opens last."
        opacity={settle(frame, 12)}
      />
    </Stage>
  );
};

// What both sides end with. 16:9: each result under its own computer, labels on one baseline.
// 9:16: stacked down the middle, receiver first, so nothing runs off the edge.
const Outcome: React.FC<{frame: number; L: number; R: number; y: number; vertical: boolean}> = ({frame, L, R, y, vertical}) => {
  const s = settle(frame, FORMULA_AT);
  const d = settle(frame, DIGITS_AT);
  const small = vertical ? SIZE.body : SIZE.small;
  const filled = frame >= DIGITS_AT + 20 ? Math.min(6, Math.floor((frame - DIGITS_AT - 20) / 4)) : 0;
  const box = (x: number | null, top: number, opacity: number, label: string, body: React.ReactNode) => (
    <div
      style={{
        position: 'absolute',
        top,
        ...(x === null ? {left: 0, right: 0} : {left: x, transform: 'translateX(-50%)'}),
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'center',
        whiteSpace: 'nowrap',
        opacity,
      }}
    >
      <div style={{height: SIZE.hero * 1.05, display: 'flex', alignItems: 'center'}}>{body}</div>
      <div style={{fontSize: small, color: C.dim, marginTop: 14}}>{label}</div>
    </div>
  );
  const digits = <div style={{fontSize: SIZE.hero, color: C.accent, lineHeight: 1, letterSpacing: 4}}>{CODE}</div>;
  const slots = <Slots filled={filled} size={SIZE.body} />;
  return (
    <div style={{position: 'absolute', top: y, left: 0, right: 0, opacity: s}}>
      <div style={{textAlign: 'center', fontSize: small, color: C.dim}}>
        both compute  <span style={{color: C.text}}>hash(handshake, A, B)</span>  →  six digits
      </div>
      {vertical ? (
        <>
          {box(null, small * 2.4, d, "Ann's MacBook shows them", digits)}
          {box(null, small * 2.4 + SIZE.hero * 1.05 + small * 2.8, d, "ThinkPad's person types them", slots)}
        </>
      ) : (
        <>
          {box(L, small * 2.4, d, 'its person types them', slots)}
          {box(R, small * 2.4, d, 'only this screen shows them', digits)}
        </>
      )}
    </div>
  );
};

const Slots: React.FC<{filled: number; size: number}> = ({filled, size}) => {
  const digits = CODE.replace(' ', '');
  return (
    <div style={{display: 'flex', gap: size * 0.25}}>
      {digits.split('').map((ch, i) => (
        <React.Fragment key={i}>
          {i === 3 ? <span style={{width: size * 0.4}} /> : null}
          <span
            style={{
              width: size * 1.1,
              height: size * 1.6,
              border: `2px solid ${i < filled ? C.accent : C.hairline}`,
              borderRadius: 6,
              display: 'inline-flex',
              alignItems: 'center',
              justifyContent: 'center',
              fontSize: size,
              color: C.text,
            }}
          >
            {i < filled ? ch : ''}
          </span>
        </React.Fragment>
      ))}
    </div>
  );
};
