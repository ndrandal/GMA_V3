#!/usr/bin/env python3
"""Scan for INode implementations and their onValue/shutdown methods."""
import os, re, json, sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else "."

DERIVES = re.compile(r'class\s+([A-Za-z0-9_]+)\s*(?:final\s*)?:\s*public\s+(?:(?:gma::)?INode|.*INode)')
ON_VALUE = re.compile(r'void\s+(\w+)::onValue\s*\(')
SHUTDOWN = re.compile(r'void\s+(\w+)::shutdown\s*\(')

nodes = {}
implementations = {"onValue": [], "shutdown": []}

for dp, _, fns in os.walk(ROOT):
    for fn in fns:
        if not fn.endswith(('.hpp', '.cpp', '.cc', '.cxx')):
            continue
        path = os.path.join(dp, fn)
        try:
            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                src = f.read()
            for m in DERIVES.finditer(src):
                cls = m.group(1)
                nodes.setdefault(cls, []).append(path)
            for m in ON_VALUE.finditer(src):
                implementations["onValue"].append({"class": m.group(1), "path": path})
            for m in SHUTDOWN.finditer(src):
                implementations["shutdown"].append({"class": m.group(1), "path": path})
        except Exception:
            pass

os.makedirs("artifacts", exist_ok=True)
with open("artifacts/T1_node_impl_raw.json", "w") as f:
    json.dump({"nodes": nodes, "implementations": implementations}, f, indent=2)

with open("artifacts/T1_node_impl_map.md", "w") as f:
    f.write("# Node Implementations\n\n")
    f.write("## Classes deriving from INode\n\n")
    for cls, paths in sorted(nodes.items()):
        f.write(f"- `{cls}`\n")
        for p in paths:
            f.write(f"  - {p}\n")
    f.write("\n## onValue implementations\n\n")
    for impl in implementations["onValue"]:
        f.write(f"- `{impl['class']}::onValue` @ {impl['path']}\n")
    f.write("\n## shutdown implementations\n\n")
    for impl in implementations["shutdown"]:
        f.write(f"- `{impl['class']}::shutdown` @ {impl['path']}\n")

print("Wrote artifacts/T1_node_impl_map.md and T1_node_impl_raw.json")
