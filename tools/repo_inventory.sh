#!/usr/bin/env bash
set -euo pipefail

echo "# Repo inventory" > artifacts/T1_repo_inventory.md
echo "" >> artifacts/T1_repo_inventory.md

echo "## Top-level files" >> artifacts/T1_repo_inventory.md
ls -la | sed 's/^/    /' >> artifacts/T1_repo_inventory.md

echo "" >> artifacts/T1_repo_inventory.md
echo "## Source tree (src/, include/)" >> artifacts/T1_repo_inventory.md
find src include -type f \( -name "*.hpp" -o -name "*.h" -o -name "*.cpp" -o -name "*.cc" -o -name "*.cxx" \) | sort | sed 's/^/ - /' >> artifacts/T1_repo_inventory.md

echo "" >> artifacts/T1_repo_inventory.md
echo "## Third-party / vendor (if any)" >> artifacts/T1_repo_inventory.md
find third_party vendor -maxdepth 2 -type d 2>/dev/null | sed 's/^/ - /' >> artifacts/T1_repo_inventory.md || true

echo "" >> artifacts/T1_repo_inventory.md
echo "## Binaries (build products)" >> artifacts/T1_repo_inventory.md
find build* -maxdepth 2 -type f -perm -u+x 2>/dev/null | sed 's/^/ - /' >> artifacts/T1_repo_inventory.md || true

echo "Wrote artifacts/T1_repo_inventory.md"
