#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Extract the completed workflow's per-agent results from the session journal
into individual markdown files, so they can be read and assembled into a report."""
import json, os, re, sys

J = r"C:/Users/Lenovo/.claude/projects/d--lixf-S---20260817/993be7a5-4630-4b71-805a-980dfc9812a2/subagents/workflows/wf_b1c46e35-fd3/journal.jsonl"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results")
os.makedirs(OUT, exist_ok=True)

started = {}
rows = []
for i, line in enumerate(open(J, encoding="utf-8"), 1):
    line = line.strip()
    if not line:
        continue
    try:
        d = json.loads(line)
    except Exception as e:
        print("PARSE_ERR line", i, e)
        continue
    t = d.get("type")
    if t == "started":
        started[d.get("key")] = d
    elif t in ("result", "failed"):
        s = started.get(d.get("key"), {})
        rows.append((i, t, s.get("phase", ""), s.get("label", ""), d.get("agentId", ""), d))


def slug(x, n=36):
    x = re.sub(r"[^\w\u4e00-\u9fff]+", "-", x).strip("-")
    return (x[:n] or "unnamed")


manifest = []
total = 0
for i, t, phase, label, aid, d in rows:
    res = d.get("result")
    err = d.get("error")
    if isinstance(res, str):
        body = res
    elif isinstance(res, dict):
        parts = []
        for k, v in res.items():
            txt = v if isinstance(v, str) else json.dumps(v, ensure_ascii=False, indent=1)
            parts.append("## [%s]\n\n%s" % (k, txt))
        body = "\n\n---\n\n".join(parts)
    else:
        body = json.dumps(res, ensure_ascii=False, indent=1)
    if err:
        body = (body + "\n\n" if body else "") + "## [error]\n\n" + str(err)
    nb = len(body.encode("utf-8"))
    fn = "%02d_%s_%s__%s.md" % (i, t, slug(phase, 14), slug(label))
    hdr = ("---\nline: %d\ntype: %s\nphase: %s\nlabel: %s\nagentId: %s\nbytes: %d\n---\n\n"
           % (i, t, phase, label, aid, nb))
    with open(os.path.join(OUT, fn), "w", encoding="utf-8") as f:
        f.write(hdr + body)
    total += nb
    manifest.append({"line": i, "type": t, "phase": phase, "label": label,
                     "agentId": aid, "file": fn, "chars": len(body), "bytes": nb})
    print("%3d %-7s %-12s %-36s %8dch -> %s" % (i, t, phase, label[:34], len(body), fn))

with open(os.path.join(OUT, "_manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f, ensure_ascii=False, indent=1)

print("\nfiles: %d  total_chars: %d  approx_tokens: %d" % (len(manifest), total, total // 2))
print("OUT:", OUT)
