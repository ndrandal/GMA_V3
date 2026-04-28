#!/usr/bin/env python3
"""
NL → GMA TreeBuilder JSON compiler.

Translates natural language descriptions of market analysis strategies
into valid GMA subscribe manifests.

Usage:
    # Validate a JSON file
    python nl_tree.py --validate manifest.json

    # Validate JSON from stdin
    echo '{"type":"subscribe",...}' | python nl_tree.py --check

    # API-powered inference (requires ANTHROPIC_API_KEY)
    python nl_tree.py --api "Stream AAPL's 20-period SMA"

    # API interactive REPL
    python nl_tree.py --api
"""

import argparse
import json
import os
import sys
from typing import Any

from schema import SYSTEM_PROMPT
from examples import EXAMPLES

# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

VALID_NODE_TYPES = {"Listener", "Worker", "Aggregate", "AtomicAccessor",
                    "Interval", "SymbolSplit", "Chain"}

VALID_WORKER_FNS = {"mean", "avg", "sum", "max", "min", "spread",
                    "last", "first", "diff", "scale", "count", "stddev"}

VALID_RAW_FIELDS = {"lastPrice", "volume", "bid", "ask",
                    "openPrice", "highPrice", "lowPrice", "prevClose"}

VALID_ATOMIC_FIELDS = {
    "sma_5", "sma_10", "sma_20", "sma_50",
    "ema_9", "ema_21", "ema_50",
    "rsi_14",
    "macd_line", "macd_signal", "macd_histogram",
    "bollinger_upper", "bollinger_lower",
    "momentum_10", "roc_10",
    "atr_14",
    "volume_avg_20",
    "vwap", "obv", "volatility_rank", "mean", "median",
}

MAX_DEPTH = 32


class ValidationError(Exception):
    pass


def validate_node(node: dict, depth: int = 0, path: str = "root") -> list[str]:
    """Recursively validate a node spec. Returns list of error strings."""
    errors: list[str] = []

    if depth > MAX_DEPTH:
        errors.append(f"{path}: nesting depth exceeds {MAX_DEPTH}")
        return errors

    if not isinstance(node, dict):
        errors.append(f"{path}: node must be an object, got {type(node).__name__}")
        return errors

    ntype = node.get("type")
    if ntype not in VALID_NODE_TYPES:
        errors.append(f"{path}: unknown node type '{ntype}' "
                      f"(valid: {', '.join(sorted(VALID_NODE_TYPES))})")
        return errors

    if ntype == "Worker":
        fn = node.get("fn")
        if fn not in VALID_WORKER_FNS:
            errors.append(f"{path}: unknown Worker fn '{fn}' "
                          f"(valid: {', '.join(sorted(VALID_WORKER_FNS))})")
        if fn == "scale" and "factor" not in node:
            errors.append(f"{path}: Worker(scale) requires 'factor'")
        if fn == "scale" and "factor" in node:
            if not isinstance(node["factor"], (int, float)):
                errors.append(f"{path}: 'factor' must be a number")

    elif ntype == "Aggregate":
        arity = node.get("arity")
        inputs = node.get("inputs", [])
        if not isinstance(inputs, list) or len(inputs) == 0:
            errors.append(f"{path}: Aggregate requires non-empty 'inputs' array")
        elif arity != len(inputs):
            errors.append(f"{path}: Aggregate arity ({arity}) != inputs length ({len(inputs)})")
        for i, inp in enumerate(inputs):
            errors.extend(validate_node(inp, depth + 1, f"{path}.inputs[{i}]"))

    elif ntype == "AtomicAccessor":
        field = node.get("field")
        if field and field not in VALID_ATOMIC_FIELDS:
            errors.append(f"{path}: unknown atomic field '{field}' "
                          f"(valid: {', '.join(sorted(VALID_ATOMIC_FIELDS))})")
        if not node.get("symbol"):
            errors.append(f"{path}: AtomicAccessor requires 'symbol'")

    elif ntype == "Interval":
        ms = node.get("ms") or node.get("periodMs")
        if ms is None:
            errors.append(f"{path}: Interval requires 'ms'")
        elif not isinstance(ms, (int, float)) or ms < 1 or ms > 3_600_000:
            errors.append(f"{path}: Interval 'ms' must be 1..3600000")
        child = node.get("child")
        if child:
            errors.extend(validate_node(child, depth + 1, f"{path}.child"))

    elif ntype == "SymbolSplit":
        child = node.get("child")
        if not child:
            errors.append(f"{path}: SymbolSplit requires 'child'")
        else:
            errors.extend(validate_node(child, depth + 1, f"{path}.child"))

    elif ntype == "Chain":
        stages = node.get("stages", [])
        if not isinstance(stages, list) or len(stages) == 0:
            errors.append(f"{path}: Chain requires non-empty 'stages' array")
        for i, stage in enumerate(stages):
            errors.extend(validate_node(stage, depth + 1, f"{path}.stages[{i}]"))

    elif ntype == "Listener":
        if not node.get("symbol"):
            errors.append(f"{path}: Listener requires 'symbol'")
        field = node.get("field")
        if field and field not in VALID_RAW_FIELDS:
            errors.append(f"{path}: Listener field '{field}' is not a valid raw field")

    return errors


