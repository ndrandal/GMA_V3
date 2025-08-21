#!/usr/bin/env python3
import re, sys, os, csv

ROOT = sys.argv[1] if len(sys.argv) > 1 else "."
PAT = re.compile(r'//\s*(TODO|FIXME|NOTE)\s*:?\s*(.*)$|/\*\s*(TODO|FIXME|NOTE)\s*:?\s*(.*?)\*/', re.IGNORECASE)

rows=[]
for dirpath, _, filenames in os.walk(ROOT):
    for fn in filenames:
        if not fn.endswith(('.hpp','.h','.hh','.cpp','.cc','.cxx','.md','.json')): continue
        path = os.path.join(dirpath, fn)
        try:
            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                for i, line in enumerate(f, 1):
                    m = PAT.search(line)
                    if m:
                        tag = (m.group(1) or m.group(3) or '').upper()
                        msg = (m.group(2) or m.group(4) or '').strip()
                        rows.append([path, i, tag, msg])
        except Exception as e:
            rows.append([path, 0, "ERROR", str(e)])

os.makedirs("artifacts", exist_ok=True)
with open("artifacts/T1_todos.csv","w",newline='',encoding='utf-8') as f:
    w=csv.writer(f); w.writerow(["file","line","tag","message"]); w.writerows(rows)

print(f"Wrote artifacts/T1_todos.csv ({len(rows)} rows)")
