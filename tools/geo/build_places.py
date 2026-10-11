#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the offline place database (docs/design/06 "Place", src/geo/blob.h).

    python3 -I tools/geo/build_places.py --downloads DIR --out places.mvgeo
    python3 -I tools/geo/build_places.py --downloads DIR --out t.mvgeo --subset test

Inputs are pinned downloads (URL + SHA-256 below), fetched once into
--downloads (never the source tree) and reused while their hash matches:

  * Natural Earth 1:10m admin-0 countries and admin-1 states/provinces
    (public domain), GeoJSON from the nvkelso/natural-earth-vector release tag.
  * GeoNames cities1000, countryInfo.txt, admin1CodesASCII.txt (CC-BY 4.0).

GeoNames republishes its dump daily under the same URL, so its pins go stale.
A mismatch fails unless --accept-upstream is given, which takes the new file
and prints the hash to pin. The blob records every input's hash in its META
section, so a build can always be traced to its data.

Output: one little-endian blob, versioned and checksummed, laid out so the C++
loader can memory-map it and use it in place (src/geo/blob.h is the format's
other half; keep them in step and bump VERSION when either changes).

Standard library only: packaging CI needs no pip install.
"""
import argparse
import hashlib
import io
import json
import math
import os
import struct
import sys
import time
import urllib.request
import zipfile

VERSION = 1
MAGIC = b"MVGEO\x1a\x00\x00"
Q = 1_000_000  # coordinates are int32 microdegrees
KM_PER_DEG = 111.32

# Simplification tolerance. Coastlines dominate the point count, so they take
# the coarser value; land borders are shared arcs, simplified once and used by
# both neighbours, so they stay exact against each other and can afford a
# finer tolerance (a point 300 m inside a border still lands on the right side).
T_COAST_KM = 1.0
T_BORDER_KM = 0.25

NE_TAG = "v5.1.2"
NE_COMMIT = "f1890d9f152c896d250a77557a5751a93d494776"
NE_BASE = f"https://raw.githubusercontent.com/nvkelso/natural-earth-vector/{NE_COMMIT}/geojson/"
GN_BASE = "https://download.geonames.org/export/dump/"

SOURCES = {
    "admin0": {
        "file": "ne_10m_admin_0_countries.geojson",
        "url": NE_BASE + "ne_10m_admin_0_countries.geojson",
        "sha256": "239eec57ac17f100a11e2536cffc56752c318b50ae765b0918ff7aab4ce8f255",
        "licence": "Natural Earth, public domain",
    },
    "admin1": {
        "file": "ne_10m_admin_1_states_provinces.geojson",
        "url": NE_BASE + "ne_10m_admin_1_states_provinces.geojson",
        "sha256": "22d0e3ad85eb3e27f17cabf8ba2d50e554fbc27a87796ff891d958185da62fb5",
        "licence": "Natural Earth, public domain",
    },
    # GeoNames snapshot of 2026-10-11.
    "cities": {
        "file": "cities1000.zip",
        "url": GN_BASE + "cities1000.zip",
        "sha256": "23c086c3d835dcaf5a9d9f6a56de98f425200af1b9b88b65b353d88359e35ccf",
        "licence": "GeoNames, CC-BY 4.0",
    },
    "countries": {
        "file": "countryInfo.txt",
        "url": GN_BASE + "countryInfo.txt",
        "sha256": "c5997ea659df5bcf42509eb937064d172b2a998bb873608603e8421d714a9974",
        "licence": "GeoNames, CC-BY 4.0",
    },
    "admin1codes": {
        "file": "admin1CodesASCII.txt",
        "url": GN_BASE + "admin1CodesASCII.txt",
        "sha256": "1da92a6323a5fec3176f3f743bf4cf4040fd56a876da55e46fbca23c863aa60a",
        "licence": "GeoNames, CC-BY 4.0",
    },
}

# Natural Earth features with no ISO code ("-99"): the country GeoNames files
# their towns under. Features not listed keep their own name and no code.
NE_CODE_OVERRIDES = {
    "SOL": "SO",  # Somaliland
    "CYN": "CY",  # Northern Cyprus
    "CNM": "CY",  # Cyprus UN buffer zone
    "ESB": "CY",  # Dhekelia (GeoNames files it under CY)
    "WSB": "CY",  # Akrotiri
    "USG": "CU",  # Guantanamo Bay
}

# GeoNames feature codes that are not a town a person would name: sections of
# a city (PPLX, and PPLA5 such as the Paris arrondissements), historical, abandoned, destroyed, religious sites.
SKIP_TOWN_CODES = {"PPLX", "PPLA5", "PPLH", "PPLQ", "PPLW", "PPLCH"}

# --subset test: the CI test blob (tests/data/geo). Whole Natural Earth
# features that touch these boxes, and the towns inside them.
SUBSETS = {
    "test": [
        (-9.60, 38.55, -8.90, 39.05),    # Lisbon, Sintra, Cascais
        (-8.75, 41.05, -7.40, 41.25),    # Porto and the Douro
        (-7.40, 38.70, -6.85, 39.00),    # Elvas / Badajoz border
        (-17.35, 32.55, -16.25, 33.15),  # Madeira and Porto Santo
        (-31.35, 36.85, -24.95, 39.80),  # Azores
        (1.85, 42.40, 2.10, 42.55),      # Llivia (Spain inside France)
        (4.88, 51.39, 4.98, 51.47),      # Baarle-Hertog / Baarle-Nassau
        (177.00, -18.30, 180.00, -16.00),  # Fiji, west of the antimeridian
        (-180.00, -17.20, -179.70, -16.00),  # Fiji, east of it (Taveuni side)
        (172.85, 1.30, 173.20, 1.50),    # Tarawa, Kiribati
    ],
}

SEC_STRINGS, SEC_COUNTRIES, SEC_REGIONS, SEC_TOWNS, SEC_TOWN_ROWS, SEC_TOWN_CELLS, SEC_META = 1, 2, 3, 4, 5, 6, 7
LAYER_BASE = (16, 32)  # + POLYS, RINGS, POINTS, BANDS, EDGES, ROWS, CELLS, CANDS
L_POLYS, L_RINGS, L_POINTS, L_BANDS, L_EDGES, L_ROWS, L_CELLS, L_CANDS = range(8)
FULL = 0x80000000
NO_REGION = 0xFFFF
REGION_COUNTRY = 1  # region flag: its country overrides the admin-0 polygon (Svalbard, Réunion)


def log(*a):
    print("build_places:", *a, file=sys.stderr, flush=True)


# ---- downloads ------------------------------------------------------------------------

def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(dl_dir, accept_upstream):
    os.makedirs(dl_dir, exist_ok=True)
    hashes = {}
    for key, s in SOURCES.items():
        path = os.path.join(dl_dir, s["file"])
        if not (os.path.exists(path) and sha256_of(path) == s["sha256"]):
            log("fetch", s["url"])
            part = path + ".part"
            req = urllib.request.Request(s["url"], headers={"User-Agent": "mediaviewer-build"})
            with urllib.request.urlopen(req, timeout=120) as r, open(part, "wb") as f:
                while True:
                    b = r.read(1 << 20)
                    if not b:
                        break
                    f.write(b)
            got = sha256_of(part)
            if got != s["sha256"]:
                # Only GeoNames moves under a fixed URL. Natural Earth is pinned
                # to a commit, so a different file there is damage or tampering,
                # and --accept-upstream never takes it.
                if not s["url"].startswith(GN_BASE):
                    os.remove(part)
                    sys.exit(f"build_places: {s['file']} does not match its pin (sha256 {got}, pinned "
                             f"{s['sha256']}). It is fetched from a fixed commit, so this is not an upstream "
                             f"update; --accept-upstream does not apply.")
                if not accept_upstream:
                    os.remove(part)
                    sys.exit(f"build_places: {s['file']} changed upstream (sha256 {got}, pinned "
                             f"{s['sha256']}). Re-run with --accept-upstream to take it, then pin the new hash.")
                log(f"accepting new upstream {s['file']}: sha256 {got} (pin this)")
            os.replace(part, path)
        hashes[key] = sha256_of(path)
    return hashes


# ---- inputs -----------------------------------------------------------------------------

def read_country_info(path):
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            c = line.rstrip("\n").split("\t")
            langs = [x.split("-")[0] for x in c[15].split(",") if x] if len(c) > 15 else []
            out[c[0]] = {"name": c[4], "langs": langs}
    return out


def read_admin1(path):
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            c = line.rstrip("\n").split("\t")
            if len(c) >= 3:
                out[c[0]] = {"name": c[1], "ascii": c[2]}
    return out


def read_cities(path):
    towns = []
    with zipfile.ZipFile(path) as z:
        with z.open("cities1000.txt") as raw:
            for line in io.TextIOWrapper(raw, encoding="utf-8"):
                c = line.rstrip("\n").split("\t")
                if len(c) < 15 or c[6] != "P" or c[7] in SKIP_TOWN_CODES:
                    continue
                towns.append({
                    "name": c[1], "lat": float(c[4]), "lon": float(c[5]), "cc": c[8],
                    "a1": c[10], "pop": int(c[14] or 0),
                })
    return towns


def qx(v):
    return int(round(v * Q))


def feature_parts(feat):
    g = feat.get("geometry")
    if not g:
        return []
    polys = g["coordinates"] if g["type"] == "MultiPolygon" else [g["coordinates"]]
    parts = []
    for P in polys:
        rings = []
        for r in P:
            pts = []
            for x, y in r:
                p = (max(-180 * Q, min(180 * Q, qx(x))), max(-90 * Q, min(90 * Q, qx(y))))
                if not pts or pts[-1] != p:
                    pts.append(p)
            if len(pts) > 1 and pts[0] == pts[-1]:
                pts.pop()
            if len(pts) >= 3:
                rings.append(pts)
        if rings:
            parts.append(rings)
    return parts


# ---- topology-preserving simplification -------------------------------------------------

def dp(pts, tol_km):
    """Douglas-Peucker on an open polyline, endpoints kept. Equirectangular at the
    polyline's mean latitude, which is exact enough at 1 km over an arc."""
    n = len(pts)
    if n <= 2:
        return list(pts)
    lat0 = sum(p[1] for p in pts) / n / Q
    kx = math.cos(math.radians(lat0))
    tol = (tol_km / KM_PER_DEG) * Q
    tol2 = tol * tol
    xs = [p[0] * kx for p in pts]
    ys = [p[1] for p in pts]
    keep = [False] * n
    keep[0] = keep[-1] = True
    stack = [(0, n - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay, bx, by = xs[a], ys[a], xs[b], ys[b]
        dx, dy = bx - ax, by - ay
        L2 = dx * dx + dy * dy
        best, bi = -1.0, -1
        for i in range(a + 1, b):
            px, py = xs[i] - ax, ys[i] - ay
            if L2 == 0:
                d2 = px * px + py * py
            else:
                t = (px * dx + py * dy) / L2
                t = 0.0 if t < 0 else (1.0 if t > 1 else t)
                ex, ey = px - t * dx, py - t * dy
                d2 = ex * ex + ey * ey
            if d2 > best:
                best, bi = d2, i
        if best > tol2:
            keep[bi] = True
            stack.append((a, bi))
            stack.append((bi, b))
    return [p for p, k in zip(pts, keep) if k]


def simplify_layer(features):
    """features: list of parts lists. Returns the same shape, simplified so that a
    border shared by two features is simplified once and used by both."""
    owner = {}
    for fi, parts in enumerate(features):
        for rings in parts:
            for ring in rings:
                for p in ring:
                    o = owner.get(p)
                    if o is None:
                        owner[p] = fi
                    elif isinstance(o, int):
                        if o != fi:
                            owner[p] = frozenset((o, fi))
                    elif fi not in o:
                        owner[p] = o | {fi}
    cache = {}

    def simplify_arc(arc, shared):
        rev = arc[::-1]
        canon, flipped = (arc, False) if arc <= rev else (rev, True)
        key = tuple(canon)
        out = cache.get(key)
        if out is None:
            out = dp(canon, T_BORDER_KM if shared else T_COAST_KM)
            cache[key] = out
        return out[::-1] if flipped else out

    def fix_small(orig, simp):
        if len(simp) >= 3:
            return simp
        if len(orig) <= 8:
            return list(orig)
        step = len(orig) / 4.0
        return [orig[int(i * step)] for i in range(4)]

    out = []
    for parts in features:
        nparts = []
        for rings in parts:
            nrings = []
            for ring in rings:
                n = len(ring)
                ow = [owner[p] for p in ring]
                breaks = [i for i in range(n) if ow[i] != ow[i - 1] or ow[i] != ow[(i + 1) % n]]
                if not breaks:
                    # A closed ring with one owner set (an island, or an enclave and the
                    # hole it makes): rotate and orient canonically so both sides of an
                    # enclave simplify identically.
                    m = ring.index(min(ring))
                    rot = ring[m:] + ring[:m]
                    if rot[-1] < rot[1]:
                        rot = [rot[0]] + rot[1:][::-1]
                    shared = not isinstance(ow[0], int)
                    key = ("ring",) + tuple(rot)
                    s = cache.get(key)
                    if s is None:
                        x0, y0 = rot[0]
                        far = max(range(n), key=lambda i: (rot[i][0] - x0) ** 2 + (rot[i][1] - y0) ** 2)
                        tol = T_BORDER_KM if shared else T_COAST_KM
                        a = dp(rot[:far + 1], tol)
                        b = dp(rot[far:] + [rot[0]], tol)
                        s = fix_small(rot, a[:-1] + b[:-1])
                        cache[key] = s
                    nrings.append(s)
                    continue
                res = []
                for k, b0 in enumerate(breaks):
                    b1 = breaks[(k + 1) % len(breaks)]
                    arc = ring[b0:b1 + 1] if b1 > b0 else ring[b0:] + ring[:b1 + 1]
                    shared = len(arc) > 2 and not isinstance(ow[(b0 + 1) % n], int)
                    s = simplify_arc(arc, shared) if len(arc) > 2 else arc
                    res.extend(s[:-1])
                res = fix_small(ring, res)
                nrings.append(res)
            nparts.append(nrings)
        out.append(nparts)
    return out


def ring_area2(r):
    a = 0
    for i in range(len(r)):
        x1, y1 = r[i - 1]
        x2, y2 = r[i]
        a += x1 * y2 - x2 * y1
    return a


# ---- spatial index ----------------------------------------------------------------------

def band_of(y):
    return min(179, max(0, (y + 90 * Q) // Q))


def col_of(x):
    return min(359, max(0, (x + 180 * Q) // Q))


class Layer:
    """Polygons of one layer in the blob's layout, with the Python twin of the
    C++ point-in-polygon (used here to vote admin-1 codes and in self-checks)."""

    def __init__(self):
        self.polys = []   # dict(minx,miny,maxx,maxy, first_ring, ring_count, band_base, owner, first_point, point_end)
        self.rings = []   # ring start indices; a sentinel is appended at write
        self.points = []  # (x, y), every ring closed (last == first)
        self.bands = []   # per polygon: band_count + 1 offsets into edges
        self.edges = []   # point index i: edge (points[i], points[i+1])
        self.cells = {}   # cell index -> list of polygon index | FULL

    def add(self, rings, owner):
        rings = [r for r in rings if len(r) >= 3 and ring_area2(r) != 0]
        if not rings:
            return
        pi = len(self.polys)
        first_ring = len(self.rings)
        first_point = len(self.points)
        for r in rings:
            self.rings.append(len(self.points))
            self.points.extend(r)
            self.points.append(r[0])
        point_end = len(self.points)
        xs = [p[0] for p in self.points[first_point:point_end]]
        ys = [p[1] for p in self.points[first_point:point_end]]
        minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
        b0, b1 = band_of(miny), band_of(maxy)
        per_band = [[] for _ in range(b1 - b0 + 1)]
        touched = set()
        for ri in range(first_ring, len(self.rings)):
            s = self.rings[ri]
            e = self.rings[ri + 1] - 1 if ri + 1 < len(self.rings) else point_end - 1
            for i in range(s, e):
                (x1, y1), (x2, y2) = self.points[i], self.points[i + 1]
                lo, hi = (y1, y2) if y1 < y2 else (y2, y1)
                for b in range(band_of(lo), band_of(hi) + 1):
                    per_band[b - b0].append(i)
                c0, c1 = col_of(min(x1, x2)), col_of(max(x1, x2))
                for r in range(band_of(lo), band_of(hi) + 1):
                    for c in range(c0, c1 + 1):
                        touched.add(r * 360 + c)
        band_base = len(self.bands)
        for lst in per_band:
            self.bands.append(len(self.edges))
            self.edges.extend(lst)
        self.bands.append(len(self.edges))
        # Cells: partial where an edge passes, FULL where the cell centre is inside
        # and no edge passes (the lookup then needs no ray cast at all).
        for r in range(b0, b1 + 1):
            yc = (r - 90) * Q + Q // 2
            xs_cross = []
            for i in per_band[r - b0]:
                (x1, y1), (x2, y2) = self.points[i], self.points[i + 1]
                if (y1 > yc) != (y2 > yc):
                    xs_cross.append(x1 + (x2 - x1) * (yc - y1) / (y2 - y1))
            xs_cross.sort()
            k = 0
            for c in range(col_of(minx), col_of(maxx) + 1):
                cell = r * 360 + c
                if cell in touched:
                    self.cells.setdefault(cell, []).append(pi)
                    continue
                xc = (c - 180) * Q + Q // 2
                while k < len(xs_cross) and xs_cross[k] < xc:
                    k += 1
                if k % 2 == 1:
                    self.cells.setdefault(cell, []).append(pi | FULL)
        self.polys.append({
            "minx": minx, "miny": miny, "maxx": maxx, "maxy": maxy, "first_ring": first_ring,
            "ring_count": len(self.rings) - first_ring, "band_base": band_base, "owner": owner,
            "first_point": first_point, "point_end": point_end,
        })

    def contains(self, pi, x, y):
        p = self.polys[pi]
        if not (p["minx"] <= x <= p["maxx"] and p["miny"] <= y <= p["maxy"]):
            return False
        b = band_of(y) - band_of(p["miny"])
        inside = False
        pts = self.points
        for k in range(self.bands[p["band_base"] + b], self.bands[p["band_base"] + b + 1]):
            i = self.edges[k]
            (x1, y1), (x2, y2) = pts[i], pts[i + 1]
            if (y1 > y) != (y2 > y):
                if y2 > y1:
                    if (x - x1) * (y2 - y1) < (x2 - x1) * (y - y1):
                        inside = not inside
                else:
                    if (x - x1) * (y2 - y1) > (x2 - x1) * (y - y1):
                        inside = not inside
        return inside

    def find(self, x, y):
        cell = band_of(y) * 360 + col_of(x)
        for c in self.cells.get(cell, []):
            if c & FULL:
                return c & ~FULL
            if self.contains(c, x, y):
                return c
        return None


# ---- build ------------------------------------------------------------------------------

def in_boxes(boxes, x, y):
    return any(b[0] <= x <= b[2] and b[1] <= y <= b[3] for b in boxes)


def parts_touch(boxes, parts):
    for rings in parts:
        xs = [p[0] / Q for p in rings[0]]
        ys = [p[1] / Q for p in rings[0]]
        for b in boxes:
            if min(xs) <= b[2] and max(xs) >= b[0] and min(ys) <= b[3] and max(ys) >= b[1]:
                return True
    return False


NE0_NAME_KEYS = ("NAME", "NAME_LONG", "NAME_ALT", "NAME_EN", "NAME_SORT", "ADMIN", "GEOUNIT", "SUBUNIT",
                 "BRK_NAME", "FORMAL_EN", "NAME_CIAWF")
NE1_NAME_KEYS = ("name", "name_en", "name_local", "gn_name", "woe_name")


def lang_names(props, upper):
    out = []
    for k, v in props.items():
        if isinstance(v, str) and v and (k.startswith("NAME_") if upper else k.startswith("name_")):
            suffix = k[5:]
            if len(suffix) in (2, 3) and suffix.isalpha():
                out.append((suffix.lower(), v))
    return out


def build(args):
    dl = args.downloads
    hashes = fetch(dl, args.accept_upstream)
    t0 = time.time()
    boxes = SUBSETS.get(args.subset) if args.subset else None

    cinfo = read_country_info(os.path.join(dl, SOURCES["countries"]["file"]))
    a1 = read_admin1(os.path.join(dl, SOURCES["admin1codes"]["file"]))
    towns = read_cities(os.path.join(dl, SOURCES["cities"]["file"]))
    if boxes:
        towns = [t for t in towns if in_boxes(boxes, t["lon"], t["lat"])]
    log(f"{len(towns)} towns, {len(cinfo)} countries, {len(a1)} admin-1 codes")

    with open(os.path.join(dl, SOURCES["admin0"]["file"]), encoding="utf-8") as f:
        ne0 = json.load(f)["features"]
    with open(os.path.join(dl, SOURCES["admin1"]["file"]), encoding="utf-8") as f:
        ne1 = json.load(f)["features"]

    # Countries: every GeoNames code, plus Natural Earth features that have none.
    countries = {}  # key -> dict(code, name, native, aliases set)
    for code, c in cinfo.items():
        countries[code] = {"code": code, "name": c["name"], "native": "", "aliases": {c["name"]}, "langs": c["langs"],
                           "pop": -1}
    f0_keys = []
    f0_parts = []
    for feat in ne0:
        p = feat["properties"]
        code = p.get("ISO_A2_EH") or ""
        if len(code) != 2 or not code.isalpha():
            code = NE_CODE_OVERRIDES.get(p.get("ADM0_A3"), "")
        key = code if code else "ne:" + p["ADM0_A3"]
        if key not in countries:
            countries[key] = {"code": code, "name": p.get("NAME_EN") or p["NAME"], "native": "", "aliases": set(),
                              "langs": [], "pop": -1}
        c = countries[key]
        for k in NE0_NAME_KEYS:
            v = p.get(k)
            if isinstance(v, str) and v:
                c["aliases"].update(x.strip() for x in v.split("|") if x.strip())
        langs = lang_names(p, True)
        c["aliases"].update(v for _, v in langs)
        if (p.get("POP_EST") or 0) > c["pop"]:
            c["pop"] = p.get("POP_EST") or 0
            for lang in c["langs"][:1]:
                for l, v in langs:
                    if l == lang:
                        c["native"] = v
        parts = feature_parts(feat)
        if boxes:
            parts = [q for q in parts if parts_touch(boxes, [q])]
            if not parts:
                continue
        f0_keys.append(key)
        f0_parts.append(parts)

    f1_props = []
    f1_parts = []
    for feat in ne1:
        parts = feature_parts(feat)
        if boxes:
            parts = [q for q in parts if parts_touch(boxes, [q])]
        if not parts:
            continue
        f1_props.append(feat["properties"])
        f1_parts.append(parts)
    log(f"admin-0 {len(f0_parts)} features, admin-1 {len(f1_parts)} features; simplifying")

    s0 = simplify_layer(f0_parts)
    s1 = simplify_layer(f1_parts)
    log(f"simplified in {time.time() - t0:.0f} s")

    country_keys = list(countries.keys())
    cidx = {k: i for i, k in enumerate(country_keys)}

    layer0 = Layer()
    for key, parts in zip(f0_keys, s0):
        for rings in parts:
            layer0.add(rings, cidx[key])

    # Regions: GeoNames admin-1. Each Natural Earth admin-1 feature takes the
    # GeoNames code most of the towns inside it carry (Natural Earth divides
    # some countries finer than GeoNames' first level: French departments map
    # to their region, UK councils to England/Scotland/Wales/Northern Ireland).
    #
    # Towns of any country vote, weighted by population, so a capital beats
    # the suburbs that simplification lets in at the edge (Oslo, not Akershus),
    # and a territory GeoNames files as its own country while Natural Earth
    # draws it inside another (Svalbard in Norway, Réunion in France) takes
    # the GeoNames country: the region record then carries REGION_COUNTRY and
    # the lookup lets it override the admin-0 country.
    town_cells = {}
    for t in towns:
        town_cells.setdefault((int(math.floor(t["lat"])), int(math.floor(t["lon"]))), []).append(t)
    region_keys = []
    region_info = {}  # key -> dict(country key, code, name, ascii, aliases, ne_count)

    def region_key(code, cc):
        if code not in region_info:
            g = a1.get(code)
            region_info[code] = {"country": cc, "code": code if g else "", "name": g["name"] if g else "",
                                 "ascii": g["ascii"] if g else "", "aliases": set(), "ne": 0}
            region_keys.append(code)
        return code

    # Layer-1 polygons are added after the vote, which needs the polygons first.
    tmp1 = Layer()
    feat_polys = []
    for fi, parts in enumerate(s1):
        start = len(tmp1.polys)
        for rings in parts:
            tmp1.add(rings, 0)
        feat_polys.append(range(start, len(tmp1.polys)))

    feat_region = []
    for fi, p in enumerate(f1_props):
        cc = p.get("iso_a2") or ""
        if cc not in countries:
            cc = NE_CODE_OVERRIDES.get(p.get("adm0_a3"), cc)
        polys = [tmp1.polys[pi] for pi in feat_polys[fi]]
        cand = []
        for pp in polys:
            for r_ in range(pp["miny"] // Q, pp["maxy"] // Q + 1):
                for c_ in range(pp["minx"] // Q, pp["maxx"] // Q + 1):
                    cand.extend(town_cells.get((r_, c_), ()))
        lx, ly = p.get("longitude"), p.get("latitude")
        if lx is not None and ly is not None and len(cand) > 300:
            kx = math.cos(math.radians(ly))
            cand = sorted(cand, key=lambda t: ((t["lon"] - lx) * kx) ** 2 + (t["lat"] - ly) ** 2)[:300]
        country_votes = {}
        code_votes = {}
        inside = 0
        for t in cand:
            x, y = qx(t["lon"]), qx(t["lat"])
            if not any(tmp1.contains(pi, x, y) for pi in feat_polys[fi]):
                continue
            w = max(t["pop"], 1000)
            country_votes[t["cc"]] = country_votes.get(t["cc"], 0) + w
            code = f"{t['cc']}.{t['a1']}"
            if t["a1"] and t["a1"] != "00" and code in a1:
                code_votes[code] = code_votes.get(code, 0) + w
            inside += 1
            if inside >= 40:
                break
        owner_cc = cc
        override = False
        if country_votes:
            top = max(sorted(country_votes), key=lambda k: country_votes[k])
            if top != cc and top in countries and country_votes[top] >= 0.8 * sum(country_votes.values()):
                owner_cc, override = top, True
        votes = {k: v for k, v in code_votes.items() if k.startswith(owner_cc + ".")}
        gn = p.get("gn_a1_code") or ""
        if not override and gn in a1 and (gn in votes or not votes):
            # Natural Earth's own GeoNames code, confirmed by a town inside it:
            # trusted over the vote (Vaduz's commune, not its bigger neighbour's
            # town that simplification let in at the edge).
            code = gn
        elif votes and max(votes.values()) >= 0.5 * sum(votes.values()):
            code = max(sorted(votes), key=lambda k: votes[k])
        else:
            # No code, or a feature coarser than GeoNames' first level (all of
            # Puerto Rico against its 78 municipalities): a Natural Earth-only
            # region, so no town is filtered out by it.
            code = "ne:" + (p.get("adm1_code") or str(fi))
        rk = region_key(code, owner_cc if owner_cc in countries else "")
        r = region_info[rk]
        r["override"] = r.get("override", False) or override
        if not r["name"]:
            r["name"] = p.get("name") or p.get("name_en") or ""
        r["ne"] += 1
        names = set()
        for k in NE1_NAME_KEYS:
            v = p.get(k)
            if isinstance(v, str) and v:
                names.add(v)
        alt = p.get("name_alt")
        if isinstance(alt, str):
            names.update(x.strip() for x in alt.split("|") if x.strip())
        r.setdefault("ne_names", []).append(names)
        feat_region.append(rk)

    # GeoNames admin-1 codes referenced only by towns still get a record, so a
    # town's region is always named.
    for t in towns:
        code = f"{t['cc']}.{t['a1']}"
        if code in a1 and t["cc"] in countries:
            region_key(code, t["cc"])

    for rk in region_keys:
        r = region_info[rk]
        for n in (r["name"], r["ascii"]):
            if n:
                r["aliases"].add(n)
        # Natural Earth names only when the feature IS the region; a French
        # department's name must not find its whole region.
        if r["ne"] == 1:
            r["aliases"].update(r["ne_names"][0])

    ridx = {k: i for i, k in enumerate(region_keys)}
    if len(region_keys) >= NO_REGION or len(country_keys) >= 0xFFFF:
        sys.exit("build_places: too many regions or countries for u16")

    layer1 = Layer()
    for fi, parts in enumerate(s1):
        for rings in parts:
            layer1.add(rings, ridx[feat_region[fi]])

    # Drop countries and regions nothing refers to (matters for the test subset).
    used_c = set(p["owner"] for p in layer0.polys)
    used_r = set(p["owner"] for p in layer1.polys)
    for t in towns:
        if t["cc"] in cidx:
            used_c.add(cidx[t["cc"]])
            code = f"{t['cc']}.{t['a1']}"
            if code in ridx:
                used_r.add(ridx[code])
    for ri in used_r:
        ck = region_info[region_keys[ri]]["country"]
        if ck in cidx:
            used_c.add(cidx[ck])
    c_new = {old: new for new, old in enumerate(sorted(used_c))}
    r_new = {old: new for new, old in enumerate(sorted(used_r))}
    for p in layer0.polys:
        p["owner"] = c_new[p["owner"]]
    for p in layer1.polys:
        p["owner"] = r_new[p["owner"]]

    strings = StringPool()
    country_recs = []
    for old in sorted(used_c):
        c = countries[country_keys[old]]
        aliases = sorted(a for a in c["aliases"] if a and a != c["name"])
        country_recs.append(struct.pack("<2sHIII", c["code"].encode("ascii") if c["code"] else b"\0\0", 0,
                                        strings.add(c["name"]), strings.add(c["native"] if c["native"] != c["name"] else ""),
                                        strings.add("\n".join(aliases))))
    region_recs = []
    for old in sorted(used_r):
        r = region_info[region_keys[old]]
        ck = r["country"]
        ci = c_new.get(cidx.get(ck, -1), 0xFFFF)
        aliases = sorted(a for a in r["aliases"] if a and a != r["name"])
        region_recs.append(struct.pack("<HHIII", ci, REGION_COUNTRY if r.get("override") else 0, strings.add(r["code"]), strings.add(r["name"]),
                                       strings.add("\n".join(aliases))))

    # Towns, sorted by grid cell for nearest-neighbour lookup.
    trecs = []
    for t in towns:
        if t["cc"] not in cidx or cidx[t["cc"]] not in c_new:
            continue
        x, y = qx(t["lon"]), qx(t["lat"])
        code = f"{t['cc']}.{t['a1']}"
        ri = r_new.get(ridx.get(code, -1), NO_REGION)
        trecs.append((band_of(y) * 360 + col_of(x), y, x, t["name"], t["pop"], c_new[cidx[t["cc"]]], ri))
    trecs.sort(key=lambda r: (r[0], -r[4], r[3]))
    town_cells = {}
    town_bytes = bytearray()
    for i, (cell, y, x, name, pop, ci, ri) in enumerate(trecs):
        town_cells.setdefault(cell, [i, i])[1] = i + 1
        town_bytes += struct.pack("<iiIIHH", y, x, strings.add(name), min(pop, 0xFFFFFFFF), ci, ri)
    town_rows, town_offsets = sparse_rows({c: (b, e) for c, (b, e) in town_cells.items()}, len(trecs))

    meta = {
        "format": VERSION,
        "subset": args.subset or "",
        "built": time.strftime("%Y-%m-%d", time.gmtime()),
        "natural_earth": NE_TAG,
        "inputs": {SOURCES[k]["file"]: v for k, v in hashes.items()},
        "attribution": "Place names: GeoNames (geonames.org), CC-BY 4.0. Borders: Natural Earth, public domain.",
        "tolerance_km": {"coast": T_COAST_KM, "border": T_BORDER_KM},
    }

    sections = [
        (SEC_COUNTRIES, 16, b"".join(country_recs)),
        (SEC_REGIONS, 16, b"".join(region_recs)),
        (SEC_TOWNS, 20, bytes(town_bytes)),
        (SEC_TOWN_ROWS, 12, town_rows),
        (SEC_TOWN_CELLS, 4, town_offsets),
        (SEC_META, 1, json.dumps(meta, sort_keys=True).encode("utf-8")),
    ]
    for li, layer in enumerate((layer0, layer1)):
        sections += layer_sections(LAYER_BASE[li], layer)
    sections.insert(0, (SEC_STRINGS, 1, strings.bytes()))

    blob = write_blob(sections)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    tmp = args.out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(blob)
    os.replace(tmp, args.out)
    log(f"wrote {args.out}: {len(blob)} bytes; countries {len(country_recs)}, regions {len(region_recs)}, "
        f"towns {len(trecs)}, points {len(layer0.points)} + {len(layer1.points)}, {time.time() - t0:.0f} s")
    for sid, _, data in sections:
        log(f"  section {sid:3d}: {len(data)} bytes")


class StringPool:
    def __init__(self):
        self.buf = bytearray(b"\0")
        self.index = {"": 0}

    def add(self, s):
        s = s or ""
        if "\0" in s:
            s = s.replace("\0", "")
        o = self.index.get(s)
        if o is None:
            o = len(self.buf)
            self.buf += s.encode("utf-8") + b"\0"
            self.index[s] = o
        return o

    def bytes(self):
        return bytes(self.buf)


def sparse_rows(cells, total):
    """cells: cell -> (begin, end) into a flat list. Rows of the 1-degree grid
    store only the column span they use, so an empty ocean row costs 12 bytes:
    row r: (col_begin, col_end, base); offsets[base + (c - col_begin)] and the
    next entry bound cell c's run."""
    rows = bytearray()
    offs = []
    by_row = {}
    for cell, be in cells.items():
        by_row.setdefault(cell // 360, {})[cell % 360] = be
    for r in range(180):
        cols = by_row.get(r)
        if not cols:
            rows += struct.pack("<III", 0, 0, len(offs))
            continue
        c0, c1 = min(cols), max(cols) + 1
        base = len(offs)
        # Runs are contiguous and increasing (towns sorted by cell; candidates
        # appended in cell order), so each cell's begin is the previous end.
        prev = cols[c0][0]
        for c in range(c0, c1):
            if c in cols:
                prev = cols[c][0]
                offs.append(prev)
                prev = cols[c][1]
            else:
                offs.append(prev)
        offs.append(prev)
        rows += struct.pack("<III", c0, c1, base)
    return bytes(rows), struct.pack(f"<{len(offs)}I", *offs)


def layer_sections(base, layer):
    polys = bytearray()
    for p in layer.polys:
        polys += struct.pack("<iiiiIIIHHII", p["minx"], p["miny"], p["maxx"], p["maxy"], p["first_ring"],
                             p["ring_count"], p["band_base"], p["owner"], 0, p["first_point"], p["point_end"])
    rings = struct.pack(f"<{len(layer.rings) + 1}I", *(layer.rings + [len(layer.points)]))
    pts = bytearray(struct.pack(f"<{2 * len(layer.points)}i", *(v for p in layer.points for v in p)))
    bands = struct.pack(f"<{len(layer.bands)}I", *layer.bands)
    edges = struct.pack(f"<{len(layer.edges)}I", *layer.edges)
    cands = []
    cells = {}
    for cell in sorted(layer.cells):
        b = len(cands)
        # FULL candidates first: the lookup returns on the first one.
        cands.extend(sorted(layer.cells[cell], key=lambda v: 0 if v & FULL else 1))
        cells[cell] = (b, len(cands))
    rows, offs = sparse_rows(cells, len(cands))
    return [
        (base + L_POLYS, 40, bytes(polys)),
        (base + L_RINGS, 4, rings),
        (base + L_POINTS, 8, bytes(pts)),
        (base + L_BANDS, 4, bands),
        (base + L_EDGES, 4, edges),
        (base + L_ROWS, 12, rows),
        (base + L_CELLS, 4, offs),
        (base + L_CANDS, 4, struct.pack(f"<{len(cands)}I", *cands)),
    ]


M64 = (1 << 64) - 1


def checksum(data):
    """The blob checksum (src/geo/geo.cpp checksum()): four lanes over
    little-endian u64 words, each h = (h ^ w) * K; h ^= h >> 29, folded at the end."""
    n = len(data) // 8
    words = struct.unpack(f"<{n}Q", data[:n * 8])
    lanes = [0x243F6A8885A308D3, 0x13198A2E03707344, 0xA4093822299F31D0, 0x082EFA98EC4E6C89]
    K = 0x9E3779B97F4A7C15
    for l in range(4):
        h = lanes[l]
        for w in words[l::4]:
            h = ((h ^ w) * K) & M64
            h ^= h >> 29
        lanes[l] = h
    r = 0
    for h in lanes:
        r = ((r ^ h) * 0xBF58476D1CE4E5B9) & M64
        r ^= r >> 31
    return r


def write_blob(sections):
    """Header (64 bytes), section table (24 bytes each), then sections, each
    8-byte aligned. Checksum covers everything after the header."""
    header_size = 64
    table_size = 24 * len(sections)
    off = header_size + table_size
    off = (off + 7) & ~7
    table = bytearray()
    body = bytearray()
    for sid, esize, data in sections:
        pad = (-(off + len(body))) % 8
        body += b"\0" * pad
        table += struct.pack("<IIQQ", sid, esize, off + len(body), len(data))
        body += data
    body += b"\0" * ((-(off + len(body))) % 8)
    pre = header_size + table_size
    gap = b"\0" * (off - pre)
    rest = bytes(table) + gap + bytes(body)
    total = header_size + len(rest)
    head = struct.pack("<8sIIQQII", MAGIC, VERSION, header_size, total, checksum(rest), len(sections), 0)
    head += b"\0" * (header_size - len(head))
    return head + rest


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--downloads", required=True, help="directory for the pinned downloads (not the source tree)")
    ap.add_argument("--out", required=True, help="the blob to write")
    ap.add_argument("--subset", choices=sorted(SUBSETS), help="a small blob for tests")
    ap.add_argument("--accept-upstream", action="store_true", help="take a GeoNames file whose hash moved")
    args = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    if os.path.abspath(args.downloads).startswith(os.path.join(repo, "src")):
        sys.exit("build_places: keep downloads out of the source tree (use build/ or a cache dir)")
    build(args)


if __name__ == "__main__":
    main()
