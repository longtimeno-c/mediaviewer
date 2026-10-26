// Film A, MediaViewer (shotlist.md, retimed to the captures taken 2026-10-26). Every screen is a
// real capture of MediaViewer 0.1.37 on the Mac with the repo's CC0 samples; every number is
// in brief.md with its source, measured on Windows.
import React from 'react';
import {Sequence, interpolate, useCurrentFrame, useVideoConfig} from 'remotion';
import {C, FPS, SAFE, SIZE} from '../tokens';
import {Caption, Stage, useVertical} from '../parts/Stage';
import {settle} from '../parts/motion';
import {DRIFT, EDITOR, MAIN, Window} from '../parts/Window';
import {Keycap, Readout} from '../parts/Readout';
import {CloseA} from '../scenes/Closes';
import {BROWSE_STEPS, FILM_A, PAN_CUT} from './timeline';

const f = (s: number) => Math.round(s * FPS);

// Where a full window sits in each format.
const useBox = (aspect = MAIN) => {
  const vertical = useVertical();
  const {width} = useVideoConfig();
  if (vertical) return {left: 32, top: 420, width: width - 64};
  const w = Math.min(1400, 818 * aspect);
  return {left: (width - w) / 2, top: 40, width: w};
};

// 9:16 shows the middle of the window (the picture), larger, instead of the whole window small.
const VCROP = {x: 0.18, y: 0, w: 0.64, h: 1};
const VCROP_PANE = {x: 0.36, y: 0, w: 0.64, h: 1};
const useCrop = (pane = false) => (useVertical() ? (pane ? VCROP_PANE : VCROP) : undefined);
const UNDER = 1380; // 9:16: readouts and keycaps go under the cropped window

// Readouts sit over the window's top-left in 16:9, under it in 9:16.
const ReadoutSpot: React.FC<{children: React.ReactNode}> = ({children}) => {
  const vertical = useVertical();
  const box = useBox();
  return (
    <div style={{position: 'absolute', left: vertical ? SAFE : box.left + 28, top: vertical ? UNDER : box.top + 90}}>{children}</div>
  );
};

const Hook: React.FC = () => (
  <>
    <Window src="browse-5.png" aspect={MAIN} crop={useCrop()} box={useBox()} push={{from: 1, to: 1.03}} />
    <ReadoutSpot>
      <Readout
        rows={[
          {label: 'preview', value: 51, unit: 'ms'},
          {label: 'full res', value: 1.23, decimals: 2, unit: 's'},
        ]}
        source="sony_ilce7rm3.arw · 42 MP RAW"
      />
    </ReadoutSpot>
    <Caption text="A 42-megapixel RAW, on screen in 51 ms." source="Measured on Windows. docs/perf/first-pixel" opacity={settle(useCurrentFrame(), 4)} />
  </>
);

const Pan: React.FC = () => {
  const frame = useCurrentFrame();
  return (
    <>
      <Window src={frame < f(PAN_CUT) ? 'actual.png' : 'pan-039.png'} aspect={MAIN} crop={useCrop()} box={useBox()} />
      <ReadoutSpot>
        <Readout
          rows={[
            {label: 'dropped', value: 0, suffix: ' frames'},
            {label: 'frames', value: 3597, grouped: true},
            {label: 'p99', value: 17.25, decimals: 2, unit: 'ms'},
            {label: 'refresh', value: 16.68, decimals: 2, unit: 'ms'},
          ]}
          source="60 s panning at 100 %"
        />
      </ReadoutSpot>
      <Caption text="Pan it at 100 %. Not one dropped frame in a minute." source="Measured on Windows. docs/perf/pan-soak-42mp-arw.json" opacity={settle(frame, 4)} />
    </>
  );
};

const Browse: React.FC = () => {
  const frame = useCurrentFrame();
  const vertical = useVertical();
  const box = useBox();
  const idx = BROWSE_STEPS.filter((s) => frame >= f(s)).length;
  const src = ['browse-0.png', 'browse-1.png', 'browse-2.png', 'browse-3.png', 'browse-3.png'][idx];
  return (
    <>
      <Window src={src} aspect={MAIN} crop={useCrop()} box={box} />
      <div style={{position: 'absolute', right: vertical ? SAFE : box.left + 28, top: vertical ? UNDER : box.top + 90, display: 'flex', gap: 12}}>
        {BROWSE_STEPS.map((s, i) => (frame >= f(s) && i === idx - 1 ? <Keycap key={i} label="→" at={f(s)} /> : null))}
      </div>
      <Caption text="RAW, HEIC and video in one strip. The next photo is ready in 0.2 ms." source="Measured on Windows. docs/perf/browse.json" opacity={settle(frame, 4)} />
    </>
  );
};

const Gallery: React.FC = () => {
  const frame = useCurrentFrame();
  const vertical = useVertical();
  const box = useBox();
  return (
    <>
      <Window src="gallery.png" aspect={MAIN} crop={useCrop()} box={box} />
      <div style={{position: 'absolute', right: vertical ? SAFE : box.left + 28, top: vertical ? UNDER : box.top + 90}}>
        <Keycap label="G" at={3} />
      </div>
      <Caption text="The whole card at a glance." opacity={settle(frame, 4)} />
    </>
  );
};

