// style-guide.md, as code. Every colour on screen comes from here.
export const C = {
  canvas: '#21232A',
  well: '#191A1F',
  surface: '#2A2D35',
  hairline: '#3A3E48',
  text: '#DCDEE4',
  dim: '#8E8E93',
  accent: '#40C8E0',
  shine: '#A9E6F1',
  bt: '#0A84FF',
  net: '#5E5CE6',
  dht: '#FF9F0A',
  relay: '#BF5AF2',
  ok: '#30D158',
  err: '#FF453A',
  warn: '#FFD60A',
} as const;

export const FONT = 'Cozette, Menlo, monospace';
// Terminal shots use a terminal face: Cozette has no rounded box corners or eighth blocks.
export const TERM_FONT = 'Menlo, monospace';

// Cozette is drawn on a 13 px grid; integer multiples stay crisp.
export const SIZE = {small: 26, body: 39, large: 52, hero: 78} as const;

// The app's pan/zoom spring (ω = 18, ζ = 1) in Remotion's terms.
export const SETTLE = {mass: 1, stiffness: 18 * 18, damping: 2 * 18, overshootClamping: true};

export const FPS = 30;
export const SAFE = 64;
