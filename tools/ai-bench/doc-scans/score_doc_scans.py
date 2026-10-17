# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
# #354 D4: how many of each scanned page's words a quoted search finds, over
# ai-bench --brief output, against the truth make_doc_scans.swift wrote.
# usage: score_doc_scans.py <jsonl> <truth.tsv>
#
# A word is found when its quoted search returns the page's document (the 50
# one-page scans). Words the search ignores are not scored: the engine's stop
# words (engine.cpp stop_word) and single characters. Each book page's 4-word
# phrase is found when the book comes back at that page.
import json, re, statistics, sys
from pathlib import Path

STOP = set("a an the of on in at to is are and or with someone somebody says said saying say who where when that "
           "this it was be for clip video videos moment photos photo about".split())
truth = {}
for line in Path(sys.argv[2]).read_text().splitlines()[1:]:
    f, page, words = line.split("\t")
    truth[(f, int(page))] = words.split()
answers = {}
slow = 0.0
for line in Path(sys.argv[1]).read_text().splitlines():
    if not line.startswith('{"query"'):
        continue
    try:
        q = json.loads(line)
    except json.JSONDecodeError:  # an ai-bench before it escaped the query's quotes
        m = re.match(r'^\{"query":"(.*)","ms":([0-9.]+),"results":(\d+),"top":(\[.*\])\}$', line)
        q = {"query": m.group(1), "ms": float(m.group(2)), "top": json.loads(m.group(4))}
    answers[q["query"]] = {(t[0], t[1]) for t in q["top"]}
    slow = max(slow, q["ms"])

rows = []
for (f, page), words in sorted(truth.items()):
    if f == "book_scan.pdf":
        continue
    scored = sorted({w for w in words if w not in STOP and len(w) > 1})
    found = [w for w in scored if any(name == f for name, _ in answers.get(f'"{w}"', ()))]
    missed = [w for w in scored if w not in found]
    rows.append((len(found) / len(scored), f, len(scored), missed))
fracs = [r[0] for r in rows]
for frac, f, n, missed in sorted(rows):
    print(f"{f}  {frac:6.1%} of {n:3d}  missed: {' '.join(missed[:12])}{' ...' if len(missed) > 12 else ''}")
print(f"pages {len(rows)}; per page: mean {statistics.mean(fracs):.1%}, median {statistics.median(fracs):.1%}, "
      f"min {min(fracs):.1%}; pages at >= 90 %: {sum(x >= 0.9 for x in fracs)} of {len(rows)}")

book_hits = 0
book_pages = 0
for (f, page), words in sorted(truth.items()):
    if f != "book_scan.pdf":
        continue
    book_pages += 1
    mid = len(words) // 2
    phrase = '"' + " ".join(words[mid:mid + 4]) + '"'
    book_hits += ("book_scan.pdf", page) in answers.get(phrase, ())
print(f"book: {book_hits} of {book_pages} pages found at their page by a 4-word phrase; slowest query {slow:.1f} ms")
