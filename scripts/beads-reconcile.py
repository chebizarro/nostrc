#!/usr/bin/env python3
"""Report beads/git reconciliation candidates; never change tracker, refs or files.

Usage: python3 scripts/beads-reconcile.py --idle-days 30 [--json]
       python3 scripts/beads-reconcile.py --tracker-repo /path/to/main-checkout

Git defaults to this script's checkout; bd defaults to that same checkout. The
tracker cwd and resolved local master snapshot are always reported. No fetch is
performed. bd is invoked with --readonly --sandbox to prohibit writes/auto-sync.
Findings are review hints, NOT closure recommendations or proof of delivery.
Only active (non-closed) beads are checked. Childless epics are identifiable;
ordinary childless tasks cannot be inferred to be parents. Duplicate checking
uses recorded duplicate/duplicates dependencies, not title similarity.

Standalone 7-40 hex tokens are checked as Git commits. 0x literals, longer event
IDs and identifier fragments are excluded. Missing objects are reported only
with explicit commit/SHA/tip context, as unverified citations (not proof that a
commit existed or was never pushed). Bare unresolved hex is counted but ignored.
Exit 0 means the report completed, even with findings; errors exit 2 on stderr.
"""

import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys


HEX = re.compile(r"(?<![\w/])([0-9a-fA-F]{7,40})(?![\w/])")
COMMIT_CONTEXT = re.compile(
    r"(?:commits?|committed(?:\s+as)?|sha(?:-?1)?|tip|cherry-picked)\s*[:=]?\s*[`\"']?$",
    re.IGNORECASE,
)
BRANCH = re.compile(
    r"\b(?:branch\s+|(?:await(?:ing)?|pending)\s+merge\s+(?:of\s+)?(?:branch\s+)?)"
    r"[`'\"]?([A-Za-z0-9_][A-Za-z0-9_./-]*)", re.IGNORECASE,
)


def command(args, cwd, *, allowed=(0,)):
    result = subprocess.run(
        args, cwd=cwd, text=True, capture_output=True,
        env={**os.environ, "GIT_OPTIONAL_LOCKS": "0", "GIT_NO_LAZY_FETCH": "1"},
    )
    if result.returncode not in allowed:
        raise RuntimeError(f"{' '.join(args)} (cwd {cwd}) exited {result.returncode}: "
                           f"{result.stderr.strip() or result.stdout.strip()}")
    return result


class Git:
    def __init__(self, repo):
        self.repo = repo
        self.master = self.run("rev-parse", "--verify", "refs/heads/master^{commit}").stdout.strip()
        self.master_commits = set(self.run("rev-list", self.master).stdout.splitlines())
        self.ref_commits = set(self.run("rev-list", "--all").stdout.splitlines())
        self.merges = defaultdict(list)
        for line in self.run("log", "--merges", "--format=%H%x09%s", self.master).stdout.splitlines():
            sha, subject = line.split("\t", 1)
            match = re.match(r"Merge branch '([^']+)'(?: into .*|$)", subject)
            if match:
                self.merges[match[1]].append(sha)
        self.citations = {}

    def run(self, *args, allowed=(0,)):
        return command(["git", *args], self.repo, allowed=allowed)

    def resolve(self, name):
        result = self.run("rev-parse", "--verify", "--quiet", f"{name}^{{commit}}", allowed=(0, 1))
        return result.stdout.strip() if result.returncode == 0 else None

    def citation(self, token):
        if token not in self.citations:
            sha = self.resolve(token)
            location = ("master" if sha in self.master_commits else
                        "other_refs" if sha in self.ref_commits else "unreachable")
            self.citations[token] = {"commit": sha, "reachability": location,
                                     "verified_commit": sha is not None}
        return self.citations[token]

    def landed_branch(self, name):
        refs = [name] if name.startswith("refs/") else [f"refs/heads/{name}", f"refs/remotes/origin/{name}"]
        for ref in refs:
            sha = self.resolve(ref)
            if sha and sha in self.master_commits:
                return {"evidence": "branch_tip_ancestor", "ref": ref, "commit": sha}
        if name in self.merges:
            return {"evidence": "historical_merge", "commit": self.merges[name][0]}
        return None


