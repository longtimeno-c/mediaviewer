#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/ios/search-live.sh's comparison: the phone's search screen against the Mac's rows.

The phone gives, per query, the tiles on screen (catalogue asset ids, in reading order) and how many
results there are in all; the Mac's rows are paths on the Mac, turned into catalogue paths here.
A query is identical when the phone has as many results as the Mac and its first k tiles are the
Mac's first k rows in order, with k = min(tiles on screen, Mac's rows). Each difference is
classified: a sound match (CLAP, which the phone has no tower for) among the Mac's rows on screen,
or anywhere in them when the counts differ, or not; the second kind is unexplained.
"""
import json
import sqlite3
import sys

SOUND = 2  # MV_AI_MATCH_SOUND


def main():
    mac_file, phone_file, catalog_db, root = sys.argv[1:5]
    db = sqlite3.connect(f"file:{catalog_db}?mode=ro", uri=True)
    by_id = {str(i): p for i, p in db.execute("SELECT id, path FROM assets")}
    uuid = next(iter(by_id.values())).split("/")[1]
    mac = {}
    with open(mac_file, encoding="utf-8") as f:
        for line in f:
            d = json.loads(line)
            mac[d["query"]] = [("/" + uuid + r[0][len(root):], r[3]) for r in d["rows"]]
    phone = json.load(open(phone_file, encoding="utf-8"))
    tried = identical = sound = unexplained = 0
    overlap_sum = 0.0
    compared = 0
    for answer in phone:
        q = answer["query"]
        if q not in mac:
            print(f"NO MAC ANSWER {q!r}")
            unexplained += 1
            tried += 1
            continue
        tiles = [by_id.get(t, "?" + t) for t in answer["tiles"]]
        total = answer.get("total", len(tiles))
        rows = mac[q]
        k = min(len(tiles), len(rows))
        want = [p for p, _ in rows[:k]]
        tried += 1
        compared += k
        same_count = total == len(rows)
        same_top = tiles[:k] == want
        overlap = 1.0 if k == 0 and len(tiles) == len(rows) else len(set(tiles[:k]) & set(want)) / max(1, k)
        overlap_sum += overlap
        if same_count and same_top and len(tiles) >= k:
            identical += 1
            continue
        # The Mac's rows the phone could have shown, and, when the counts differ, every Mac row:
        # a sound match anywhere can be the row the phone lacks.
        heard = any(m & SOUND for _, m in (rows if total != len(rows) else rows[:max(k, len(tiles), 1)]))
        if heard:
            sound += 1
        else:
            unexplained += 1
        why = "sound-matched rows in the Mac's top" if heard else "UNEXPLAINED"
        print(f"DIFFER {q!r}: phone {total} results ({len(tiles)} on screen), Mac {len(rows)} rows, "
              f"top-{k} overlap {overlap:.2f}: {why}")
    print(json.dumps({"queries": tried, "identical": identical, "differ_sound": sound,
                      "differ_unexplained": unexplained, "mean_top_k_overlap": round(overlap_sum / max(1, tried), 4),
                      "rows_compared": compared}))
    return 0 if identical == tried else 1


if __name__ == "__main__":
    sys.exit(main())
