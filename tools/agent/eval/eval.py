#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""The edit eval set (docs/design/29 "Eval"): how well an agent edits a timeline
through `MediaViewer --mcp`, measured, so "edits well" is a number that moves
when the instructions, a skill or a tool changes.

Each case in cases.json is an interview (or a few) synthesized phrase by phrase
with macOS `say`, so every line, filler, retake and pause has exact ground-truth
times, plus a brief and the checks a reference cut passes.

    eval.py build --lab DIR                      # media, projects, ground truth
    eval.py index --lab DIR --bench build/bin/ai-bench --addons ADDONS
    eval.py run   --lab DIR --app build/bin/MediaViewer --addons ADDONS --config skills|plain
                  [--skills src/addons/agent/skills] [--cases id,id] [--model opus]
    eval.py score --lab DIR --timeline-tool build/bin/mv-timeline --config skills|plain [--json OUT]
    eval.py reference --lab DIR --timeline-tool build/bin/mv-timeline
                  # a reference cut from the truth where one follows from it (config "reference"):
                  # scores 1.0 by construction, which checks the scorer

`run` drives Claude Code (`claude -p`, a strict MCP config per case, only the
mediaviewer tools allowed) in a scratch folder per case; `plain` gives it no
skills, `skills` puts the add-on's skills in that folder's .claude/skills.
`score` reads the version each run committed and checks it against the truth.

Scores, each 0..1, a case's score their mean:
  boundaries   every cut edge lies in silence, never inside a spoken phrase
  keep         lines that had to stay are whole (all lines, or the brief's musts)
  fillers      the ums and uhs are gone (when the brief asked)
  bad          fluffed takes and restarts are gone (when asked)
  pauses       pauses over the limit are cut to it (+150 ms), others left
  kept_pause   the pause the brief said to keep is still there
  length       within the brief's tolerance of its target length
  order        the selected lines come in the brief's order, and nothing else
  broll        every jump cut is covered by a connected clip; every clip from the scope
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.parse
import wave
import zlib
from fractions import Fraction as F
from pathlib import Path

HERE = Path(__file__).resolve().parent
RATE = 48000
FPS = 25


# ---- build ---------------------------------------------------------------------------------

def synth(text: str, voice: str, out: Path) -> list[int]:
    subprocess.run(["say", "-v", voice, "--data-format=LEI16@48000", "-o", str(out), text], check=True)
    with wave.open(str(out)) as w:
        assert w.getframerate() == RATE and w.getsampwidth() == 2
        raw = w.readframes(w.getnframes())
        ch = w.getnchannels()
    samples = [int.from_bytes(raw[i:i + 2], "little", signed=True) for i in range(0, len(raw), 2 * ch)]
    return samples


def speech_bounds(samples: list[int], threshold: int = 330) -> tuple[int, int]:
    first = next((i for i, s in enumerate(samples) if abs(s) > threshold), 0)
    last = next((i for i in range(len(samples) - 1, -1, -1) if abs(samples[i]) > threshold), len(samples) - 1)
    return first, last + 1


def write_wav(path: Path, samples: list[int]) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(int(s).to_bytes(2, "little", signed=True) for s in samples))


def build_clip(clip: dict, folder: Path, tmp: Path, colour: str) -> dict:
    audio: list[int] = []
    truth = []
    for n, seg in enumerate(clip["segments"]):
        if "pause" in seg:
            at = len(audio)
            audio += [0] * int(seg["pause"] * RATE)
            truth.append({"k": "pause", "start": at / RATE, "end": len(audio) / RATE, "len": seg["pause"],
                          "keep": bool(seg.get("keep"))})
            continue
        s = synth(seg["t"], clip["voice"], tmp / f"{clip['name']}-{n}.wav")
        a, b = speech_bounds(s)
        base = len(audio)
        audio += s
        truth.append({"k": seg["k"], "t": seg["t"], "start": (base + a) / RATE, "end": (base + b) / RATE,
                      "must": bool(seg.get("must")), "order": seg.get("order")})
    audio += [0] * int(0.3 * RATE)
    wav = tmp / f"{clip['name']}.wav"
    write_wav(wav, audio)
    length = len(audio) / RATE
    mov = folder / f"{clip['name']}.mov"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", f"color=c={colour}:s=1280x720:r={FPS}",
                    "-i", str(wav), "-t", f"{length:.3f}", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
                    "-ar", "48000", str(mov)], check=True)
    dur = float(subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0",
                                str(mov)], capture_output=True, text=True).stdout)
    return {"name": clip["name"], "path": str(mov), "duration": dur, "segments": truth}


