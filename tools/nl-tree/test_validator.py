#!/usr/bin/env python3
"""Smoke test: validate all built-in examples pass validation."""

import sys
import json
sys.path.insert(0, ".")

from examples import EXAMPLES
from nl_tree import validate_manifest

passed = 0
failed = 0

for i, ex in enumerate(EXAMPLES, 1):
    errors = validate_manifest(ex["json"])
    if errors:
        print(f"FAIL Example {i}: {ex['nl']}")
        for e in errors:
            print(f"  - {e}")
        failed += 1
    else:
        passed += 1

print(f"\n{passed}/{passed + failed} examples passed validation")
if failed:
    sys.exit(1)
else:
    print("All OK")
