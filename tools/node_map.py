#!/usr/bin/env python3
import os, re, json, sys
ROOT = sys.argv[1] if len(sys.argv)>1 else "."

# crude patterns: adjust if your base class name differs
DERIVES = re.compile(r'class\s+([A-Za-z0-9_]+)\s*:\s*public\s+I(Node|.*Node)')
FACTORY  = re.compile(r'register(Node|Factory)\s*\(\s*"([^"]+)"\s*,\s*&?([A-Za-z0-9_:\.]+)')

nodes = {}
factories = []

for dp,_,fns in os.walk(ROOT):
    for fn in fns:
        if not fn.endswith(('.hpp','.cpp','.cc','.cxx')): continue
        path=os.path.join(dp,fn)
        try:
            with open(path,'r',encoding='utf-8',errors='ignore') as f:
                src=f.read()
            for m in DERIVES.finditer(src):
                cls=m.group(1)
                nodes.setdefault(cls,[]).append(path)
            for m in FACTORY.finditer(src):
                factories.append({"path":path,"name":m.group(2),"impl":m.group(3)})
        except: pass

os.makedirs("artifacts", exist_ok=True)
with open("artifacts/T1_nodes_raw.json","w") as f:
    json.dump({"nodes":nodes,"factories":factories}, f, indent=2)

# Markdown summary
with open("artifacts/T1_nodes_map.md","w") as f:
    f.write("# Node Types & Factories\n\n")
    f.write("## Classes deriving from INode\n\n")
    for cls, paths in sorted(nodes.items()):
        f.write(f"- `{cls}`\n")
        for p in paths: f.write(f"  - {p}\n")
    f.write("\n## Factory registrations\n\n")
    for fac in factories:
        f.write(f"- `{fac['name']}` -> `{fac['impl']}` @ {fac['path']}\n")

print("Wrote artifacts/T1_nodes_map.md and T1_nodes_raw.json")