def validate_manifest(manifest: dict) -> list[str]:
    """Validate a full subscribe manifest. Returns list of error strings."""
    errors: list[str] = []

    if not isinstance(manifest, dict):
        errors.append("Manifest must be a JSON object")
        return errors

    if manifest.get("type") != "subscribe":
        errors.append(f"Expected type='subscribe', got '{manifest.get('type')}'")

    requests = manifest.get("requests")
    if not isinstance(requests, list) or len(requests) == 0:
        errors.append("'requests' must be a non-empty array")
        return errors

    seen_keys: set[int] = set()

    for i, req in enumerate(requests):
        rpath = f"requests[{i}]"

        # key
        key = req.get("key")
        if not isinstance(key, int):
            errors.append(f"{rpath}: 'key' must be an integer")
        elif key in seen_keys:
            errors.append(f"{rpath}: duplicate key {key}")
        else:
            seen_keys.add(key)

        # symbol
        if not req.get("symbol"):
            errors.append(f"{rpath}: 'symbol' is required")

        # field
        field = req.get("field")
        if not field:
            errors.append(f"{rpath}: 'field' is required")
        elif field not in VALID_RAW_FIELDS:
            errors.append(f"{rpath}: field '{field}' is not a valid raw tick field "
                          f"(valid: {', '.join(sorted(VALID_RAW_FIELDS))})")

        # node + pipeline mutual exclusion
        has_node = "node" in req
        has_pipeline = "pipeline" in req

        if has_node:
            errors.extend(validate_node(req["node"], 0, f"{rpath}.node"))

        if has_pipeline:
            pipeline = req["pipeline"]
            if not isinstance(pipeline, list):
                errors.append(f"{rpath}: 'pipeline' must be an array")
            else:
                for j, stage in enumerate(pipeline):
                    errors.extend(validate_node(stage, 0, f"{rpath}.pipeline[{j}]"))

    return errors


# ---------------------------------------------------------------------------
# API Inference (optional, requires ANTHROPIC_API_KEY)
# ---------------------------------------------------------------------------

def build_few_shot_messages() -> list[dict]:
    """Build alternating user/assistant messages from examples."""
    messages = []
    for ex in EXAMPLES:
        messages.append({"role": "user", "content": ex["nl"]})
        messages.append({"role": "assistant", "content": json.dumps(ex["json"], indent=2)})
    return messages


