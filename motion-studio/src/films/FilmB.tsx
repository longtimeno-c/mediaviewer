// Film B, Driftwire (shotlist.md, retimed). The send is shown in driftwire, the command line,
// from its own render output (src/terminal-*.json): ThinkPad sends 24 files to Ann's MacBook.
// The made-up example data is made to agree across cards (one send of 24 files, 412 MB at
// 14 MB/s), and the receiver's cards name the sender. Every rename is the same length or
// shorter, so no card's frame moves. MediaViewer's own window closes the film.
import React from 'react';
import {Sequence, useCurrentFrame, useVideoConfig} from 'remotion';
import {FPS, SIZE} from '../tokens';
import {Caption, Stage, useVertical} from '../parts/Stage';
import {settle} from '../parts/motion';
import {Span, TerminalWell} from '../parts/Terminal';
import {Block, lines, moveDots, rename} from '../parts/TermView';
import {DRIFT, Window} from '../parts/Window';
import {CodeDiagram} from '../scenes/CodeDiagram';
import {Locks} from '../scenes/Locks';
import {CloseB} from '../scenes/Closes';
import {DRIFT_BLURS} from './FilmA';
import {CODE_WAIT, DROP, FILM_B} from './timeline';

const f = (s: number) => Math.round(s * FPS);
const SENDER = 'ThinkPad';
const RECEIVER = "Ann's MacBook";

type Cols = 90 | 44;
// A receiver's card from the gallery names Ann's MacBook as the sender; here Ann receives.
const recv = (cols: Cols, title: string) => rename(lines(cols, title), RECEIVER, SENDER);

// One terminal, centred: the CLI at 44 columns, large enough to read on a phone.
const Term: React.FC<{title: string; children: (cols: Cols) => Span[][]; top?: number}> = ({title, children, top}) => {
  const vertical = useVertical();
  const cols: Cols = 44;
  return (
    <div style={{position: 'absolute', left: 0, right: 0, top: top ?? (vertical ? 470 : 110), display: 'flex', justifyContent: 'center'}}>
      <TerminalWell title={title} fontSize={vertical ? 34 : 46} cols={cols}>
        <Block lines={children(cols)} />
      </TerminalWell>
    </div>
  );
};

// The gallery's sending cards say 4.8 GB; this send is the 412 MB the offer showed.
const sizes = (l: Span[][]) =>
  rename(rename(rename(l, '2.0 GB of 4.8 GB', '172 MB of 412 MB'), '4.8 GB of 4.8 GB', '412 MB of 412 MB'), 'about 3 min left', 'about 17 s left');
const sending = (cols: Cols, state: string, frame: number) =>
  moveDots(sizes(lines(cols, state)), frame, state === 'sending: reconnecting');

const Open: React.FC = () => {
  const frame = useCurrentFrame();
  return (
    <>
      <Term title={`${SENDER} — zsh`}>
        {(cols) => [...lines(cols, 'start banner (printed once)'), [], ...sending(cols, 'sending: Wi-Fi, 42 %', frame)]}
      </Term>
      <Caption
        text="Driftwire: send files to the computer next to you."
        source="Shown in driftwire, its command line. MediaViewer's Driftwire window runs the same engine."
        opacity={settle(frame, 6)}
      />
    </>
  );
};

const Nearby: React.FC = () => (
  <>
    <Term title={`${SENDER} — zsh`}>{(cols) => lines(cols, 'idle: Nearby, the prompt, its hints')}</Term>
    <Caption text="It sees the computers nearby, on Wi-Fi or Bluetooth." opacity={settle(useCurrentFrame(), 4)} />
  </>
);

// Two screens: the sender types, the receiver shows the code.
const Code: React.FC = () => {
  const frame = useCurrentFrame();
  const vertical = useVertical();
  const {width} = useVideoConfig();
  const size = 30;
  const sender = frame < f(CODE_WAIT) ? lines(44, 'send: type the code (empty)') : [lines(44, 'waiting lines')[1]];
  const wells = [
    {title: `${SENDER} — sending`, body: sender},
    {title: `${RECEIVER} — receiving`, body: recv(44, 'receive: the code to type over there')},
  ];
  return (
    <>
      <div
        style={{
          position: 'absolute',
          left: 0,
          width,
          top: vertical ? 440 : 130,
          display: 'flex',
          flexDirection: vertical ? 'column-reverse' : 'row',
          alignItems: vertical ? 'center' : 'flex-start',
          justifyContent: 'center',
          gap: 40,
        }}
      >
        {wells.map((w) => (
          <TerminalWell key={w.title} title={w.title} fontSize={size} cols={44}>
            <Block lines={w.body} />
          </TerminalWell>
        ))}
      </div>
      <Caption text="Type the code the other screen shows. Nothing moves until they accept." opacity={settle(frame, 4)} />
    </>
  );
};

