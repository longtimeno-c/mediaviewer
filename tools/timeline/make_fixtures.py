#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the FCPXML round-trip set (docs/design/29 "Round trip") into
tests/data/fcpxml: 20 projects in the shape Final Cut Pro 10.6-12 exports them
(a dragged project is library > event > project), each built around what a
round trip must keep: connected clips and storylines, compound and multicam
clips, titles and captions, effects, colour and keyframes, roles, audio
components and split edits, retiming, NTSC timing, an older FCPXML, and
names that need escaping.

Times are exact fractions; offsets in a storyline follow one another as FCP
writes them. Every file is checked against Final Cut Pro's own DTD for its
version when FCP is installed (`--check`), so a fixture is a document FCP
would accept.

    python3 tools/timeline/make_fixtures.py [--out DIR] [--check]
"""
from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import urllib.parse
import sys
from fractions import Fraction as F

DTD_DIR = "/Applications/Final Cut Pro.app/Contents/Frameworks/Interchange.framework/Versions/A/Resources"
HOME = "/Users/editor"


def h(x, bits: int) -> int:
    """A stable hash (Python's hash() changes from run to run)."""
    return int.from_bytes(hashlib.sha256(repr(x).encode()).digest(), "big") % (1 << bits)


def t(x) -> str:
    x = F(x)
    if x.denominator == 1:
        return f"{x.numerator}s"
    return f"{x.numerator}/{x.denominator}s"


def esc(s: str) -> str:
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;"))


def url(path: str) -> str:
    keep = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/")
    out = []
    for b in path.encode("utf-8"):
        c = chr(b)
        out.append(c if c in keep else "%%%02X" % b)
    return "file://" + "".join(out)


class Doc:
    """A tiny FCPXML writer in FCP's own layout (4-space indents)."""

    def __init__(self, version="1.13"):
        self.version = version
        self.resources: list[str] = []
        self.next_id = 1
        self.formats: dict[str, str] = {}

    def rid(self) -> str:
        r = f"r{self.next_id}"
        self.next_id += 1
        return r

    def fmt(self, name, frame, w, h, cs="1-1-1 (Rec. 709)") -> str:
        if name in self.formats:
            return self.formats[name]
        r = self.rid()
        self.formats[name] = r
        fd = f' frameDuration="{t(frame)}"' if frame else ""
        self.resources.append(f'<format id="{r}" name="{name}"{fd} width="{w}" height="{h}" colorSpace="{cs}"/>')
        return r

    def asset(self, name, path, duration, fmt=None, start=0, video=True, audio=True, channels=2, uid=None,
              legacy=False, md=True) -> str:
        r = self.rid()
        uid = uid or ("%032X" % (h((name, path), 128)))
        attrs = f'id="{r}" name="{esc(name)}" uid="{uid}" start="{t(start)}" duration="{t(duration)}"'
        if video:
            attrs += ' hasVideo="1"'
            if fmt:
                attrs += f' format="{fmt}"'
            if not legacy:
                attrs += ' videoSources="1"'
        if audio:
            attrs += f' hasAudio="1" audioSources="1" audioChannels="{channels}" audioRate="48000"'
        if legacy:
            self.resources.append(f'<asset {attrs} src="{esc(url(path))}"/>')
            return r
        body = [f'    <media-rep kind="original-media" sig="{uid[:16]}" src="{esc(url(path))}">',
                '        <bookmark>Ym9vazgEAAAAAAQQMAAAAA==</bookmark>',
                '    </media-rep>']
        if md:
            body += ['    <metadata>',
                     '        <md key="com.apple.proapps.studio.rawToLogConversion" value="0"/>',
                     '        <md key="com.apple.proapps.spotlight.kMDItemProfileName" value="HD (Rec. 709)"/>',
                     '        <md key="com.apple.proapps.studio.cameraISO" value="400"/>',
                     '    </metadata>']
        self.resources.append(f'<asset {attrs}>\n' + "\n".join(body) + '\n</asset>')
        return r

    def effect(self, name, uid) -> str:
        r = self.rid()
        self.resources.append(f'<effect id="{r}" name="{esc(name)}" uid="{esc(uid)}"/>')
        return r

    def media(self, name, inner: str, uid=None) -> str:
        r = self.rid()
        uid = uid or ("M%031X" % (h(name, 124)))
        self.resources.append(f'<media id="{r}" name="{esc(name)}" uid="{uid}" modDate="2026-10-11 21:37:12 +0100">\n'
                              + indent(inner, 1) + '\n</media>')
        return r

    def render(self, body: str) -> str:
        res = "\n".join(indent(r, 2) for r in self.resources)
        return ('<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE fcpxml>\n\n'
                f'<fcpxml version="{self.version}">\n    <resources>\n{res}\n    </resources>\n'
                + indent(body, 1) + "\n</fcpxml>\n")


def indent(s: str, n: int) -> str:
    pad = "    " * n
    return "\n".join(pad + line if line else line for line in s.split("\n"))


def wrap_project(name, seq, library="Dolomites trip", event="18-09-2026", extra_event_items="") -> str:
    lib = url(f"{HOME}/Movies/{library}.fcpbundle/")
    return (f'<library location="{esc(lib)}">\n'
            f'    <event name="{esc(event)}" uid="E{h(event, 60):015X}">\n'
            + (indent(extra_event_items, 2) + "\n" if extra_event_items else "")
            + f'        <project name="{esc(name)}" uid="P{h(name, 60):015X}" '
              f'modDate="2026-10-11 21:37:12 +0100">\n'
            + indent(seq, 3) + '\n        </project>\n    </event>\n'
            '    <smart-collection name="Projects" match="all">\n'
            '        <match-clip rule="is" type="project"/>\n'
            '    </smart-collection>\n'
            '</library>')


def sequence(fmt, items: list[str], duration, tc_start=0, tc_format="NDF", audio="stereo") -> str:
    spine = "\n".join(indent(i, 2) for i in items)
    return (f'<sequence format="{fmt}" duration="{t(duration)}" tcStart="{t(tc_start)}" tcFormat="{tc_format}" '
            f'audioLayout="{audio}" audioRate="48k">\n    <spine>\n{spine}\n    </spine>\n</sequence>')


class Spine:
    """Places clips one after another, as FCP writes a storyline."""

    def __init__(self, origin=0):
        self.at = F(origin)
        self.items: list[str] = []

    def add(self, tag, duration, attrs="", body="", name=None, offset_attr=True):
        duration = F(duration)
        if "ref=" not in attrs and tag in ("asset-clip", "ref-clip", "mc-clip", "title", "audio", "video"):
            raise ValueError("ref missing")
        # FCP's attribute order: ref, offset, name, start, duration, the rest.
        head, rest = "", attrs
        if attrs.startswith(("ref=", "ref='")):
            q = attrs[4]
            end = attrs.index(q, 5) + 1
            head, rest = attrs[:end], attrs[end:].lstrip()
        parts = [p for p in (head, f'offset="{t(self.at)}"' if offset_attr else "",
                             f'name="{esc(name)}"' if name is not None else "") if p]
        dur = f'duration="{t(duration)}"' if "duration=" not in attrs else ""
        # Put duration after start when the rest names one, as FCP does.
        if dur and " format=" in " " + rest:
            i = (" " + rest).index(" format=")
            rest = (" " + rest)[:i].strip() + " " + dur + " " + (" " + rest)[i:].strip()
            dur = ""
        a = f"<{tag} " + " ".join(parts + [rest] + ([dur] if dur else []))
        a = a.replace("  ", " ").rstrip()
        if body:
            self.items.append(a + ">\n" + indent(body, 1) + f"\n</{tag}>")
        else:
            self.items.append(a + "/>")
        start = self.at
        self.at += duration
        return start

    def transition(self, duration, name="Cross Dissolve", body=""):
        # Centred on the edit point; the clips keep their offsets.
        duration = F(duration)
        x = f'<transition name="{name}" offset="{t(self.at - duration / 2)}" duration="{t(duration)}"'
        self.items.append(x + (">\n" + indent(body, 1) + "\n</transition>" if body else "/>"))


FPS25 = F(1, 25)
FPS2997 = F(1001, 30000)
FPS24 = F(1001, 24000)


def frames(n, fd=FPS25):
    return F(n) * fd


DISSOLVE = ('<filter-video ref="{fx}" name="Cross Dissolve">\n'
            '    <param name="Look" key="1" value="11 (Video)"/>\n'
            '    <param name="Amount" key="2" value="50"/>\n'
            '    <param name="Ease" key="50" value="2 (In &amp; Out)"/>\n'
            '    <param name="Ease Amount" key="51" value="0"/>\n'
            '</filter-video>\n'
            '<filter-audio ref="{afx}" name="Audio Crossfade"/>')


def interview_clips(d, fmt, names, base=f"{HOME}/Footage/Interview", length=F(300)):
    return [d.asset(n, f"{base}/{n}.mov", length, fmt) for n in names]


def p01(d):
    fmt = d.fmt("FFVideoFormat1080p25", FPS25, 1920, 1080)
    a = interview_clips(d, fmt, ["A001_C001", "A001_C002", "A001_C003"])
    s = Spine()
    s.add("asset-clip", frames(412), f'ref="{a[0]}" name="A001_C001" start="{t(frames(250))}" format="{fmt}" '
          f'tcFormat="NDF" audioRole="dialogue"', '<marker start="%s" duration="%s" value="good answer"/>'
          % (t(frames(300)), t(FPS25)))
    s.add("asset-clip", frames(1250), f'ref="{a[1]}" name="A001_C002" start="{t(frames(1000))}" format="{fmt}" '
          f'tcFormat="NDF" audioRole="dialogue"')
    s.add("asset-clip", frames(733), f'ref="{a[2]}" name="A001_C003" start="0s" format="{fmt}" tcFormat="NDF" '
          f'audioRole="dialogue"', '<keyword start="0s" duration="%s" value="Interview"/>' % t(frames(733)))
    return wrap_project("Interview simple", sequence(fmt, s.items, s.at))


def p02(d):
    fmt = d.fmt("FFVideoFormat1080p25", FPS25, 1920, 1080)
    a = interview_clips(d, fmt, ["B002_C010", "B002_C011"])
    s = Spine()
    s.add("gap", frames(50), 'name="Gap" start="3600s"',
          '<chapter-marker start="3600s" duration="%s" value="Intro" posterOffset="0s"/>' % t(FPS25), offset_attr=True)
    s.add("asset-clip", frames(900), f'ref="{a[0]}" name="B002_C010" start="{t(frames(75))}" format="{fmt}" '
          f'tcFormat="NDF" audioRole="dialogue"',
          '<marker start="%s" duration="%s" value="cut the um here" completed="0"/>\n'
          '<marker start="%s" duration="%s" value="checked" completed="1"/>\n'
          '<chapter-marker start="%s" duration="%s" value="Pricing" posterOffset="%s"/>'
          % (t(frames(200)), t(FPS25), t(frames(400)), t(FPS25), t(frames(600)), t(FPS25), t(frames(5))))
    s.add("gap", frames(25), 'name="Gap" start="3600s"')
    s.add("asset-clip", frames(640), f'ref="{a[1]}" name="B002_C011" start="{t(frames(10))}" format="{fmt}" '
          f'tcFormat="NDF" audioRole="dialogue" enabled="0"')
    return wrap_project("Interview with gaps & markers", sequence(fmt, s.items, s.at))


def p03(d):
    fmt = d.fmt("FFVideoFormat1080p25", FPS25, 1920, 1080)
    a = interview_clips(d, fmt, ["Talk_01"], length=F(600))[0]
    broll = [d.asset(n, f"{HOME}/Footage/broll/{n}.MP4", F(40), fmt) for n in ("GX010042", "GX010043", "GX010044")]
    music = d.asset("Dolomites Theme", f"{HOME}/Music/Dolomites Theme.m4a", F(240), video=False)
    s = Spine()
    anchored = (f'<asset-clip ref="{broll[0]}" lane="1" offset="{t(frames(500))}" name="GX010042" '
                f'start="{t(frames(100))}" duration="{t(frames(125))}" format="{fmt}" tcFormat="NDF" '
                f'videoRole="video.B-roll" audioRole="effects">\n'
                f'    <adjust-volume amount="-96dB"/>\n</asset-clip>\n'
                f'<asset-clip ref="{broll[1]}" lane="2" offset="{t(frames(900))}" name="GX010043" '
                f'start="{t(frames(50))}" duration="{t(frames(75))}" format="{fmt}" tcFormat="NDF" '
                f'videoRole="video.B-roll" audioRole="effects" enabled="0"/>\n'
                f'<asset-clip ref="{music}" lane="-1" offset="0s" name="Dolomites Theme" '
                f'duration="{t(frames(2500))}" audioRole="music">\n'
                f'    <adjust-volume amount="-18dB">\n'
                f'        <param name="amount">\n'
                f'            <fadeIn type="easeIn" duration="{t(frames(25))}"/>\n'
                f'            <fadeOut type="easeOut" duration="{t(frames(50))}"/>\n'
                f'        </param>\n'
                f'    </adjust-volume>\n</asset-clip>')
    s.add("asset-clip", frames(2500), f'ref="{a}" name="Talk_01" start="0s" format="{fmt}" tcFormat="NDF" '
          f'audioRole="dialogue"', anchored)
    s.add("asset-clip", frames(125), f'ref="{broll[2]}" name="GX010044" start="{t(frames(10))}" format="{fmt}" '
          f'tcFormat="NDF" videoRole="video.B-roll"')
    return wrap_project("B-roll over talk", sequence(fmt, s.items, s.at))


def p04(d):
    fmt = d.fmt("FFVideoFormat3840x2160p25", FPS25, 3840, 2160)
    fx = d.effect("Cross Dissolve", "FxPlug:4731E73A-8DAC-4113-9A30-AE85B1761265")
    afx = d.effect("Audio Crossfade", "FFAudioTransition")
    main = d.asset("Walk", f"{HOME}/Footage/Walk.mov", F(900), fmt)
    b = [d.asset(f"Peak {i}", f"{HOME}/Footage/broll/Peak {i}.mov", F(30), fmt) for i in range(1, 4)]
    inner = Spine(origin=0)
    inner.add("asset-clip", frames(100), f'ref="{b[0]}" name="Peak 1" start="0s" format="{fmt}" tcFormat="NDF"')
    inner.transition(frames(20), body=DISSOLVE.format(fx=fx, afx=afx))
    inner.add("asset-clip", frames(150), f'ref="{b[1]}" name="Peak 2" start="{t(frames(30))}" format="{fmt}" '
              f'tcFormat="NDF"')
    inner.add("asset-clip", frames(90), f'ref="{b[2]}" name="Peak 3" start="{t(frames(5))}" format="{fmt}" '
              f'tcFormat="NDF"')
    storyline = (f'<spine lane="1" offset="{t(frames(200))}" format="{fmt}">\n'
                 + "\n".join(indent(i, 1) for i in inner.items) + "\n</spine>")
    s = Spine()
    s.add("asset-clip", frames(1500), f'ref="{main}" name="Walk" start="0s" format="{fmt}" tcFormat="NDF" '
          f'audioRole="dialogue"', storyline)
    return wrap_project("Connected storyline", sequence(fmt, s.items, s.at))


def p05(d):
    fmt = d.fmt("FFVideoFormat1080p25", FPS25, 1920, 1080)
    a = [d.asset(n, f"{HOME}/Footage/Compound/{n}.mov", F(60), fmt) for n in ("C1", "C2", "C3")]
    inner = Spine()
    inner.add("asset-clip", frames(200), f'ref="{a[0]}" name="C1" start="0s" format="{fmt}" tcFormat="NDF"')
    inner.add("asset-clip", frames(300), f'ref="{a[1]}" name="C2" start="{t(frames(25))}" format="{fmt}" tcFormat="NDF"')
    comp = d.media("Opening compound", sequence(fmt, inner.items, inner.at, tc_start=3600))
    t1 = d.asset("Title bed", f"{HOME}/Footage/Compound/bed.wav", F(120), video=False)
    s = Spine()
    s.add("ref-clip", frames(400), f'ref="{comp}" name="Opening compound" start="3600s"',
          f'<asset-clip ref="{t1}" lane="-1" offset="{t(F(3600) + frames(50))}" name="Title bed" '
          f'duration="{t(frames(250))}" audioRole="music"/>\n'
          f'<marker start="{t(F(3600) + frames(100))}" duration="{t(FPS25)}" value="compound beat"/>')
    s.add("asset-clip", frames(500), f'ref="{a[2]}" name="C3" start="0s" format="{fmt}" tcFormat="NDF"')
    s.add("ref-clip", frames(100), f'ref="{comp}" name="Opening compound" start="{t(F(3600) + frames(250))}" '
          f'useAudioSubroles="1"', '<filter-video ref="%s" name="Gaussian Blur">\n'
          '    <param name="Amount" key="9999/gaussianBlur/amount" value="12"/>\n</filter-video>'
          % d.effect("Gaussian Blur", ".../Effects.localized/Blurs.localized/Gaussian.localized/Gaussian.moef"))
    return wrap_project("Compound clips", sequence(fmt, s.items, s.at))


def p06(d):
    fmt = d.fmt("FFVideoFormat1080p2997", FPS2997, 1920, 1080)
    cam_a = d.asset("CamA", f"{HOME}/Footage/Multicam/CamA.mov", F(1800), fmt, start=F(3600))
    cam_b = d.asset("CamB", f"{HOME}/Footage/Multicam/CamB.mov", F(1800), fmt, start=F(3600))
    mic = d.asset("Lav", f"{HOME}/Footage/Multicam/Lav.wav", F(1800), video=False, channels=1, start=F(3600))
    mc = ('<multicam format="%s" tcStart="3600s" tcFormat="DF">\n'
          '    <mc-angle name="Cam A" angleID="angleA">\n'
          '        <asset-clip ref="%s" offset="3600s" name="CamA" start="3600s" duration="1800s" format="%s" tcFormat="DF"/>\n'
          '    </mc-angle>\n'
          '    <mc-angle name="Cam B" angleID="angleB">\n'
          '        <gap name="Gap" offset="3600s" start="3600s" duration="%s"/>\n'
          '        <asset-clip ref="%s" offset="%s" name="CamB" start="3600s" duration="1700s" format="%s" tcFormat="DF"/>\n'
          '    </mc-angle>\n'
          '    <mc-angle name="Lav" angleID="angleL">\n'
          '        <asset-clip ref="%s" offset="3600s" name="Lav" start="3600s" duration="1800s" audioRole="dialogue"/>\n'
          '    </mc-angle>\n'
          '</multicam>') % (fmt, cam_a, fmt, t(frames(300, FPS2997)), cam_b, t(F(3600) + frames(300, FPS2997)), fmt,
                            mic)
    m = d.media("Panel discussion", mc)
    s = Spine(origin=3600)
    for i, (angle, n) in enumerate((("angleA", 600), ("angleB", 450), ("angleA", 900))):
        s.add("mc-clip", frames(n, FPS2997), f'ref="{m}" name="Panel discussion" '
              f'start="{t(F(3600) + frames(1000 * (i + 1), FPS2997))}"',
              f'<mc-source angleID="{angle}" srcEnable="video"/>\n<mc-source angleID="angleL" srcEnable="audio"/>')
    return wrap_project("Multicam panel", sequence(fmt, s.items, s.at - 3600, tc_start=3600, tc_format="DF"))


def p07(d):
    fmt = d.fmt("FFVideoFormat1080p25", FPS25, 1920, 1080)
    basic = d.effect("Basic Title", ".../Titles.localized/Bumper:Opener.localized/Basic Title.localized/Basic Title.moti")
    lower = d.effect("Basic Lower Third", ".../Titles.localized/Lower Thirds.localized/Basic Lower Third.localized/"
                     "Basic Lower Third.moti")
    a = interview_clips(d, fmt, ["Anna_01"])[0]
    s = Spine()
    s.add("title", frames(100), f'ref="{basic}" name="Dolomites - Basic Title" start="3600s"',
          '<param name="Position" key="9999/999166631/999166633/1/100/101" value="0 0"/>\n'
          '<param name="Alignment" key="9999/999166631/999166633/2/354/999169573/401" value="1 (Center)"/>\n'
          '<text>\n    <text-style ref="ts1">Dolomites 2026</text-style>\n</text>\n'
          '<text-style-def id="ts1">\n'
          '    <text-style font="Helvetica Neue" fontSize="96" fontFace="Bold" fontColor="1 1 1 1" alignment="center"/>\n'
          '</text-style-def>')
    lt = (f'<title ref="{lower}" lane="1" offset="{t(frames(50))}" name="Anna Weber - Basic Lower Third" '
          f'start="3600s" duration="{t(frames(150))}">\n'
          '    <text>\n        <text-style ref="ts2">Anna Weber</text-style>\n    </text>\n'
          '    <text>\n        <text-style ref="ts3">Mountain guide</text-style>\n    </text>\n'
          '    <text-style-def id="ts2">\n        <text-style font="Helvetica Neue" fontSize="60" fontColor="1 1 1 1"/>\n'
          '    </text-style-def>\n    <text-style-def id="ts3">\n'
          '        <text-style font="Helvetica Neue" fontSize="40" fontColor="0.8 0.8 0.8 1"/>\n    </text-style-def>\n'
          '</title>')
    s.add("asset-clip", frames(800), f'ref="{a}" name="Anna_01" start="0s" format="{fmt}" tcFormat="NDF" '
          f'audioRole="dialogue"', lt)
    return wrap_project("Titles", sequence(fmt, s.items, s.at))


def p08(d):
    fmt = d.fmt("FFVideoFormat3840x2160p25", FPS25, 3840, 2160, cs="9-18-9 (Rec. 2020 HLG)")
    a = d.asset("Sunrise", f"{HOME}/Footage/Sunrise.mov", F(120), fmt)
    b = d.asset("Lake", f"{HOME}/Footage/Lake.mov", F(120), fmt)
    wheels = d.effect("Color Wheels", "FFHDRColorWheels")
    curves = d.effect("Color Curves", "FFHDRColorCurves")
    lut = d.effect("Custom LUT", "FFCustomLUTEffect")
    grade = ('<adjust-crop mode="trim">\n    <trim-rect left="2.5" right="2.5" top="1.2" bottom="1.2"/>\n'
             '</adjust-crop>\n'
             '<adjust-transform position="12.5 -3.75" scale="1.08 1.08" rotation="1.5">\n'
             '    <param name="position">\n        <keyframeAnimation>\n'
             '            <keyframe time="0s" value="0 0"/>\n'
             '            <keyframe time="%s" value="12.5 -3.75" curve="smooth"/>\n'
             '        </keyframeAnimation>\n    </param>\n</adjust-transform>\n'
             '<filter-video ref="%s" name="Color Wheels">\n'
             '    <data key="effectConfig">YnBsaXN0MDDUAQIDBAUGBwpYJHZlcnNpb24=</data>\n'
             '    <param name="Master" key="1" value="0 0 0.02"/>\n'
             '    <param name="Shadows" key="2" value="-0.01 0.004 0"/>\n'
             '    <param name="Mix" key="100" value="0.85"/>\n'
             '</filter-video>\n'
             '<filter-video ref="%s" name="Color Curves">\n'
             '    <data key="effectConfig">YnBsaXN0MDDUAQIDBAUGBwpYJHZlcnNpb24=</data>\n'
             '</filter-video>\n'
             '<filter-video ref="%s" name="Custom LUT">\n'
             '    <param name="LUT" key="1" value="Dolomites.cube"/>\n'
             '    <param name="Mix" key="2" value="1"/>\n'
             '</filter-video>') % (t(frames(75)), wheels, curves, lut)
    s = Spine()
    s.add("asset-clip", frames(300), f'ref="{a}" name="Sunrise" start="{t(frames(60))}" format="{fmt}" tcFormat="NDF"',
          grade)
    s.add("asset-clip", frames(250), f'ref="{b}" name="Lake" start="0s" format="{fmt}" tcFormat="NDF"',
          '<adjust-blend amount="0.9" mode="13 (Overlay)"/>\n<adjust-stabilization type="automatic"/>\n'
          '<audio-channel-source srcCh="1, 2">\n    <adjust-loudness amount="0.4" uniformity="0.2"/>\n'
          '    <adjust-noiseReduction amount="0.5"/>\n</audio-channel-source>')
    return wrap_project("Colour and effects", sequence(fmt, s.items, s.at), library="Holden CCTV", event="Grades")


def p09(d):
    fmt = d.fmt("FFVideoFormat1080p25", FPS25, 1920, 1080)
    fx = d.effect("Cross Dissolve", "FxPlug:4731E73A-8DAC-4113-9A30-AE85B1761265")
    afx = d.effect("Audio Crossfade", "FFAudioTransition")
    a = [d.asset(f"Shot {i}", f"{HOME}/Footage/Shot {i}.mov", F(60), fmt) for i in range(1, 5)]
    s = Spine()
    for i, n in enumerate((200, 175, 250, 300)):
        if i:
            s.transition(frames(25 if i != 2 else 12), body=DISSOLVE.format(fx=fx, afx=afx))
        s.add("asset-clip", frames(n), f'ref="{a[i]}" name="Shot {i + 1}" start="{t(frames(50))}" format="{fmt}" '
              f'tcFormat="NDF"')
    return wrap_project("Transitions", sequence(fmt, s.items, s.at))


def p10(d):
    fmt = d.fmt("FFVideoFormat1080p25", FPS25, 1920, 1080)
    a = interview_clips(d, fmt, ["Q1", "Q2", "Q3"])
    s = Spine()
    s.add("asset-clip", frames(500), f'ref="{a[0]}" name="Q1" start="{t(frames(100))}" format="{fmt}" '
          f'tcFormat="NDF" audioRole="dialogue"',
          '<audio-channel-source srcCh="1, 2" role="dialogue.dialogue-1">\n'
          '    <adjust-EQ mode="voice_enhance"/>\n</audio-channel-source>')
    # An L cut: the audio runs on under the next clip.
    s.add("asset-clip", frames(400), f'ref="{a[1]}" name="Q2" start="{t(frames(40))}" format="{fmt}" '
          f'tcFormat="NDF" audioRole="dialogue" audioStart="{t(frames(40))}" audioDuration="{t(frames(450))}"',
          '<audio-channel-source srcCh="1" role="dialogue.dialogue-1"/>\n'
          '<audio-channel-source srcCh="2" role="dialogue.dialogue-2" enabled="0"/>')
    # A J cut: its audio starts under the previous clip.
    s.add("asset-clip", frames(600), f'ref="{a[2]}" name="Q3" start="{t(frames(120))}" format="{fmt}" '
          f'tcFormat="NDF" audioRole="dialogue" audioStart="{t(frames(70))}" audioDuration="{t(frames(650))}"',
          '<audio-channel-source srcCh="1, 2" role="dialogue.dialogue-1" outCh="L, R">\n'
          '    <mute start="%s" duration="%s"/>\n</audio-channel-source>' % (t(frames(200)), t(frames(25))))
    return wrap_project("Audio components and split edits", sequence(fmt, s.items, s.at))


def p11(d):
    fmt = d.fmt("FFVideoFormat1080p25", FPS25, 1920, 1080)
    v = d.asset("Boat", f"{HOME}/Footage/Boat.mov", F(120), fmt)
    vo = d.asset("VO take 3", f"{HOME}/Audio/VO take 3.wav", F(60), video=False, channels=1)
    sfx = d.asset("Waves", f"{HOME}/Audio/Waves.wav", F(60), video=False)
    s = Spine()
    s.add("asset-clip", frames(1000), f'ref="{v}" name="Boat" start="0s" format="{fmt}" tcFormat="NDF" '
          f'videoRole="video.Hero" audioRole="effects.Natural sound"',
          f'<audio ref="{vo}" lane="-1" offset="{t(frames(100))}" name="VO take 3" start="0s" '
          f'duration="{t(frames(600))}" role="dialogue.VO" srcCh="1" outCh="C"/>\n'
          f'<audio ref="{sfx}" lane="-2" offset="0s" name="Waves" duration="{t(frames(1000))}" role="effects.Ambience">\n'
          f'    <adjust-volume amount="-12dB"/>\n</audio>\n'
          f'<video ref="{v}" lane="1" offset="{t(frames(500))}" name="Boat (video only)" start="{t(frames(40))}" '
          f'duration="{t(frames(100))}" role="video.Inserts"/>')
    return wrap_project("Roles", sequence(fmt, s.items, s.at))


def p12(d):
    fmt = d.fmt("FFVideoFormat1080p25", FPS25, 1920, 1080)
    slow = d.asset("Jump 120fps", f"{HOME}/Footage/Jump 120fps.mov", F(20),
                   d.fmt("FFVideoFormat1080p120", F(1, 120), 1920, 1080))
    a = d.asset("Ridge", f"{HOME}/Footage/Ridge.mov", F(60), fmt)
    s = Spine()
    s.add("asset-clip", frames(125), f'ref="{a}" name="Ridge" start="0s" format="{fmt}" tcFormat="NDF"')
    s.add("asset-clip", frames(250), f'ref="{slow}" name="Jump 120fps" start="0s" tcFormat="NDF"',
          '<conform-rate scaleEnabled="0" srcFrameRate="119.88"/>\n'
          '<timeMap>\n    <timept time="0s" value="0s" interp="smooth2"/>\n'
          '    <timept time="%s" value="%s" interp="smooth2"/>\n'
          '    <timept time="%s" value="%s" interp="smooth2"/>\n</timeMap>'
          % (t(frames(125)), t(frames(25)), t(frames(250)), t(frames(150))))
    s.add("asset-clip", frames(100), f'ref="{a}" name="Ridge" start="{t(frames(200))}" format="{fmt}" tcFormat="NDF"',
          '<conform-rate srcFrameRate="23.98"/>')
    return wrap_project("Retiming", sequence(fmt, s.items, s.at))


def p13(d):
    fmt = d.fmt("FFVideoFormat1080p24", FPS24, 1920, 1080)
    cam = d.asset("Scene 4 cam", f"{HOME}/Footage/Scene 4 cam.mov", F(300), fmt)
    rec = d.asset("Scene 4 zoom", f"{HOME}/Footage/Scene 4 zoom.wav", F(300), video=False, channels=2)
    s = Spine()
    sync_body = ('<spine>\n'
                 f'    <asset-clip ref="{cam}" offset="0s" name="Scene 4 cam" duration="{t(frames(2000, FPS24))}" '
                 f'format="{fmt}" tcFormat="NDF" audioRole="dialogue"/>\n</spine>\n'
                 f'<asset-clip ref="{rec}" lane="-1" offset="{t(frames(12, FPS24))}" name="Scene 4 zoom" '
                 f'duration="{t(frames(1990, FPS24))}" audioRole="dialogue"/>\n'
                 '<sync-source sourceID="storyline">\n'
                 '    <audio-role-source role="dialogue.dialogue-1" active="0"/>\n</sync-source>\n'
                 '<sync-source sourceID="connected">\n'
                 '    <audio-role-source role="dialogue.dialogue-1"/>\n</sync-source>')
    s.add("sync-clip", frames(900, FPS24), f'name="Scene 4 - Synchronized" start="{t(frames(240, FPS24))}" '
          f'format="{fmt}" tcFormat="NDF"', sync_body)
    return wrap_project("Synchronised clip", sequence(fmt, s.items, s.at))


def p14(d):
    fmt = d.fmt("FFVideoFormat1080p25", FPS25, 1920, 1080)
    t1 = d.asset("Take 1", f"{HOME}/Footage/Take 1.mov", F(30), fmt)
    t2 = d.asset("Take 2", f"{HOME}/Footage/Take 2.mov", F(30), fmt)
    other = d.asset("Wide", f"{HOME}/Footage/Wide.mov", F(30), fmt)
    s = Spine()
    s.add("asset-clip", frames(200), f'ref="{other}" name="Wide" start="0s" format="{fmt}" tcFormat="NDF"')
    s.items.append(f'<audition offset="{t(s.at)}">\n'
                   f'    <asset-clip ref="{t2}" name="Take 2" start="{t(frames(10))}" duration="{t(frames(300))}" '
                   f'format="{fmt}" tcFormat="NDF"/>\n'
                   f'    <asset-clip ref="{t1}" name="Take 1" start="{t(frames(30))}" duration="{t(frames(280))}" '
                   f'format="{fmt}" tcFormat="NDF"/>\n</audition>')
    s.at += frames(300)
    s.add("asset-clip", frames(150), f'ref="{other}" name="Wide" start="{t(frames(300))}" format="{fmt}" tcFormat="NDF"')
    return wrap_project("Audition", sequence(fmt, s.items, s.at))


def p15(d):
    fmt = d.fmt("FFVideoFormat1080p25", FPS25, 1920, 1080)
    a = interview_clips(d, fmt, ["Caption talk"])[0]
    caps = []
    for i, (st, line) in enumerate(((50, "Welcome to the Dolomites."), (125, "We start at Passo Giau."),
                                    (220, "It’s 2,236 m up."))):
        caps.append(f'<caption lane="1" offset="{t(frames(st))}" name="{esc(line)}" start="3600s" '
                    f'duration="{t(frames(70))}" role="SRT?captionFormat=SRT.en">\n'
                    f'    <text placement="bottom">\n        <text-style ref="cs{i}">{esc(line)}</text-style>\n'
                    f'    </text>\n    <text-style-def id="cs{i}">\n'
                    f'        <text-style font=".AppleSystemUIFont" fontSize="13" fontFace="Regular" '
                    f'fontColor="1 1 1 1" backgroundColor="0 0 0 1"/>\n    </text-style-def>\n</caption>')
    s = Spine()
    s.add("asset-clip", frames(400), f'ref="{a}" name="Caption talk" start="0s" format="{fmt}" tcFormat="NDF" '
          f'audioRole="dialogue"', "\n".join(caps))
    return wrap_project("Captions", sequence(fmt, s.items, s.at))


def p16(d):
    fmt = d.fmt("FFVideoFormat1080p2997", FPS2997, 1920, 1080)
    a = [d.asset(f"NTSC {i}", f"{HOME}/Footage/NTSC {i}.mov", F(600), fmt, start=F(3600)) for i in range(1, 4)]
    s = Spine(origin=3600)
    for i, n in enumerate((901, 1799, 457)):
        s.add("asset-clip", frames(n, FPS2997), f'ref="{a[i]}" name="NTSC {i + 1}" '
              f'start="{t(F(3600) + frames(30 * (i + 1), FPS2997))}" format="{fmt}" tcFormat="DF"',
              f'<marker start="{t(F(3600) + frames(30 * (i + 1) + 15, FPS2997))}" duration="1001/30000s" '
              f'value="beat {i + 1}"/>')
    return wrap_project("NTSC drop frame", sequence(fmt, s.items, s.at - 3600, tc_start=3600, tc_format="DF"))


def p17(d):
    fmt = d.fmt("FFVideoFormat1080p25", FPS25, 1920, 1080)
    a = interview_clips(d, fmt, ["Event A", "Event B"])
    clips = (f'<asset-clip ref="{a[0]}" name="Event A" duration="300s" format="{fmt}" tcFormat="NDF" '
             f'audioRole="dialogue">\n    <keyword start="0s" duration="300s" value="selects"/>\n</asset-clip>\n'
             f'<asset-clip ref="{a[1]}" name="Event B" duration="300s" format="{fmt}" tcFormat="NDF" '
             f'audioRole="dialogue"/>\n'
             '<keyword-collection name="selects"/>\n'
             '<collection-folder name="Folder">\n    <keyword-collection name="maybe"/>\n</collection-folder>\n'
             '<smart-collection name="Favourites" match="all">\n    <match-ratings value="favorites"/>\n'
             '</smart-collection>\n')
    other = Spine()
    other.add("asset-clip", frames(100), f'ref="{a[1]}" name="Event B" start="0s" format="{fmt}" tcFormat="NDF"')
    first_project = (f'<project name="Rough cut v1" uid="PROUGH00000001" modDate="2026-10-01 10:00:00 +0100">\n'
                     + indent(sequence(fmt, other.items, other.at), 1) + '\n</project>')
    s = Spine()
    s.add("asset-clip", frames(250), f'ref="{a[0]}" name="Event A" start="{t(frames(25))}" format="{fmt}" '
          f'tcFormat="NDF" audioRole="dialogue"')
    s.add("asset-clip", frames(300), f'ref="{a[1]}" name="Event B" start="{t(frames(500))}" format="{fmt}" '
          f'tcFormat="NDF" audioRole="dialogue"')
    return wrap_project("Rough cut v2", sequence(fmt, s.items, s.at), extra_event_items=clips + first_project)


def p18(d):
    d.version = "1.8"
    fmt = d.fmt("FFVideoFormat720p25", FPS25, 1280, 720)
    a = d.asset("Old export", f"{HOME}/Footage/Old export.mov", F(90), fmt, legacy=True)
    b = d.asset("Old export 2", f"{HOME}/Footage/Old export 2.mov", F(90), fmt, legacy=True)
    s = Spine()
    s.add("clip", frames(300), f'name="Old export" start="0s" format="{fmt}" tcFormat="NDF"',
          f'<video ref="{a}" offset="0s" duration="{t(frames(300))}"/>\n'
          f'<audio ref="{a}" lane="-1" offset="0s" duration="{t(frames(300))}" role="dialogue"/>')
    s.add("asset-clip", frames(200), f'ref="{b}" name="Old export 2" start="{t(frames(50))}" format="{fmt}" '
          f'tcFormat="NDF"')
    return wrap_project("Legacy 1.8", sequence(fmt, s.items, s.at))


def p19(d):
    fmt = d.fmt("FFVideoFormat1080p25", FPS25, 1920, 1080)
    weird = d.asset("Café \"Zum Hirschen\" & <Bar>", f"{HOME}/Footage/Ü & ö/Café #1 (100%).mov", F(60), fmt)
    emoji = d.asset("🏔️ Gipfel", f"{HOME}/Footage/🏔️ Gipfel.mov", F(60), fmt)
    s = Spine()
    s.add("asset-clip", frames(200), f'ref="{weird}" name="Café &quot;Zum Hirschen&quot; &amp; &lt;Bar&gt;" '
          f'start="0s" format="{fmt}" tcFormat="NDF"',
          "<!-- a comment FCP never writes, kept anyway -->\n"
          "<note><![CDATA[Notes with <angle brackets> & ampersands]]></note>")
    s.add("asset-clip", frames(150), f"ref='{emoji}' name='🏔️ Gipfel' start='0s' format='{fmt}' tcFormat='NDF'",
          '<marker start="0s" duration="1/25s" value="Gipfel &#x1F3D4; &#38; Tal"/>')
    return wrap_project("Unicode & entities — \"quoted\"", sequence(fmt, s.items, s.at),
                        library="Dolomites 26", event="Tag 1 – Ankunft")


def p20(d):
    d.version = "1.14"
    fmt = d.fmt("FFVideoFormat1080p25", FPS25, 1920, 1080)
    a = interview_clips(d, fmt, ["Long talk"], length=F(1800))[0]
    broll = [d.asset(f"Drone {i:02d}", f"{HOME}/Footage/broll/Drone {i:02d}.mov", F(45), fmt) for i in range(1, 7)]
    basic = d.effect("Basic Title", ".../Titles.localized/Bumper:Opener.localized/Basic Title.localized/Basic Title.moti")
    s = Spine()
    for k in range(6):
        anchored = (f'<asset-clip ref="{broll[k]}" lane="1" offset="{t(frames(1200 * k + 200))}" '
                    f'name="Drone {k + 1:02d}" start="{t(frames(25))}" duration="{t(frames(150))}" format="{fmt}" '
                    f'tcFormat="NDF" videoRole="video.B-roll"/>')
        if k == 2:
            anchored += (f'\n<title ref="{basic}" lane="2" offset="{t(frames(1200 * k + 300))}" name="Section 3" '
                         f'start="3600s" duration="{t(frames(100))}">\n'
                         '    <text>\n        <text-style ref="ts9">Part three</text-style>\n    </text>\n'
                         '    <text-style-def id="ts9">\n        <text-style font="Helvetica" fontSize="72"/>\n'
                         '    </text-style-def>\n</title>')
        s.add("asset-clip", frames(1000), f'ref="{a}" name="Long talk" start="{t(frames(1200 * k))}" format="{fmt}" '
              f'tcFormat="NDF" audioRole="dialogue"', anchored + "\n" +
              f'<marker start="{t(frames(1200 * k + 500))}" duration="1/25s" value="section {k + 1}"/>')
    return wrap_project("Long interview with B-roll", sequence(fmt, s.items, s.at))


PROJECTS = [p01, p02, p03, p04, p05, p06, p07, p08, p09, p10, p11, p12, p13, p14, p15, p16, p17, p18, p19, p20]
NAMES = ["01-interview-simple", "02-gaps-markers", "03-connected-broll", "04-connected-storyline", "05-compound",
         "06-multicam", "07-titles", "08-colour-effects", "09-transitions", "10-audio-components", "11-roles",
         "12-retiming", "13-sync-clip", "14-audition", "15-captions", "16-ntsc-drop-frame", "17-event-two-projects",
         "18-legacy-1-8", "19-unicode-entities", "20-long-interview-broll"]


def check(path: str, version: str) -> bool:
    dtd = os.path.join(DTD_DIR, "FCPXMLv%s.dtd" % version.replace(".", "_"))
    if not os.path.exists(dtd):
        print(f"  (no DTD for {version}: Final Cut Pro not installed?)")
        return True
    # xmllint reads the DTD argument as a URL: escape the spaces in FCP's path.
    r = subprocess.run(["xmllint", "--noout", "--dtdvalid", "file://" + urllib.parse.quote(dtd), path],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr)
    return r.returncode == 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--out", default=os.path.join(here, "..", "..", "tests", "data", "fcpxml"))
    ap.add_argument("--check", action="store_true", help="validate each file against FCP's DTD")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    ok = True
    for fn, name in zip(PROJECTS, NAMES):
        d = Doc()
        body = fn(d)
        text = d.render(body)
        path = os.path.join(args.out, name + ".fcpxml")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        if args.check:
            good = check(path, d.version)
            ok = ok and good
            print(("ok   " if good else "FAIL ") + name + f" (FCPXML {d.version})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
