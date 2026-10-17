# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
# #354 D3: P@5 and R@10 of the spike's 50 queries over ai-bench's output (the
# engine itself: words and meaning merged, every passage embedded), judged as
# spike.py judges them. usage: score_bench.py <ai-bench jsonl> <queries.json> <corpus.json>
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from spike import relevant_set  # noqa: E402

qs = json.loads(Path(sys.argv[2]).read_text())["queries"]
corpus = json.loads(Path(sys.argv[3]).read_text())
want = {q["q"]: relevant_set(q, corpus) for q in qs}
lang = {q["q"]: q.get("lang", "en") for q in qs}
p5s, r10s, slow, empty = {}, {}, 0.0, []
for line in Path(sys.argv[1]).read_text().splitlines():
    if not line.startswith('{"query"'):
        continue
    try:
        q = json.loads(line)
        files = [t["file"] for t in q["top"]]
    except json.JSONDecodeError:  # an ai-bench before snippets were escaped
        q = {"query": re.match(r'\{"query":"(.*?)","ms"', line).group(1),
             "ms": float(re.search(r'"ms":([0-9.]+)', line).group(1))}
        files = re.findall(r'\{"file":"(.*?)","ms"', line)
    rel = want[q["query"]]
    top = [f.rsplit("/", 1)[-1] for f in files]
    slow = max(slow, q["ms"])
    if not top:
        empty.append(q["query"])
    p5s[q["query"]] = len(set(top[:5]) & rel) / max(1, min(5, len(rel)))
    r10s[q["query"]] = len(set(top[:10]) & rel) / max(1, min(10, len(rel)))
for name, keep in (("all", lambda q: True), ("english", lambda q: lang[q] == "en"), ("other", lambda q: lang[q] != "en")):
    ks = [k for k in p5s if keep(k)]
    if ks:
        print(f"{name:8s} P@5 {sum(p5s[k] for k in ks) / len(ks):.3f}  R@10 {sum(r10s[k] for k in ks) / len(ks):.3f}  n={len(ks)}")
print(f"slowest query {slow:.1f} ms; answered nothing: {len(empty)} {empty}")
