# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
# #354 D2: P@5 of the 20 visual queries over ai-bench's output, against the
# labels make_doc_pages.swift wrote. usage: score_doc_pages.py <jsonl> <labels.tsv>
import json, sys
from pathlib import Path
labels = {}
for line in Path(sys.argv[2]).read_text().splitlines()[1:]:
    f, kind, page, pages = line.split("\t"); labels[f] = (kind, int(page))
want = {"a receipt": "receipt", "a floor plan": "floor plan", "a bar chart": "bar chart", "a pie chart": "pie chart",
        "a line graph": "line graph", "a presentation slide": "slide", "sheet music": "sheet music",
        "a calendar": "calendar", "a crossword puzzle": "crossword", "a chessboard": "chessboard",
        "a flowchart": "flowchart", "a map": "map", "a QR code": "qr code", "a barcode": "barcode",
        "a spreadsheet": "spreadsheet", "a certificate": "certificate", "a sudoku puzzle": "sudoku",
        "a circuit diagram": "circuit", "a photo of a person": "photo", "a business card": "business card"}
total = 0.0; n = 0; rows = []; right_page = 0; hits = 0; slow = 0.0
for line in Path(sys.argv[1]).read_text().splitlines():
    if not line.startswith('{"query"'): continue
    q = json.loads(line); k = want[q["query"]]
    top = q["top"][:5]; slow = max(slow, q["ms"])
    good = 0
    for t in top:
        name = t["file"].rsplit("/", 1)[-1]
        lab = labels.get(name)
        if lab and lab[0] == k:
            good += 1; hits += 1
            right_page += t["ms"] == lab[1]
    p = good / 5; total += p; n += 1
    why = " ".join(f'{t["file"].rsplit("/",1)[-1]}:{labels.get(t["file"].rsplit("/",1)[-1], ("text",0))[0]}@{t["ms"]}/m{t["match"]}' for t in top if labels.get(t["file"].rsplit("/",1)[-1], ("",))[0] != k)
    rows.append(f'{q["query"]:24s} P@5 {p:.1f}  results {q["results"]:4d}  {q["ms"]:6.1f} ms  {why}')
print("\n".join(rows))
print(f"mean P@5 {total / n:.3f} over {n} queries; relevant hits on the labelled page {right_page}/{hits}; slowest {slow:.1f} ms")
