import React from 'react';
import {Composition} from 'remotion';
import {FPS} from './tokens';
import {CodeDiagram} from './scenes/CodeDiagram';
import {Locks} from './scenes/Locks';
import {TerminalBeat} from './scenes/TerminalBeat';
import {CloseA, CloseB} from './scenes/Closes';
import {FilmA} from './films/FilmA';
import {FilmB} from './films/FilmB';
import {FILM_A, FILM_B, seconds} from './films/timeline';

// Every scene is one composition per format (PROMPT.md: 16:9 and 9:16 are separate
// compositions, never a crop). Ids are <film beat>-<name>-<format>.
const SCENES: {id: string; component: React.FC; seconds: number}[] = [
  {id: 'B05-code', component: CodeDiagram, seconds: 10},
  {id: 'B06-locks', component: Locks, seconds: 8},
  {id: 'B14-terminal', component: TerminalBeat, seconds: 6},
  {id: 'A12-close', component: CloseA, seconds: 5},
  {id: 'B15-close', component: CloseB, seconds: 4},
  {id: 'FilmA', component: FilmA, seconds: seconds(FILM_A)},
  {id: 'FilmB', component: FilmB, seconds: seconds(FILM_B)},
];

const FORMATS = [
  {name: '16x9', width: 1920, height: 1080},
  {name: '9x16', width: 1080, height: 1920},
];

export const Root: React.FC = () => (
  <>
    {SCENES.flatMap((s) =>
      FORMATS.map((f) => (
        <Composition
          key={`${s.id}-${f.name}`}
          id={`${s.id}-${f.name}`}
          component={s.component}
          durationInFrames={Math.round(s.seconds * FPS)}
          fps={FPS}
          width={f.width}
          height={f.height}
        />
      )),
    )}
  </>
);
