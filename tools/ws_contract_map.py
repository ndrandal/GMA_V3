#!/usr/bin/env python3
import os, re, sys

ROOT = sys.argv[1] if len(sys.argv)>1 else "."
# rough heuristics
ONMSG = re.compile(r'\bon(Message|Text|Read)\b|\bhandle_message\b', re.IGNORECASE)
SEND  = re.compile(r'\bsend\(|_send\(|sendText\(', re.IGNORECASE)
TREE  = re.compile(r'createTree|buildTree|TreeBuilder', re.IGNORECASE)

sections={"on_message":[],"send":[],"tree_build":[]}
for dp,_,fns in os.walk(ROOT):
    for fn in fns:
        if not fn.endswith(('.hpp','.cpp','.cc','.cxx')): continue
        path=os.path.join(dp,fn)
        try:
            with open(path,'r',encoding='utf-8',errors='ignore') as f:
                for i,l in enumerate(f,1):
                    if ONMSG.search(l): sections["on_message"].append((path,i,l.strip()))
                    if SEND.search(l):  sections["send"].append((path,i,l.strip()))
                    if TREE.search(l):  sections["tree_build"].append((path,i,l.strip()))
        except: pass

os.makedirs("artifacts", exist_ok=True)
with open("artifacts/T1_ws_contract.md","w") as f:
    f.write("# WebSocket Request/Response & Tree Building\n\n")
    f.write("## Message handlers (incoming)\n")
    for p,i,l in sections["on_message"]: f.write(f"- {p}:{i} :: {l}\n")
    f.write("\n## Send paths (outgoing)\n")
    for p,i,l in sections["send"]: f.write(f"- {p}:{i} :: {l}\n")
    f.write("\n## Tree builder touchpoints\n")
    for p,i,l in sections["tree_build"]: f.write(f"- {p}:{i} :: {l}\n")

print("Wrote artifacts/T1_ws_contract.md")