def t(x) -> str:
    x = F(x).limit_denominator(1000000)
    return f"{x.numerator}s" if x.denominator == 1 else f"{x.numerator}/{x.denominator}s"


def frames(s: float) -> F:
    return F(round(s * FPS), FPS)


def project(case_id: str, clips: list[dict], pieces: list[tuple[int, float, float]]) -> str:
    url = lambda p: "file://" + urllib.parse.quote(p)
    res = ['<format id="r1" name="FFVideoFormat720p25" frameDuration="1/25s" width="1280" height="720" '
           'colorSpace="1-1-1 (Rec. 709)"/>']
    for i, c in enumerate(clips):
        res.append(f'<asset id="r{i + 2}" name="{c["name"]}" uid="EVAL{i:04d}{zlib.crc32(case_id.encode()) % 10000:04d}" '
                   f'start="0s" duration="{t(frames(c["duration"]))}" hasVideo="1" format="r1" hasAudio="1" '
                   f'audioSources="1" audioChannels="1" audioRate="48000">\n'
                   f'                <media-rep kind="original-media" src="{url(c["path"])}"/>\n            </asset>')
    spine, at = [], F(0)
    for ci, a, b in pieces:
        start, dur = frames(a), frames(b) - frames(a)
        spine.append(f'<asset-clip ref="r{ci + 2}" offset="{t(at)}" name="{clips[ci]["name"]}" start="{t(start)}" '
                     f'duration="{t(dur)}" format="r1" tcFormat="NDF" audioRole="dialogue"/>')
        at += dur
    nl = "\n            "
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE fcpxml>

<fcpxml version="1.13">
    <resources>
            {nl.join(res)}
    </resources>
    <library location="file:///Users/editor/Movies/Eval.fcpbundle/">
        <event name="Eval" uid="EVENT{zlib.crc32(case_id.encode()) % 100000:05d}">
            <project name="{case_id}" uid="PROJ{zlib.crc32(case_id.encode()) % 100000:05d}" modDate="2026-10-19 00:00:00 +0100">
                <sequence format="r1" duration="{t(at)}" tcStart="0s" tcFormat="NDF" audioLayout="stereo" audioRate="48k">
                    <spine>
                        {(nl + "            ").join(spine)}
                    </spine>
                </sequence>
            </project>
        </event>
    </library>
