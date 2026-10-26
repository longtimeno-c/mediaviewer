import React from 'react';
import {AbsoluteFill, useVideoConfig} from 'remotion';
import {C, FONT, SAFE, SIZE} from '../tokens';
import {useCozette} from './useFont';

export const useVertical = () => {
  const {width, height} = useVideoConfig();
  return height > width;
};

// The charcoal ground every shot sits on.
export const Stage: React.FC<{children: React.ReactNode}> = ({children}) => {
  useCozette();
  return (
    <AbsoluteFill style={{background: C.canvas, color: C.text, fontFamily: FONT, fontSize: SIZE.small}}>
      {children}
    </AbsoluteFill>
  );
};

// Caption: bottom-left in 16:9, above the picture in 9:16 (style-guide.md "Layout").
export const Caption: React.FC<{text: string; source?: string; opacity?: number}> = ({text, source, opacity = 1}) => {
  const vertical = useVertical();
  return (
    <div
      style={{
        position: 'absolute',
        left: SAFE,
        right: SAFE,
        ...(vertical ? {top: SAFE * 2} : {bottom: SAFE}),
        opacity,
      }}
    >
      <div style={{fontSize: vertical ? SIZE.large : SIZE.body, lineHeight: 1.3, maxWidth: vertical ? undefined : 1300}}>{text}</div>
      {source ? (
        <div style={{fontSize: vertical ? SIZE.body : SIZE.small, color: C.dim, marginTop: 12, lineHeight: 1.3}}>{source}</div>
      ) : null}
    </div>
  );
};