const Offer: React.FC = () => (
  <>
    <Term title={`${RECEIVER} — receiving`}>{(cols) => recv(cols, 'the offer')}</Term>
    <Caption text="They see what's coming, and choose where it goes." opacity={settle(useCurrentFrame(), 4)} />
  </>
);

const Drop: React.FC = () => {
  const frame = useCurrentFrame();
  const t = frame / FPS;
  const state =
    t < DROP.reconnect ? 'sending: Wi-Fi, 42 %' : t < DROP.bluetooth ? 'sending: reconnecting' : t < DROP.wifi ? 'sending: Bluetooth' : 'sending: Wi-Fi, 42 %';
  return (
    <>
      <Term title={`${SENDER} — sending`} top={useVertical() ? 700 : 330}>{(cols) => sending(cols, state, frame)}</Term>
      <Caption text="Wi-Fi drops. It keeps going over Bluetooth, and moves back when Wi-Fi returns." source="No new code, and nothing is sent twice." opacity={settle(frame, 4)} />
    </>
  );
};

const Done: React.FC = () => {
  const frame = useCurrentFrame();
  const vertical = useVertical();
  return (
    <>
      <Term title={`${SENDER} — sending`} top={vertical ? 470 : 90}>{(cols) => sending(cols, 'sending: 100 %', frame)}</Term>
      {frame >= f(1.5) ? (
        <Term title={`${RECEIVER} — receiving`} top={vertical ? 900 : 520}>
          {(cols) => recv(cols, 'transcript').slice(0, cols === 44 ? 3 : 2)}
        </Term>
      ) : null}
      <Caption text="Every file is checked as it lands, and read back before it appears." opacity={settle(frame, 4)} />
    </>
  );
};

const Anywhere: React.FC = () => {
  const frame = useCurrentFrame();
  const vertical = useVertical();
  const words = frame < f(4.5);
  return (
    <>
      <Term title={words ? `${RECEIVER} — receiving` : `${SENDER} — sending`} top={words ? undefined : vertical ? 700 : 330}>
        {(cols) => (words ? lines(cols, '/code: four words') : sending(cols, 'sending: met on the DHT', frame))}
      </Term>
      <Caption
        text="Anywhere, with a code. It works once, for ten minutes, with no server in between."
        source="Four words when the two meet over the public DHT, which is off until you turn it on."
        opacity={settle(frame, 4)}
      />
    </>
  );
};

const Remember: React.FC = () => {
  const frame = useCurrentFrame();
  const ask = frame < f(3);
  return (
    <>
      <Term title={`${RECEIVER} — receiving`}>
        {(cols) => (ask ? lines(cols, 'remember: ask') : recv(cols, 'receive: a remembered device (no code)'))}
      </Term>
      <Caption text="Remember each other: next time, no code. It refuses if a key ever changes." opacity={settle(frame, 4)} />
    </>
  );
};

const Relay: React.FC = () => {
  const frame = useCurrentFrame();
  return (
    <>
      <Term title={`${SENDER} — sending`} top={useVertical() ? 700 : 330}>{(cols) => sending(cols, 'sending: through a relay', frame)}</Term>
      <Caption text="Through a relay that can't read it." source="Another computer of yours, or a relay server you run. Sealed end to end." opacity={settle(frame, 4)} />
    </>
  );
};

const WindowBeat: React.FC = () => {
  const vertical = useVertical();
  const {width} = useVideoConfig();
  const w = vertical ? width - 260 : 600;
  return (
    <>
      <Window src="driftwire-window.png" aspect={DRIFT} blurs={DRIFT_BLURS} box={{left: (width - w) / 2, top: vertical ? 420 : 40, width: w}} />
      <Caption text="The same engine, in MediaViewer's Driftwire window." source="Computer names covered." opacity={settle(useCurrentFrame(), 4)} />
    </>
  );
};

const SCENES: Record<string, React.FC> = {
  open: Open, nearby: Nearby, code: Code, diagram: CodeDiagram, locks: Locks, offer: Offer, drop: Drop,
  done: Done, anywhere: Anywhere, remember: Remember, relay: Relay, window: WindowBeat, close: CloseB,
};

export const FilmB: React.FC = () => (
  <Stage>
    {FILM_B.map((b) => {
      const S = SCENES[b.id];
      return (
        <Sequence key={b.id} from={f(b.start)} durationInFrames={f(b.end) - f(b.start)}>
          <Stage>
            <S />
          </Stage>
        </Sequence>
      );
    })}
  </Stage>
);