</fcpxml>
'''


def build(lab: Path) -> None:
    cases = json.loads((HERE / "cases.json").read_text())["cases"]
    colours = ["0x3a5a7a", "0x7a5a3a", "0x4a7a4a", "0x6a3a6a"]
    for case in cases:
        d = lab / "cases" / case["id"]
        if d.exists():
            shutil.rmtree(d)
        (d / "media").mkdir(parents=True)
        tmp = d / "tmp"
        tmp.mkdir()
        clips = [build_clip(c, d / "media", tmp, colours[i % 4]) for i, c in enumerate(case["clips"])]
        if case.get("precut"):
            # Already tightened: each line by itself, its pauses gone -> jump cuts.
            pieces = [(ci, s["start"] - 0.08, s["end"] + 0.08) for ci, c in enumerate(clips)
                      for s in c["segments"] if s["k"] != "pause"]
        else:
            pieces = [(ci, 0.0, c["duration"]) for ci, c in enumerate(clips)]
        if case.get("broll"):
            (d / "broll").mkdir()
            sources = ["mandelbrot", "smptebars", "cellauto=rule=110:s=1280x720", "life=s=1280x720:mold=10"]
            names = ["Lake", "Peak", "Trail", "Forest"]
            for i in range(case["broll"]):
                subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", sources[i % 4], "-t", "12", "-r",
                                str(FPS), "-s", "1280x720", "-c:v", "libx264", "-pix_fmt", "yuv420p",
                                str(d / "broll" / f"{names[i % 4]}.mov")], check=True)
        (d / "project.fcpxml").write_text(project(case["id"], clips, pieces))
        (d / "truth.json").write_text(json.dumps({"case": case, "clips": clips, "pieces": pieces}, indent=1))
        shutil.rmtree(tmp)
        print("built", case["id"])


def index(lab: Path, bench: str, addons: str) -> None:
    env = dict(os.environ, MV_DEV_ADDONS_DIR=addons)
    for d in sorted((lab / "cases").iterdir()):
        for folder in ("media", "broll"):
            if (d / folder).is_dir():
                r = subprocess.run([bench, "--addons", addons, "--index", str(d / folder)], env=env,
                                   capture_output=True, text=True)
                print("indexed", d.name, folder, "ok" if r.returncode == 0 else r.stderr[-300:])


# ---- run -----------------------------------------------------------------------------------

def run(lab: Path, app: str, addons: str, config: str, skills: Path | None, only: list[str], model: str) -> None:
    claude = shutil.which("claude") or str(Path.home() / ".local/bin/claude")
    for d in sorted((lab / "cases").iterdir()):
        truth = json.loads((d / "truth.json").read_text())
        case = truth["case"]
        if only and case["id"] not in only:
            continue
        w = lab / "runs" / config / case["id"]
        if w.exists():
            shutil.rmtree(w)
        w.mkdir(parents=True)
        shutil.copy(d / "project.fcpxml", w / f"{case['id']}.fcpxml")
        args = ["--mcp", "--timeline", str(w / f"{case['id']}.fcpxml"), "--no-handoff"]
        if (d / "broll").is_dir():
            args += ["--folder", str(d / "broll")]
        (w / "mcp.json").write_text(json.dumps({"mcpServers": {"mediaviewer": {
            "command": app, "args": args, "env": {"MV_DEV_ADDONS_DIR": addons}}}}))
        if config == "skills" and skills:
            shutil.copytree(skills, w / ".claude" / "skills")
        prompt = (case["brief"] + " The timeline is the Final Cut Pro project in MediaViewer (use the mediaviewer "
                  "tools). Commit one new version when you are done. Do not ask me to confirm anything.")
        cmd = [claude, "-p", prompt, "--mcp-config", "mcp.json", "--strict-mcp-config", "--allowedTools",
               "mcp__mediaviewer", "--output-format", "stream-json", "--verbose", "--model", model]
        with open(w / "session.jsonl", "w") as out:
            subprocess.run(cmd, cwd=w, stdout=out, stderr=subprocess.DEVNULL)
        cost = turns = None
        for line in open(w / "session.jsonl"):
            m = json.loads(line)
            if m.get("type") == "result":
                cost, turns = m.get("total_cost_usd"), m.get("num_turns")
        print(f"ran {case['id']} ({config}): ${cost} {turns} turns")


# ---- score ---------------------------------------------------------------------------------

LINE = re.compile(r'^(\d+) (\w+) parent (-?\d+) lane (-?\d+) (?:storyline -?\d+|anchored) at (\S+) offset \S+ '
                  r'start (\S+) duration (\S+)( disabled)? name ".*?" ref \S* role ".*?" file "(.*?)"')


def secs(s: str) -> float:
    s = s.rstrip("s")
    if "/" in s:
        a, b = s.split("/")
        return int(a) / int(b)
    return float(s)


def read_version(tool: str, path: Path) -> list[dict]:
    out = subprocess.run([tool, "describe", str(path)], capture_output=True, text=True, check=True).stdout
    items = []
    for line in out.splitlines()[1:]:
        m = LINE.match(line)
        if not m:
            continue
        items.append({"kind": m.group(2), "parent": int(m.group(3)), "lane": int(m.group(4)), "at": secs(m.group(5)),
                      "start": secs(m.group(6)), "dur": secs(m.group(7)), "enabled": not m.group(8),
                      "file": m.group(9)})
    return items


def covered(ranges: list[tuple[float, float]], a: float, b: float) -> float:
    if b <= a:
        return 1.0
    got = sum(max(0.0, min(b, y) - max(a, x)) for x, y in ranges)
    return got / (b - a)


def score_case(truth: dict, items: list[dict]) -> dict:
    case, clips, checks = truth["case"], truth["clips"], truth["case"]["checks"]
    by_path = {c["path"]: c for c in clips}
    primary = [i for i in items if i["parent"] == -1 and i["kind"] in ("clip",) and i["enabled"]]
    kept: dict[str, list[tuple[float, float]]] = {c["path"]: [] for c in clips}
    for i in primary:
        if i["file"] in kept:
            kept[i["file"]].append((i["start"], i["start"] + i["dur"]))
    s: dict[str, float] = {}
    # boundaries: each edge inside a spoken phrase (20 ms in) is a bad cut.
    edges = bad = 0
    for i in primary:
        c = by_path.get(i["file"])
        if not c:
            continue
        for e in (i["start"], i["start"] + i["dur"]):
            if e <= 0.05 or e >= c["duration"] - 0.05:
                continue
            edges += 1
            if any(seg["k"] != "pause" and seg["start"] + 0.02 < e < seg["end"] - 0.02 for seg in c["segments"]):
                bad += 1
    s["boundaries"] = 1.0 if edges == 0 else 1 - bad / edges
    lines = [(c["path"], seg) for c in clips for seg in c["segments"] if seg["k"] == "line"]
    if checks.get("keep_all_lines"):
        s["keep"] = sum(covered(kept[p], g["start"], g["end"]) >= 0.95 for p, g in lines) / max(1, len(lines))
    elif checks.get("must_keep"):
        musts = [(p, g) for p, g in lines if g["must"]]
        s["keep"] = sum(covered(kept[p], g["start"], g["end"]) >= 0.95 for p, g in musts) / max(1, len(musts))
    if checks.get("fillers_removed"):
        fill = [(c["path"], g) for c in clips for g in c["segments"] if g["k"] == "filler"]
        s["fillers"] = sum(covered(kept[p], g["start"], g["end"]) <= 0.1 for p, g in fill) / max(1, len(fill))
    if checks.get("bad_removed"):
        badt = [(c["path"], g) for c in clips for g in c["segments"] if g["k"] == "bad"]
        s["bad"] = sum(covered(kept[p], g["start"], g["end"]) <= 0.1 for p, g in badt) / max(1, len(badt))
    if "max_pause" in checks:
        # The silence left between consecutive kept speech, against the
        # silence that was there (a removal in between merges its pauses).
        limit = checks["max_pause"]
        good = total = 0
        for c in clips:
            segs = c["segments"]
            speech = [g for g in segs if g["k"] != "pause"]
            alive = [g for g in speech if covered(kept[c["path"]], g["start"], g["end"]) >= 0.5]
            for x, y in zip(alive, alive[1:]):
                between = [g for g in segs if g["k"] == "pause" and g["start"] >= x["end"] - 0.3
                           and g["end"] <= y["start"] + 0.3]
                if any(g.get("keep") for g in between):
                    continue  # kept_pause scores it
                removed = any(x["end"] <= g["start"] and g["end"] <= y["start"] for g in speech
                              if g not in alive)
                orig = sum(g["len"] for g in between) + (0 if removed else 0)
                silence_kept = sum(max(0.0, min(y["start"], b) - max(x["end"], a)) for a, b in kept[c["path"]])
                # Speech removed in between is not silence.
                silence_kept -= sum(max(0.0, min(g["end"], b) - max(g["start"], a)) for g in speech if g not in alive
                                    for a, b in kept[c["path"]] if x["end"] <= g["start"] and g["end"] <= y["start"])
                total += 1
                if orig > limit + 0.05 or removed:
                    good += silence_kept <= max(limit, 0 if not removed else min(orig, limit)) + 0.2
                else:
                    good += silence_kept >= orig - 0.2
        s["pauses"] = good / max(1, total)
    if checks.get("kept_pause"):
        for c in clips:
            segs = c["segments"]
            for n, g in enumerate(segs):
                if g["k"] == "pause" and g.get("keep"):
                    a, b = segs[n - 1]["end"], segs[n + 1]["start"]
                    s["kept_pause"] = 1.0 if covered(kept[c["path"]], a, b) >= 0.9 else 0.0
    length = sum(i["dur"] for i in primary)
    if "target_length" in checks:
        tgt, tol = checks["target_length"], checks.get("tolerance", 0.1)
        err = abs(length - tgt) / tgt
        s["length"] = 1.0 if err <= tol else max(0.0, 1 - (err - tol) / tol)
    if checks.get("order"):
        ordered = sorted([(g["order"], p, g) for p, g in lines if g.get("order")])
        # Where each ordered line plays on the edited timeline.
        def tl(p, g):
            for i in primary:
                if i["file"] == p and i["start"] <= g["start"] + 0.02 and g["end"] - 0.02 <= i["start"] + i["dur"]:
                    return i["at"] + (g["start"] - i["start"])
            return None
        pos = [tl(p, g) for _, p, g in ordered]
        s["order"] = 1.0 if all(x is not None for x in pos) and pos == sorted(pos) else 0.0
        if checks.get("only_ordered"):
            others = [(p, g) for p, g in lines if not g.get("order")]
            s["only"] = sum(covered(kept[p], g["start"], g["end"]) <= 0.1 for p, g in others) / max(1, len(others))
    if checks.get("broll_covers_cuts") or checks.get("broll_in_scope"):
        conn = [i for i in items if i["parent"] != -1 and i["lane"] > 0 and i["kind"] == "clip"]
        broll_dir = str(Path(clips[0]["path"]).parent.parent / "broll") + "/"
        if checks.get("broll_in_scope"):
            # B-roll was asked for: none at all is a miss; any shot from outside the scope is a fail.
            s["broll_scope"] = 1.0 if conn and all(i["file"].startswith(broll_dir) for i in conn) else 0.0
        if checks.get("broll_covers_cuts"):
            joins = [i["at"] for k, i in enumerate(primary) if k > 0]
            s["broll_cover"] = sum(any(c["at"] < j < c["at"] + c["dur"] for c in conn) for j in joins) / max(1, len(joins))
    s["score"] = sum(v for v in s.values()) / max(1, len(s))
    s["length_s"] = round(length, 2)
    return s


def score(lab: Path, tool: str, config: str, out: str | None) -> None:
    results = {}
    for d in sorted((lab / "cases").iterdir()):
        truth = json.loads((d / "truth.json").read_text())
        cid = truth["case"]["id"]
        w = lab / "runs" / config / cid
        if not w.exists():
            continue
        versions = sorted(w.glob("*(Claude*).fcpxml"), key=lambda p: p.stat().st_mtime)
        cost = turns = None
        if (w / "session.jsonl").exists():
            for line in open(w / "session.jsonl"):
                try:
                    m = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if m.get("type") == "result":
                    cost, turns = m.get("total_cost_usd"), m.get("num_turns")
        if not versions:
            results[cid] = {"score": 0.0, "note": "no version committed", "cost": cost, "turns": turns}
        else:
            r = score_case(truth, read_version(tool, versions[-1]))
            r.update({"cost": cost, "turns": turns})
            results[cid] = r
        print(cid, json.dumps({k: (round(v, 3) if isinstance(v, float) else v) for k, v in results[cid].items()}))
    mean = sum(r["score"] for r in results.values()) / max(1, len(results))
    print(f"\n{config}: mean {mean:.3f} over {len(results)} cases, "
          f"${sum((r.get('cost') or 0) for r in results.values()):.2f}")
    if out:
        Path(out).write_text(json.dumps({"config": config, "mean": mean, "cases": results}, indent=1))


# ---- reference ----------------------------------------------------------------------------

def reference_ops(truth: dict) -> list[dict] | None:
    case, clips, checks = truth["case"], truth["clips"], truth["case"]["checks"]
    if case.get("precut") or "target_length" in checks or checks.get("order") or case.get("broll"):
        return None  # no single cut follows from the truth alone
    ops, offset = [], 0.0
    remove = {"filler"} if checks.get("fillers_removed") else set()
    if checks.get("bad_removed"):
        remove.add("bad")
    limit = checks.get("max_pause")
    for c in clips:
        segs = c["segments"]
        speech = [g for g in segs if g["k"] != "pause"]
        # Spans of the file to cut: removed speech with the silence after it, then long silences.
        cuts = []
        for n, g in enumerate(speech):
            if g["k"] not in remove:
                continue
            prev_end = speech[n - 1]["end"] if n > 0 else 0.0
            nxt = next((h for h in speech[n + 1:] if h["k"] not in remove), None)
            a = g["start"] - 0.02 if n > 0 else 0.0
            b = nxt["start"] - 0.08 if nxt else c["duration"]
            # Keep the silence before the removed speech as it was; drop what it adds.
            cuts.append((max(a, prev_end + 0.08), b))
        if limit is not None:
            kept_speech = [g for g in speech if g["k"] not in remove]
            for x, y in zip(kept_speech, kept_speech[1:]):
                gap = y["start"] - x["end"]
                keep_this = any(p.get("keep") and x["end"] <= p["start"] + 0.01 and p["end"] <= y["start"] + 0.01
                                for p in segs if p["k"] == "pause")
                # The silence left after any removals between them.
                inside = [(a, b) for a, b in cuts if a >= x["end"] - 0.01 and b <= y["start"] + 0.01]
                left = gap - sum(b - a for a, b in inside)
                if keep_this or left <= limit + 0.05:
                    continue
                if inside:
                    # Widen the removal backwards into the silence before it.
                    a, b = inside[0]
                    cuts[cuts.index(inside[0])] = (max(x["end"] + 0.08, a - (left - limit)), b)
                else:
                    mid = x["end"] + gap / 2
                    cuts.append((mid - (gap - limit) / 2, mid + (gap - limit) / 2))
        merged: list[tuple[float, float]] = []
        for a, b in sorted(cuts):
            if merged and a <= merged[-1][1]:
                merged[-1] = (merged[-1][0], max(merged[-1][1], b))
            else:
                merged.append((a, b))
        for a, b in merged:
            if b - a < 0.06:
                continue  # less than a frame: nothing to cut
            ops.append({"op": "cut", "from": round(offset + a, 3), "to": round(offset + b, 3)})
        offset += float(F(round(c["duration"] * FPS), FPS))
    return ops


def reference(lab: Path, tool: str) -> None:
    for d in sorted((lab / "cases").iterdir()):
        truth = json.loads((d / "truth.json").read_text())
        ops = reference_ops(truth)
        if ops is None:
            continue
        w = lab / "runs" / "reference" / truth["case"]["id"]
        w.mkdir(parents=True, exist_ok=True)
        out = w / f"{truth['case']['id']} (Claude 1).fcpxml"
        r = subprocess.run([tool, "edit", str(d / "project.fcpxml"), "--ops", json.dumps(ops), "--out", str(out),
                            "--no-marks"], capture_output=True, text=True)
        print(truth["case"]["id"], "reference:", len(ops), "cuts", "" if r.returncode == 0 else r.stderr.strip())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("verb", choices=["build", "index", "run", "score", "reference"])
    ap.add_argument("--lab", type=Path, required=True)
    ap.add_argument("--bench")
    ap.add_argument("--addons")
    ap.add_argument("--app")
    ap.add_argument("--config", default="skills", choices=["skills", "plain", "reference", "noedit"])
    ap.add_argument("--skills", type=Path, default=HERE.parents[2] / "src" / "addons" / "agent" / "skills")
    ap.add_argument("--cases", default="")
    ap.add_argument("--model", default="opus")
    ap.add_argument("--timeline-tool")
    ap.add_argument("--json")
    a = ap.parse_args()
    if a.verb == "build":
        build(a.lab)
    elif a.verb == "index":
        index(a.lab, a.bench, a.addons)
    elif a.verb == "run":
        run(a.lab, a.app, a.addons, a.config, a.skills, [c for c in a.cases.split(",") if c], a.model)
    elif a.verb == "reference":
        reference(a.lab, a.timeline_tool)
    else:
        score(a.lab, a.timeline_tool, a.config, a.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