def query_claude(nl_input: str, model: str = "claude-sonnet-4-20250514",
                 verbose: bool = False) -> dict:
    """Send NL to Claude API, parse and validate the JSON response."""
    import anthropic

    client = anthropic.Anthropic()
    messages = build_few_shot_messages()
    messages.append({"role": "user", "content": nl_input})

    if verbose:
        print(f"[nl-tree] Sending to {model}...", file=sys.stderr)

    response = client.messages.create(
        model=model,
        max_tokens=4096,
        system=SYSTEM_PROMPT,
        messages=messages,
    )

    raw = response.content[0].text.strip()
    manifest = _parse_json_response(raw)

    errors = validate_manifest(manifest)
    if errors:
        error_str = "\n".join(f"  - {e}" for e in errors)
        if verbose:
            print(f"[nl-tree] Validation errors, requesting fix...", file=sys.stderr)
        messages.append({"role": "assistant", "content": raw})
        messages.append({
            "role": "user",
            "content": (
                f"That JSON has validation errors:\n{error_str}\n\n"
                "Please fix and return only the corrected JSON."
            )
        })
        response = client.messages.create(
            model=model,
            max_tokens=4096,
            system=SYSTEM_PROMPT,
            messages=messages,
        )
        raw2 = response.content[0].text.strip()
        manifest = _parse_json_response(raw2)
        errors2 = validate_manifest(manifest)
        if errors2:
            error_str2 = "\n".join(f"  - {e}" for e in errors2)
            raise ValidationError(
                f"Manifest still invalid after retry:\n{error_str2}\n\n"
                f"JSON:\n{json.dumps(manifest, indent=2)}")

    return manifest


def _parse_json_response(raw: str) -> dict:
    """Parse JSON from a response, stripping markdown fences if present."""
    text = raw.strip()
    if text.startswith("```"):
        lines = text.split("\n")
        if lines[0].startswith("```"):
            lines = lines[1:]
        if lines and lines[-1].strip() == "```":
            lines = lines[:-1]
        text = "\n".join(lines)
    try:
        return json.loads(text)
    except json.JSONDecodeError as e:
        raise ValidationError(f"Invalid JSON: {e}\n\nRaw output:\n{text}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="GMA TreeBuilder JSON validator and NL compiler")
    parser.add_argument("--validate", metavar="FILE",
                        help="Validate a JSON manifest file")
    parser.add_argument("--check", action="store_true",
                        help="Read JSON from stdin and validate it")
    parser.add_argument("--api", nargs="*", default=None,
                        help="Use Claude API for NL→JSON (optional query, or REPL if empty)")
    parser.add_argument("--model", default="claude-sonnet-4-20250514",
                        help="Claude model ID for --api mode")
    parser.add_argument("--verbose", "-v", action="store_true")
    parser.add_argument("--compact", action="store_true",
                        help="Compact JSON output")

    args = parser.parse_args()
    indent = None if args.compact else 2

    # --validate FILE
    if args.validate:
        with open(args.validate) as f:
            manifest = json.load(f)
        errors = validate_manifest(manifest)
        if errors:
            print("FAIL", file=sys.stderr)
            for e in errors:
                print(f"  - {e}", file=sys.stderr)
            sys.exit(1)
        else:
            print("OK", file=sys.stderr)
            print(json.dumps(manifest, indent=indent))
            sys.exit(0)

    # --check (stdin JSON)
    if args.check:
        raw = sys.stdin.read().strip()
        try:
            manifest = json.loads(raw)
        except json.JSONDecodeError as e:
            print(f"FAIL: invalid JSON: {e}", file=sys.stderr)
            sys.exit(1)
        errors = validate_manifest(manifest)
        if errors:
            print("FAIL", file=sys.stderr)
            for e in errors:
                print(f"  - {e}", file=sys.stderr)
            sys.exit(1)
        else:
            print("OK", file=sys.stderr)
            print(json.dumps(manifest, indent=indent))
            sys.exit(0)

    # --api [query]
    if args.api is not None:
        if not os.environ.get("ANTHROPIC_API_KEY"):
            print("Error: ANTHROPIC_API_KEY not set", file=sys.stderr)
            sys.exit(1)

        if args.api:
            nl = " ".join(args.api)
            manifest = query_claude(nl, model=args.model, verbose=args.verbose)
            print(json.dumps(manifest, indent=indent))
            return

        # API REPL
        print("GMA NL→Tree Compiler [API mode] (type 'quit' to exit)")
        print("-" * 50)
        while True:
            try:
                nl = input("\n> ").strip()
            except (EOFError, KeyboardInterrupt):
                print("\nBye.")
                break
            if not nl or nl.lower() in ("quit", "exit", "q"):
                break
            try:
                manifest = query_claude(nl, model=args.model, verbose=args.verbose)
                print(json.dumps(manifest, indent=indent))
            except ValidationError as e:
                print(f"ERROR: {e}", file=sys.stderr)
        return

    # Default: print usage
    parser.print_help()


if __name__ == "__main__":
    main()
