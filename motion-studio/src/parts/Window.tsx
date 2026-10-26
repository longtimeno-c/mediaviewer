// A real capture of a MediaViewer window (public/caps, taken from the app on the CC0 sample
// folder), on the canvas with a hairline edge: no device mockup, no shadow (style-guide.md).
import React from 'react';
import {Img, interpolate, staticFile, useCurrentFrame, useVideoConfig} from 'remotion';
import {C} from '../tokens';

// A solid redaction box over a capture (a blur stayed readable): fractions of the capture.
export type Blur = {x: number; y: number; w: number; h: number};

export const Window: React.FC<{
  src: string;
  aspect: number; // capture width / height
  box: {left: number; top: number; width: number};
  push?: {from: number; to: number; origin?: string}; // slow camera, ≤ 1.6× (style-guide.md)
  blurs?: Blur[];
  crop?: {x: number; y: number; w: number; h: number}; // show only part of the capture
}> = ({src, aspect, box, push, blurs = [], crop}) => {
  const frame = useCurrentFrame();
  const {durationInFrames} = useVideoConfig();
  const z = push ? interpolate(frame, [0, durationInFrames], [push.from, push.to], {extrapolateRight: 'clamp'}) : 1;
  const c = crop ?? {x: 0, y: 0, w: 1, h: 1};
  const height = (box.width / (aspect * (c.w / c.h)));
  const innerW = box.width / c.w;
  const innerH = innerW / aspect;
  return (
    <div
      style={{
        position: 'absolute',
        left: box.left,
        top: box.top,
        width: box.width,
        height,
        overflow: 'hidden',
        border: `1px solid ${C.hairline}`,
        borderRadius: 12,
        background: '#000',
      }}
    >
      <div
        style={{
          position: 'absolute',
          left: -c.x * innerW,
          top: -c.y * innerH,
          width: innerW,
          height: innerH,
          transform: `scale(${z})`,
          transformOrigin: push?.origin ?? '50% 50%',
        }}
      >
        <Img src={staticFile(`caps/${src}`)} style={{width: '100%', height: '100%', display: 'block'}} />
        {blurs.map((b, i) => (
          <div
            key={i}
            style={{
              position: 'absolute',
              left: `${b.x * 100}%`,
              top: `${b.y * 100}%`,
              width: `${b.w * 100}%`,
              height: `${b.h * 100}%`,
              background: '#3a3a3d',
              borderRadius: 6,
            }}
          />
        ))}
      </div>
    </div>
  );
};

// Captures are 2560×1496 (main window), 2360×1696 (Video Editor), 1120×1416 (Driftwire).
export const MAIN = 2560 / 1496;
export const EDITOR = 2360 / 1696;
export const DRIFT = 1120 / 1416;
