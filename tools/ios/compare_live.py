#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/ios/search-live.sh's comparison: the tiles the phone showed against the Mac's first rows.

The phone's tiles are catalogue asset ids, in reading order; the Mac's rows are paths on the Mac.
Both are turned into the catalogue's paths. A query matches when the phone's tiles are exactly the
Mac's first rows, in order (the screen shows as many as fit); top-k overlap is reported too.
"""
import json
import sqlite3
import sys


def main():
    mac_file, phone_file, catalog_db, root = sys.argv[1:5]
    db = sqlite3.connect(f"file:{catalog_db}?mode=ro", uri=True)
    by_id = {str(i): p for i, p in db.execute("SELECT id, path FROM assets")}
    # The root's own uuid: the first path segment the catalogue gives this root's files.
    uuid = next(iter(by_id.values())).split("/")[1]
    mac = {}
    with open(mac_file, encoding="utf-8") as f:
        for line in f:
            d = json.loads(line)
            mac[d["query"]] = ["/" + uuid + r[0][len(root):] for r in d["rows"]]
    phone = json.load(open(phone_file, encoding="utf-8"))
    tried = exact = 0
    overlap_sum = 0.0
    shown = 0
    for answer in phone:
        q = answer["query"]
        tiles = [by_id.get(t, "?" + t) for t in answer["tiles"]]
        want = mac.get(q, [])
        k = len(tiles)
        tried += 1
        if k == 0 and not want:
            exact += 1
            overlap_sum += 1
            continue
        top = want[:k] if k else want[:1]
        same = tiles == top
        exact += same
        overlap = len(set(tiles) & set(top)) / max(1, len(top))
        overlap_sum += overlap
        shown += k
        if not same:
            print(f"DIFFER {q!r}: phone {len(tiles)} tiles, Mac {len(want)} rows, overlap {overlap:.2f}")
    print(json.dumps({"queries": tried, "identical_top_k": exact, "mean_top_k_overlap": round(overlap_sum / max(1, tried), 4),
                      "tiles_compared": shown}))
    return 0 if exact == tried else 1


if __name__ == "__main__":
    sys.exit(main())
