// The last frame of each film, which is also its poster (shotlist.md A12, B15).
import React from 'react';
import {Img, staticFile, useCurrentFrame} from 'remotion';
import {C, SAFE, SIZE} from '../tokens';
import {Stage, useVertical} from '../parts/Stage';
import {settle} from '../parts/motion';

const Icon: React.FC<{size: number}> = ({size}) => <Img src={staticFile('mediaviewer-icon.png')} style={{width: size, height: size}} />;

// Film A: the mark, the name, where it runs, where to get it, and where the numbers came from.
export const CloseA: React.FC = () => {
  const frame = useCurrentFrame();
  const vertical = useVertical();
  const s1 = settle(frame, 0);
  const s2 = settle(frame, 10);
  const s3 = settle(frame, 20);
  return (
    <Stage>
      <div
        style={{
          position: 'absolute',
          left: vertical ? SAFE * 1.5 : 300,
          right: SAFE,
          top: vertical ? 500 : 330,
          display: 'flex',
          flexDirection: vertical ? 'column' : 'row',
          alignItems: vertical ? 'flex-start' : 'center',
          gap: vertical ? 56 : 72,
        }}
      >
        <div style={{opacity: s1, transform: `scale(${0.94 + 0.06 * s1})`}}>
          <Icon size={vertical ? 300 : 340} />
        </div>
        <div>
          <div style={{fontSize: SIZE.hero * (vertical ? 1 : 1.33), lineHeight: 1, opacity: s1}}>MediaViewer</div>
          <div style={{fontSize: SIZE.body, marginTop: 34, opacity: s2, lineHeight: 1.35}}>
            Windows and macOS.{vertical ? <br /> : ' '}Free and open source.
          </div>
          <div style={{fontSize: vertical ? SIZE.body : SIZE.small, color: C.accent, marginTop: 26, opacity: s3}}>
            github.com/longtimeno-c/mediaviewer
          </div>
        </div>
      </div>
      <div style={{position: 'absolute', left: vertical ? SAFE * 1.5 : 300, right: SAFE, bottom: SAFE * 1.5, fontSize: SIZE.small, color: C.dim, opacity: s3, lineHeight: 1.4}}>
        Numbers measured on Windows: Ryzen 7 5700X3D, RTX 4070, 59.95 Hz.{vertical ? <br /> : ' '}Raw reports in docs/perf.
      </div>
    </Stage>
  );
};

// Film B: how to get driftwire (README "driftwire, on the command line"), and the promise.
const INSTALL: {os: string; line: string; prompt?: string}[] = [
  {os: 'Linux', line: 'curl -fsSL https://longtimeno-c.github.io/mediaviewer/driftwire/install.sh | sh'},
  {os: 'macOS', line: 'brew install longtimeno-c/tap/driftwire'},
  {os: 'Windows', line: 'irm https://longtimeno-c.github.io/mediaviewer/driftwire/install.ps1 | iex', prompt: 'PS> '},
];

export const CloseB: React.FC = () => {
  const frame = useCurrentFrame();
  const vertical = useVertical();
  const s0 = settle(frame, 0);
  return (
    <Stage>
      <div style={{position: 'absolute', left: vertical ? SAFE * 1.5 : 220, right: SAFE, top: vertical ? 600 : 210}}>
        <div style={{fontSize: SIZE.hero, lineHeight: 1, opacity: s0}}>
          <span style={{color: C.accent}}>≋</span> Driftwire
        </div>
        <div style={{fontSize: SIZE.large, marginTop: 36, opacity: s0}}>No account. No cloud.</div>

        <div style={{marginTop: vertical ? 120 : 90}}>
          {vertical ? (
            <>
              <Row frame={frame} at={12} os="macOS" line={INSTALL[1].line} size={SIZE.body} />
              <Row frame={frame} at={18} os="Linux and Windows: one line from" line="longtimeno-c.github.io/mediaviewer/driftwire" prompt="" size={SIZE.body} />
            </>
          ) : (
            INSTALL.map((r, i) => <Row key={r.os} frame={frame} at={12 + i * 6} os={r.os} line={r.line} prompt={r.prompt} size={SIZE.small} />)
          )}
        </div>
      </div>
      <div
        style={{
          position: 'absolute',
          left: vertical ? SAFE * 1.5 : 220,
          bottom: SAFE * 1.5,
          display: 'flex',
          alignItems: 'center',
          gap: 20,
          fontSize: vertical ? SIZE.body : SIZE.small,
          color: C.dim,
          opacity: settle(frame, 30),
        }}
      >
        <Img src={staticFile('mediaviewer-icon.png')} style={{width: 64, height: 64}} />
        In MediaViewer, or on its own.
      </div>
    </Stage>
  );
};

const Row: React.FC<{frame: number; at: number; os: string; line: string; prompt?: string; size: number}> = ({frame, at, os, line, prompt = '$ ', size}) => {
  const s = settle(frame, at);
  return (
    <div style={{opacity: s, marginBottom: size * 1.3}}>
      <div style={{fontSize: size, color: C.dim, marginBottom: 10}}>{os}</div>
      <div style={{fontSize: size, color: C.text, whiteSpace: 'pre'}}>
        <span style={{color: C.dim}}>{prompt}</span>
        {line}
      </div>
    </div>
  );
};
