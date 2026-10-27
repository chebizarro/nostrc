#!/usr/bin/env python3
"""Validate a CI selection handoff and extract the selected vault plan."""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / ".github/workflows"))
import ci_selection
import test_selection as selection


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--handoff", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sha", required=True)
    args = parser.parse_args()

    saved = selection.read_json(args.handoff)
    policy = selection.load_inventory(ROOT / "tests/selection/inventory.json")
    plan = selection.validate_plan(saved["shared_plan"], policy)
    selection.require(saved["dispatch"] == ci_selection.project(plan),
                      "dispatch differs from shared plan")
    selection.require(plan["context"] == "ci" and not plan["partial_profiles"],
                      "vault CI requires an unrestricted CI plan")
    selection.require(plan["source_identity"]["kind"] == "committed" and
                      plan["tested_sha"] == args.sha and
                      selection.commit_sha(ROOT, "HEAD") == args.sha,
                      "vault CI source identity mismatch")
    selection.require("homed.vault.filesystem" in plan["selected_checks"] and
                      "homed.vault.linux" in plan["expected_lane_ids"],
                      "vault check/profile was not selected")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(plan, indent=2) + "\n")


if __name__ == "__main__":
    try:
        main()
    except (selection.ContractError, KeyError, ValueError, OSError) as exc:
        print(f"vault CI selection refused: {exc}", file=sys.stderr)
        sys.exit(1)
