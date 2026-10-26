// Film B beat 6: one link, two locks (docs/design/28 "The wire"). The handshake mixes an X25519
// secret and an ML-KEM-768 secret into the keys, so reading a recording needs both broken.
import React from 'react';
import {useCurrentFrame, useVideoConfig} from 'remotion';
import {C, SIZE} from '../tokens';
import {Caption, Stage, useVertical} from '../parts/Stage';
import {ramp, settle, snap} from '../parts/motion';

const CIPHER = 'Noise_XXhfs_25519+MLKEM768_ChaChaPoly_BLAKE2b';
const LOCKS = [
  {label: 'X25519', appear: 30, close: 44},
  {label: 'ML-KEM-768', appear: 60, close: 74},
];
const CIPHER_AT = 100;

const Padlock: React.FC<{size: number; closed: boolean; colour: string}> = ({size, closed, colour}) => {
  const lift = closed ? 0 : size * 0.16;
  return (
    <svg width={size} height={size * 1.15} viewBox={`0 0 ${size} ${size * 1.15}`}>
      <rect x={size * 0.2} y={size * 0.14 - lift} width={size * 0.6} height={size * 0.5 + lift} fill={C.canvas} />
      <path
        d={`M ${size * 0.3} ${size * 0.55 - lift} V ${size * 0.36 - lift} a ${size * 0.2} ${size * 0.2} 0 0 1 ${size * 0.4} 0 V ${size * 0.55 - (closed ? 0 : lift * 0.2)}`}
        fill="none"
        stroke={colour}
        strokeWidth={size * 0.08}
        strokeLinecap="round"
      />
      <rect x={size * 0.14} y={size * 0.52} width={size * 0.72} height={size * 0.56} rx={size * 0.09} fill={C.canvas} stroke={colour} strokeWidth={size * 0.07} />
      <circle cx={size * 0.5} cy={size * 0.78} r={size * 0.06} fill={colour} />
    </svg>
  );
};

export const Locks: React.FC = () => {
  const frame = useCurrentFrame();
  const {width, height} = useVideoConfig();
  const vertical = useVertical();
  const draw = ramp(frame, 0, 20);
  const lockSize = vertical ? 150 : 130;

  // Two computers and the link between them: across in 16:9, down in 9:16.
  const a = vertical ? {x: width / 2, y: 520} : {x: 330, y: 400};
  const b = vertical ? {x: width / 2, y: 1330} : {x: width - 330, y: 400};
  const lineFrom = vertical ? {x: a.x, y: a.y + 110} : {x: a.x + 230, y: a.y + 20};
  const lineTo = vertical ? {x: b.x, y: b.y - 40} : {x: b.x - 230, y: b.y + 20};
  const at = (t: number) => ({x: lineFrom.x + (lineTo.x - lineFrom.x) * t, y: lineFrom.y + (lineTo.y - lineFrom.y) * t});
  const lockPos = [at(0.36), at(0.64)];

  const nodeStyle = (p: {x: number; y: number}): React.CSSProperties => ({
    position: 'absolute', left: p.x, top: p.y, transform: 'translateX(-50%)', textAlign: 'center', whiteSpace: 'nowrap', fontSize: SIZE.body,
  });

  const both = frame >= LOCKS[1].close;
  return (
    <Stage>
      <div style={nodeStyle(a)}>ThinkPad</div>
      <div style={nodeStyle(b)}>Ann's MacBook</div>

      <svg width={width} height={height} style={{position: 'absolute', left: 0, top: 0}}>
        <line
          x1={lineFrom.x}
          y1={lineFrom.y}
          x2={lineFrom.x + (lineTo.x - lineFrom.x) * draw}
          y2={lineFrom.y + (lineTo.y - lineFrom.y) * draw}
          stroke={both ? C.accent : C.hairline}
          strokeWidth={4}
        />
      </svg>

      {LOCKS.map((l, i) => {
        if (frame < l.appear) return null;
        const s = settle(frame, l.appear);
        const closed = snap(frame, l.close) === 1;
        const p = lockPos[i];
        return (
          <div
            key={l.label}
            style={{
              position: 'absolute',
              left: p.x,
              top: p.y,
              transform: `translate(-50%, -62%) scale(${0.9 + 0.1 * s})`,
              opacity: s,
              display: 'flex',
              flexDirection: vertical ? 'row' : 'column',
              alignItems: 'center',
              gap: vertical ? 28 : 12,
            }}
          >
            <Padlock size={lockSize} closed={closed} colour={closed ? C.accent : C.text} />
            <div style={{fontSize: SIZE.body, whiteSpace: 'nowrap', color: closed ? C.text : C.dim, ...(vertical ? {position: 'absolute', left: lockSize + 28} : {})}}>{l.label}</div>
          </div>
        );
      })}

      {frame >= CIPHER_AT ? (
        <div
          style={{
            position: 'absolute',
            left: 0,
            right: 0,
            top: vertical ? 1520 : 640,
            textAlign: 'center',
            fontSize: SIZE.body,
            color: C.text,
            opacity: settle(frame, CIPHER_AT),
            whiteSpace: 'pre',
            lineHeight: 1.4,
          }}
        >
          {vertical ? CIPHER.replace('+', '+\n') : CIPHER}
        </div>
      ) : null}

      <Caption
        text="A recording made today stays sealed."
        source="Reading it needs X25519 and ML-KEM-768 both broken. Modelled in ProVerif."
        opacity={settle(frame, 12)}
      />
    </Stage>
  );
};
