// Both films' beats and sound cues in seconds; FilmA.tsx / FilmB.tsx draw from these and
// scripts/sound.mjs reads the same file through scripts/cues.mjs, so picture and sound share
// one clock (PROMPT.md "Keep motion, audio cues, and transitions on one timeline").
export type Beat = {id: string; start: number; end: number};
export type Cue = {t: number; kind: 'cut' | 'key' | 'tick' | 'chime' | 'drop' | 'chirp' | 'swell' | 'glide' | 'glideEnd'};

export const FILM_A: Beat[] = [
  {id: 'hook', start: 0, end: 4.5},
  {id: 'pan', start: 4.5, end: 10},
  {id: 'browse', start: 10, end: 15},
  {id: 'gallery', start: 15, end: 18},
  {id: 'video', start: 18, end: 23.5},
  {id: 'search', start: 23.5, end: 30},
  {id: 'metadata', start: 30, end: 34.5},
  {id: 'edit', start: 34.5, end: 40},
  {id: 'editor', start: 40, end: 44},
  {id: 'driftwire', start: 44, end: 47.5},
  {id: 'close', start: 47.5, end: 53},
];

export const BROWSE_STEPS = [0.6, 1.6, 2.6, 3.6]; // seconds into 'browse' where → is pressed
export const PAN_CUT = 2.0; // seconds into 'pan' where the drag lands

export const CUES_A: Cue[] = [
  {t: 0.35, kind: 'tick'},
  ...FILM_A.slice(1).map((b) => ({t: b.start, kind: 'cut' as const})),
  {t: 4.5 + PAN_CUT, kind: 'tick'},
  ...BROWSE_STEPS.map((s) => ({t: 10 + s, kind: 'key' as const})),
  {t: 15.1, kind: 'key'},
  {t: 34.5 + 2.7, kind: 'tick'},
  {t: 47.5, kind: 'swell'},
];

export const FILM_B: Beat[] = [
  {id: 'open', start: 0, end: 6},
  {id: 'nearby', start: 6, end: 13},
  {id: 'code', start: 13, end: 22},
  {id: 'diagram', start: 22, end: 32},
  {id: 'locks', start: 32, end: 40},
  {id: 'offer', start: 40, end: 46},
  {id: 'drop', start: 46, end: 60},
  {id: 'done', start: 60, end: 66},
  {id: 'anywhere', start: 66, end: 75},
  {id: 'remember', start: 75, end: 81},
  {id: 'relay', start: 81, end: 85},
  {id: 'window', start: 85, end: 91},
  {id: 'close', start: 91, end: 96},
];

// Inside 'drop': Wi-Fi, then the link drops, Bluetooth carries on, Wi-Fi comes back.
export const DROP = {reconnect: 4, bluetooth: 7, wifi: 11};
export const CODE_WAIT = 5.5; // inside 'code': the sender has typed it and waits

export const CUES_B: Cue[] = [
  {t: 0, kind: 'glide'},
  {t: 6, kind: 'glideEnd'},
  ...FILM_B.slice(1).map((b) => ({t: b.start, kind: 'cut' as const})),
  ...[0, 0.3, 0.6, 0.9, 1.2, 1.5].map((d) => ({t: 13 + 3.5 + d, kind: 'key' as const})),
  {t: 13 + CODE_WAIT, kind: 'tick'},
  // the code diagram's own beats (CodeDiagram.tsx: frames 30, 75, 120, 165, 210, 240)
  ...[1, 2.5, 4, 5.5, 7, 8].map((d) => ({t: 22 + d, kind: 'tick' as const})),
  ...[1.47, 2.47].map((d) => ({t: 32 + d, kind: 'tick' as const})),
  {t: 44, kind: 'chime'},
  {t: 46, kind: 'glide'},
  {t: 46 + DROP.reconnect, kind: 'drop'},
  {t: 46 + DROP.bluetooth, kind: 'chirp'},
  {t: 46 + DROP.wifi, kind: 'tick'},
  {t: 60, kind: 'glideEnd'},
  {t: 61.5, kind: 'chime'},
  {t: 70.5, kind: 'glide'},
  {t: 75, kind: 'glideEnd'},
  {t: 78, kind: 'tick'},
  {t: 91, kind: 'swell'},
];

export const seconds = (beats: Beat[]) => beats[beats.length - 1].end;
