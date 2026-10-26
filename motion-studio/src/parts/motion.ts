import {interpolate, spring} from 'remotion';
import {FPS, SETTLE} from '../tokens';

// 0 → 1 on the settle spring from frame `at` (style-guide.md "Cards, caption plates").
export const settle = (frame: number, at: number) => spring({frame: frame - at, fps: FPS, config: SETTLE});

// 0 or 1: UI state snaps (style-guide.md "UI state").
export const snap = (frame: number, at: number) => (frame >= at ? 1 : 0);

// Linear 0 → 1 over `len` frames from `at`, clamped.
export const ramp = (frame: number, at: number, len: number) =>
  interpolate(frame, [at, at + len], [0, 1], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'});
