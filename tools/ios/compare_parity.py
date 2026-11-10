#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""docs/plans/mobile.md 9: the phone and the desktop on one generation, row for row.

Rows are (path, moment, kind, match, more-in-clip) and must match in order. Scores must agree to
within --score-delta; they are reported, not ranked, since both towers ran on the CPU.
"""
import argparse
import json
import sys


def load(path):
    answers = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            doc = json.loads(line)
            if doc["query"] in answers:
                raise SystemExit(f"{path}: {doc['query']!r} twice: every query counts once")
            answers[doc["query"]] = doc
    return answers


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("desktop")
    ap.add_argument("phone")
    # docs/plans/mobile.md M5's verify line: the 200-query parity set.
    ap.add_argument("--min-queries", type=int, default=200)
    ap.add_argument("--score-delta", type=float, default=1e-4)
    a = ap.parse_args()
    desktop, phone = load(a.desktop), load(a.phone)
    if set(desktop) != set(phone):
        print("different query sets:", sorted(set(desktop) ^ set(phone))[:5])
        return 1
    if len(desktop) < a.min_queries:
        print(f"only {len(desktop)} queries, need {a.min_queries}")
        return 1
    bad = 0
    biggest = 0.0
    rows_total = 0
    empties = 0
    with_hits = 0
    for q, d in desktop.items():
        p = phone[q]
        dr = [r[:5] for r in d["rows"]]
        pr = [r[:5] for r in p["rows"]]
        rows_total += len(dr)
        if dr:
            with_hits += 1
        else:
            empties += 1
        if dr != pr:
            bad += 1
            print(f"DIFFER: {q!r}: desktop {len(dr)} rows, phone {len(pr)} rows")
            for i, (x, y) in enumerate(zip(dr, pr)):
                if x != y:
                    print(f"  first difference at row {i}: {x} / {y}")
                    break
            continue
        for x, y in zip(d["rows"], p["rows"]):
            delta = abs(x[5] - y[5])
            biggest = max(biggest, delta)
            if delta > a.score_delta:
                bad += 1
                print(f"SCORE: {q!r}: {x[0]} {x[5]} vs {y[5]}")
                break
    print(json.dumps({
        "queries": len(desktop),
        "queries_with_rows": with_hits,
        "queries_without_rows": empties,
        "rows": rows_total,
        "queries_differing": bad,
        "largest_score_delta": biggest,
    }))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
