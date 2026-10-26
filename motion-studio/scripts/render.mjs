// Renders the films: each composition silent from one bundle, then muxed with its synthesized
// sound into out/<film>-<fmt>.mp4, plus a poster (the last frame). Usage:
//   node scripts/render.mjs [FilmA|FilmB] [16x9|9x16]
import {bundle} from '@remotion/bundler';
import {renderMedia, renderStill, selectComposition} from '@remotion/renderer';
import {execFileSync} from 'node:child_process';
import path from 'node:path';

const films = process.argv[2] ? [process.argv[2]] : ['FilmA', 'FilmB'];
const fmts = process.argv[3] ? [process.argv[3]] : ['16x9', '9x16'];
const FF = '/opt/homebrew/bin/ffmpeg';
const serveUrl = await bundle({entryPoint: path.resolve('src/index.ts')});
for (const film of films) {
  execFileSync('node', ['scripts/sound.mjs', film], {stdio: 'inherit'});
  for (const fmt of fmts) {
    const id = `${film}-${fmt}`;
    const composition = await selectComposition({serveUrl, id});
    const silent = `out/${id}.silent.mp4`;
    await renderMedia({composition, serveUrl, codec: 'h264', crf: 20, outputLocation: silent, muted: true,
      onProgress: ({progress}) => process.stdout.write(`\r${id} ${Math.round(progress * 100)} %`)});
    console.log();
    execFileSync(FF, ['-hide_banner', '-loglevel', 'error', '-y', '-i', silent, '-i', `out/${film}.wav`,
      '-c:v', 'copy', '-af', 'loudnorm=I=-16:TP=-1.5:LRA=11', '-ar', '48000', '-c:a', 'aac', '-b:a', '160k', '-shortest', '-movflags', '+faststart', `out/${id}.mp4`]);
    await renderStill({composition, serveUrl, output: `out/${id}-poster.png`, frame: composition.durationInFrames - 1});
    console.log(`out/${id}.mp4`);
  }
}
