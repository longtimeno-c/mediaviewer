// Film B beat 14: the driftwire command line, drawn from its own renderer's output
// (assets/terminal/gallery-*.ansi via scripts/ansi-to-json.mjs). Scrollback above, the live
// region below, as the CLI lays it out (docs/design/28 "From a terminal").
import React from 'react';
import {useCurrentFrame} from 'remotion';
import {C, SAFE, SIZE} from '../tokens';
import {Caption, Stage, useVertical} from '../parts/Stage';
import {settle} from '../parts/motion';
import {Section, Span, TermLine, TerminalWell, plain} from '../parts/Terminal';
import wide from '../terminal-90.json';
import narrow from '../terminal-44.json';

const COMMAND = 'driftwire send ~/Pictures/trip';
// The live region moves through these states; each holds until the next.
const LIVE: {at: number; title: string}[] = [
  {at: 34, title: 'idle: Nearby, the prompt, its hints'},
  {at: 62, title: 'send: type the code (empty)'},
  {at: 92, title: 'waiting lines'},
  {at: 118, title: 'sending: Wi-Fi, 42 %'},
];

const find = (set: Section[], title: string) => {
  const s = set.find((x) => x.title === title);
  if (!s) throw new Error(`no gallery section "${title}"`);
  return s.lines;
};

export const TerminalBeat: React.FC = () => {
  const frame = useCurrentFrame();
  const vertical = useVertical();
  const set = (vertical ? narrow : wide) as Section[];
  // 9:16: 34 px keeps 44 Menlo columns inside the safe margin (the one size under the 39 px floor).
  const fontSize = vertical ? 34 : SIZE.small;

  const typed = Math.max(0, Math.min(COMMAND.length, Math.floor((frame - 4) / 0.8)));
  const prompt: Span[] = [...plain('$ ', C.dim), ...plain(COMMAND.slice(0, typed))];

  let live: Span[][] = [];
  for (const l of LIVE) if (frame >= l.at) live = find(set, l.title);
  // The waiting state shows its one line for this send.
  if (live === find(set, 'waiting lines')) live = [live[1]];

  return (
    <Stage>
      <div style={{position: 'absolute', left: 0, right: 0, top: vertical ? 560 : 190, display: 'flex', justifyContent: 'center'}}>
        <TerminalWell title="zsh — driftwire" fontSize={fontSize} cols={vertical ? 44 : 90}>
          <TermLine spans={prompt} />
          {frame >= 30 ? (
            <>
              <TermLine spans={[]} />
              {find(set, 'start banner (printed once)').map((l, i) => <TermLine key={`b${i}`} spans={l} />)}
              <TermLine spans={[]} />
            </>
          ) : null}
          {live.map((l, i) => <TermLine key={`l${i}`} spans={l} />)}
        </TerminalWell>
      </div>
      <Caption text="From a terminal, too." source="Linux, macOS and Windows. Nothing is saved until you answer." opacity={settle(frame, 12)} />
    </Stage>
  );
};