const Video: React.FC = () => (
  <>
    <Window src="video-000.png" aspect={MAIN} crop={useCrop()} box={useBox()} push={{from: 1, to: 1.05}} />
    <ReadoutSpot>
      <Readout
        rows={[
          {label: 'dropped', value: 0, suffix: ' of 3,597'},
          {label: 'A/V p99', value: 7.2, decimals: 1, unit: 'ms'},
        ]}
        source="60 s of 1080p HEVC"
      />
    </ReadoutSpot>
    <Caption text="Video on the same canvas. The sound is the clock." source="Measured on Windows. docs/perf/investigation" opacity={settle(useCurrentFrame(), 4)} />
  </>
);

// Local search, from its own evaluation (docs/perf/search.json at 24,992 photos). The search
// window on this Mac shows the owner's library, so the film shows the measurement, not the window.
const SEARCH = [
  {label: 'An exact description finds its photo', high: 483 / 500, fast: 470 / 500},
  {label: 'Top five right, described searches', high: 1, fast: 0.943},
];

const Search: React.FC = () => {
  const frame = useCurrentFrame();
  const vertical = useVertical();
  const {width} = useVideoConfig();
  const barW = vertical ? width - SAFE * 2 - 200 : 900;
  return (
    <>
      <div style={{position: 'absolute', left: vertical ? SAFE : 260, top: vertical ? 470 : 150, right: SAFE}}>
        <div style={{fontSize: vertical ? SIZE.large : SIZE.hero, lineHeight: 1.15, opacity: settle(frame, 0)}}>
          Find anything{vertical ? <br /> : ' '}by describing it.
        </div>
        <div style={{fontSize: SIZE.body, color: C.dim, marginTop: 24, opacity: settle(frame, 8)}}>
          “guy on a skateboard” · “giraffes” · “a dog”
        </div>
        <div style={{marginTop: vertical ? 90 : 70}}>
          {SEARCH.map((m, i) => (
            <div key={m.label} style={{marginBottom: 44, opacity: settle(frame, 16 + i * 8)}}>
              <div style={{fontSize: vertical ? SIZE.body : SIZE.small, color: C.text, marginBottom: 14}}>{m.label}</div>
              {(['high', 'fast'] as const).map((k) => {
                const v = m[k] * interpolate(frame, [20 + i * 8, 40 + i * 8], [0, 1], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'});
                return (
                  <div key={k} style={{display: 'flex', alignItems: 'center', gap: 18, marginBottom: 10}}>
                    <div style={{width: 120, fontSize: SIZE.small, color: C.dim}}>{k === 'high' ? 'High' : 'Fast'}</div>
                    <div style={{width: barW * v, height: 22, background: k === 'high' ? C.accent : C.dim, borderRadius: 4}} />
                    <div style={{fontSize: SIZE.small}}>{Math.round(v * 100)} %</div>
                  </div>
                );
              })}
            </div>
          ))}
        </div>
      </div>
      <Caption text="Computed on your computer. Nothing is uploaded." source="24,992 captioned COCO photos. docs/perf/search.json" opacity={settle(frame, 4)} />
    </>
  );
};

const Metadata: React.FC = () => {
  const vertical = useVertical();
  const {width} = useVideoConfig();
  const crop = {x: 0.42, y: 0.0, w: 0.58, h: 0.6};
  const w = vertical ? width - 64 : 1240;
  return (
    <>
      <Window src="metadata.png" aspect={MAIN} crop={crop} box={{left: (width - w) / 2, top: vertical ? 420 : 50, width: w}} />
      <Caption text="Know your shot: every EXIF, IPTC and XMP tag, and you can edit any of them." opacity={settle(useCurrentFrame(), 4)} />
    </>
  );
};

const Edit: React.FC = () => {
  const frame = useCurrentFrame();
  return (
    <>
      <Window src={frame < f(2.7) ? 'edit-crop.png' : 'edit-colour.png'} aspect={MAIN} crop={useCrop(true)} box={useBox()} push={{from: 1, to: 1.15, origin: '100% 30%'}} />
      <Caption text="Crop, straighten, colour. Originals are never touched." opacity={settle(frame, 4)} />
    </>
  );
};

const Editor: React.FC = () => (
  <>
    <Window src="video-editor.png" aspect={EDITOR} box={useBox(EDITOR)} push={{from: 1, to: 1.04, origin: '50% 80%'}} />
    <Caption text="Cut a clip in seconds. Every result is a new file." opacity={settle(useCurrentFrame(), 4)} />
  </>
);

// Driftwire's window names this computer and the last peer: covered.
export const DRIFT_BLURS = [
  {x: 0.515, y: 0.077, w: 0.385, h: 0.032},
  {x: 0.795, y: 0.27, w: 0.185, h: 0.12},
];

const Drift: React.FC = () => {
  const vertical = useVertical();
  const {width} = useVideoConfig();
  const w = vertical ? width - 260 : 600;
  return (
    <>
      <Window src="driftwire-window.png" aspect={DRIFT} blurs={DRIFT_BLURS} box={{left: (width - w) / 2, top: vertical ? 420 : 40, width: w}} />
      <Caption text="Send to the computer next to you." source="The Driftwire add-on. Computer names covered." opacity={settle(useCurrentFrame(), 4)} />
    </>
  );
};

const SCENES: Record<string, React.FC> = {
  hook: Hook, pan: Pan, browse: Browse, gallery: Gallery, video: Video, search: Search,
  metadata: Metadata, edit: Edit, editor: Editor, driftwire: Drift, close: CloseA,
};

export const FilmA: React.FC = () => (
  <Stage>
    {FILM_A.map((b) => {
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