def reconcile(issues, git, *, idle_days=30, now=None):
    if not isinstance(issues, list) or any(not isinstance(x, dict) or
            not all(x.get(k) for k in ("id", "status")) for x in issues):
        raise ValueError("bd JSON must be an array of issues with id and status")
    by_id = {x["id"]: x for x in issues}
    if len(by_id) != len(issues):
        raise ValueError("duplicate issue IDs in bd JSON")
    now = now or datetime.now(timezone.utc)
    children = defaultdict(set)
    duplicate_edges = set()
    for issue in issues:
        for dep in issue.get("dependencies") or []:
            source = dep.get("issue_id", issue["id"])
            target = dep.get("depends_on_id")
            if not target:
                continue
            if dep.get("type") == "parent-child":
                children[target].add(source)
            if dep.get("type") in {"duplicate", "duplicates"}:
                duplicate_edges.add((source, target))
        if "." in issue["id"]:
            children[issue["id"].rsplit(".", 1)[0]].add(issue["id"])

    findings, warnings = [], []

    def add(kind, issue, **details):
        findings.append({"check": kind, "id": issue["id"], "title": issue.get("title", ""), **details})

    ignored = 0
    active = sorted((x for x in issues if x["status"] != "closed"), key=lambda x: x["id"])
    for issue in active:
        if issue["status"] == "in_progress":
            try:
                updated = datetime.fromisoformat(issue["updated_at"].replace("Z", "+00:00"))
                if updated.tzinfo is None:
                    raise ValueError("timestamp has no timezone")
                idle = (now - updated).total_seconds() / 86400
                if idle > idle_days:
                    add("stale_claim", issue, idle_days=round(idle, 2), updated_at=issue["updated_at"],
                        assignee=issue.get("assignee"))
            except (KeyError, ValueError, TypeError) as exc:
                warnings.append(f"{issue['id']}: cannot assess claim age: {exc}")
        kids = children[issue["id"]]
        if issue.get("issue_type") == "epic" or kids:
            unknown = sorted(kids - by_id.keys())
            if unknown:
                warnings.append(f"{issue['id']}: missing children: {', '.join(unknown)}")
            elif all(by_id[k]["status"] == "closed" for k in kids):
                add("parent_complete", issue, children=sorted(kids),
                    reason="all_children_closed" if kids else "no_children")
        notes = issue.get("notes") or ""
        for branch in sorted({m[1].rstrip(".") for m in BRANCH.finditer(notes)}):
            evidence = git.landed_branch(branch)
            if evidence:
                add("landed_branch", issue, branch=branch, **evidence)
        for field in ("notes", "description"):
            seen = set()
            for match in HEX.finditer(issue.get(field) or ""):
                token = match[1].lower()
                if token in seen:
                    continue
                citation = git.citation(token)
                context = COMMIT_CONTEXT.search((issue[field] or "")[:match.start()][-70:])
                if not citation["verified_commit"] and not context:
                    ignored += 1
                    continue
                seen.add(token)
                add("commit_citation", issue, field=field, citation=token, **citation)

    for source, target in sorted(duplicate_edges):
        if source not in by_id or target not in by_id:
            warnings.append(f"duplicate relation {source} -> {target}: missing issue")
        elif by_id[source]["status"] != "closed" and by_id[target]["status"] == "closed":
            add("duplicate_keeper_closed", by_id[source], keeper=target)

    return {
        "total_issues": len(issues), "active_issues": len(active), "idle_threshold_days": idle_days,
        "counts": {key: sum(f["check"] == key for f in findings) for key in
                   ("stale_claim", "parent_complete", "landed_branch", "commit_citation", "duplicate_keeper_closed")},
        "commit_reachability_counts": dict(sorted(Counter(
            f["reachability"] for f in findings if f["check"] == "commit_citation").items())),
        "duplicate_check": (f"checked {len(duplicate_edges)} recorded duplicate relations"
                            if duplicate_edges else
                            "skipped: no duplicate/duplicates dependencies exposed by bd list; "
                            "bd duplicates similarity groups are not recorded keeper relations"),
        "ignored_unresolved_hex_tokens": ignored, "warnings": warnings, "findings": findings,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1], help="git checkout (default: script checkout)")
    parser.add_argument("--tracker-repo", type=Path, help="read-only bd cwd (default: git checkout)")
    parser.add_argument("--idle-days", type=float, default=30, help="idle claim threshold, strictly greater than N days (default: 30)")
    parser.add_argument("--json", action="store_true", help="print the complete report as JSON")
    args = parser.parse_args(argv)
    if not math.isfinite(args.idle_days) or args.idle_days < 0:
        parser.error("--idle-days must be a finite nonnegative number")
    try:
        repo = args.repo.resolve()
        tracker = (args.tracker_repo or repo).resolve()
        git = Git(repo)
        issues = json.loads(command(["bd", "--readonly", "--sandbox", "list", "--all", "-n", "0", "--json"], tracker).stdout)
        report = reconcile(issues, git, idle_days=args.idle_days)
        report.update(git_repo=str(repo), tracker_repo=str(tracker), master=git.master,
                      caveat="Review hints only; local master/refs snapshot, no fetch. Historical merges may "
                             "precede new branch work. Missing commit objects are unverified explicit citations; "
                             "bare unresolved hex, 0x literals and long event IDs are ignored.")
        if args.json:
            print(json.dumps(report, indent=2, sort_keys=True))
        else:
            print(f"Tracker (read-only bd cwd): {tracker}\nGit: {repo}\nMaster: {git.master}")
            print(f"Issues: {report['total_issues']} total, {report['active_issues']} active")
            print(report["caveat"])
            print(f"Duplicate check: {report['duplicate_check']}")
            print(f"Ignored unresolved hex tokens: {report['ignored_unresolved_hex_tokens']}")
            print("Counts: " + json.dumps(report["counts"], sort_keys=True))
            print("Commit reachability: " + json.dumps(report["commit_reachability_counts"], sort_keys=True))
            for warning in report["warnings"]:
                print(f"WARNING: {warning}")
            for finding in report["findings"]:
                details = {k: v for k, v in finding.items() if k not in {"check", "id", "title"}}
                print(f"[{finding['check']}] {finding['id']} {finding['title']}: " + json.dumps(details, sort_keys=True))
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"beads-reconcile: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
