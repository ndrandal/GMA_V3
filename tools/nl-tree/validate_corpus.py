#!/usr/bin/env python3
"""Validate all 200 corpus entries."""
import sys
sys.path.insert(0, ".")

from corpus import CORPUS
from nl_tree import validate_manifest

passed = 0
failed = 0
errors_by_id = {}

for entry in CORPUS:
    eid = entry["id"]
    errors = validate_manifest(entry["json"])
    if errors:
        failed += 1
        errors_by_id[eid] = (entry["nl"], errors)
    else:
        passed += 1

if errors_by_id:
    print(f"FAILED {failed}/{passed + failed}:\n")
    for eid, (nl, errs) in sorted(errors_by_id.items()):
        print(f"  #{eid}: {nl}")
        for e in errs:
            print(f"    - {e}")
        print()

print(f"{passed}/{passed + failed} passed")
if failed:
    sys.exit(1)
else:
    print("All OK")
