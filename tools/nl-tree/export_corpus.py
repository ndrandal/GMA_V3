#!/usr/bin/env python3
"""Export corpus to JSON for C++ tests and WS smoke tests."""
import json
import sys
sys.path.insert(0, ".")

from corpus import CORPUS

# Full corpus export
with open("corpus.json", "w") as f:
    json.dump(CORPUS, f, indent=2)
print(f"Exported {len(CORPUS)} entries to corpus.json")

# Extract just the individual request objects for TreeBuilder tests
# (TreeBuilder sees individual requests, not the outer subscribe wrapper)
requests = []
for entry in CORPUS:
    for req in entry["json"]["requests"]:
        requests.append({
            "corpus_id": entry["id"],
            "nl": entry["nl"],
            "request": req
        })

with open("corpus_requests.json", "w") as f:
    json.dump(requests, f, indent=2)
print(f"Exported {len(requests)} individual requests to corpus_requests.json")

# Export just the subscribe messages for WS smoke testing
messages = []
for entry in CORPUS:
    messages.append({
        "id": entry["id"],
        "nl": entry["nl"],
        "message": entry["json"]
    })

with open("corpus_ws_messages.json", "w") as f:
    json.dump(messages, f, indent=2)
print(f"Exported {len(messages)} WS messages to corpus_ws_messages.json")
