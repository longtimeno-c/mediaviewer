import {useState} from 'react';
import {continueRender, delayRender, staticFile} from 'remotion';

let loaded: Promise<void> | null = null;

function load(): Promise<void> {
  if (!loaded) {
    const face = new FontFace('Cozette', `url(${staticFile('CozetteVector.ttf')})`);
    loaded = face.load().then((f) => { document.fonts.add(f); });
  }
  return loaded;
}

// Holds the render until Cozette is in, so no frame is drawn in a fallback face.
export function useCozette(): void {
  const [handle] = useState(() => {
    const h = delayRender('Cozette');
    load().then(() => continueRender(h)).catch(() => continueRender(h));
    return h;
  });
  void handle;
}
