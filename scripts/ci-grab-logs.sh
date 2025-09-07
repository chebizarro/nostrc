# scripts/ci-for-sha.sh
#!/usr/bin/env bash
set -euo pipefail

# -------- defaults --------
SHA=""                     # commit to inspect; default: git HEAD
STATUS="any"               # any|queued|in_progress|completed|failure|success|cancelled
MODE="failed-only"         # or "all"
OUTDIR="ci-logs"           # output base
TRIM_LIMIT=$((2*1024*1024))  # >2MB logs get trimmed (head/tail)
HEAD_LINES=300
TAIL_LINES=400

usage() {
  cat <<USAGE
Usage: $(basename "$0") [options]

Options:
  --sha <SHA>             Commit SHA to collect runs for (default: git rev-parse HEAD)
  --status <state>        any|queued|in_progress|completed|failure|success|cancelled (default: any)
  --all                   Download FULL logs per job (default shows failed steps only)
  --failed-only           Only failed steps per job (default)
  --outdir <dir>          Output directory base (default: ci-logs)
  --trim-bytes <N>        Trim logs larger than N bytes (default: 2097152)
  --head <N>              Lines from start when trimming (default: 300)
  --tail <N>              Lines from end when trimming (default: 400)
  -h, --help              Show help

Examples:
  $(basename "$0")                        # all runs for HEAD, failed steps per job
  $(basename "$0") --all                  # all runs for HEAD, full logs per job
  $(basename "$0") --sha abc123 --status failure
USAGE
}

# -------- arg parse --------
while [ $# -gt 0 ]; do
  case "$1" in
    --sha) SHA="${2:-}"; shift 2 ;;
    --status) STATUS="${2:-}"; shift 2 ;;
    --all) MODE="all"; shift ;;
    --failed-only) MODE="failed-only"; shift ;;
    --outdir) OUTDIR="${2:-}"; shift 2 ;;
    --trim-bytes) TRIM_LIMIT="${2:-}"; shift 2 ;;
    --head) HEAD_LINES="${2:-}"; shift 2 ;;
    --tail) TAIL_LINES="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

# -------- prereqs --------
need() { command -v "$1" >/dev/null 2>&1 || { echo "Error: '$1' not found"; exit 1; }; }
need gh
need jq
gh auth status >/dev/null 2>&1 || { echo "Error: run 'gh auth login' first"; exit 1; }

REPO="$(gh repo view --json nameWithOwner -q .nameWithOwner)"
[ -n "${SHA}" ] || SHA="$(git rev-parse HEAD 2>/dev/null || true)"
[ -n "${SHA}" ] || { echo "Error: could not determine SHA (try --sha)"; exit 1; }

echo "Repo: $REPO"
echo "SHA:  $SHA"
echo "Mode: $MODE   Status filter: $STATUS"
echo

BASE_DIR="$OUTDIR/sha-$SHA"
mkdir -p "$BASE_DIR"

# -------- list ALL runs for repo, filter by head_sha (server-side filter isn't universal across gh versions) --------
# We paginate and then jq-select head_sha == SHA; optionally filter by status if not 'any'
ALL_RUNS_JSON="$(gh api "repos/$REPO/actions/runs" --paginate)"
if [ "$STATUS" = "any" ]; then
  RUNS_TSV="$(printf '%s\n' "$ALL_RUNS_JSON" | jq -r --arg SHA "$SHA" \
    '.workflow_runs[] | select(.head_sha==$SHA) | [.id, .name, .status, .conclusion] | @tsv')"
else
  RUNS_TSV="$(printf '%s\n' "$ALL_RUNS_JSON" | jq -r --arg SHA "$SHA" --arg ST "$STATUS" \
    '.workflow_runs[] | select(.head_sha==$SHA)
     | select((.status==$ST) or (.conclusion==$ST))
     | [.id, .name, .status, .conclusion] | @tsv')"
fi

if [ -z "$RUNS_TSV" ]; then
  echo "No runs found for SHA=$SHA (status=$STATUS)."
  exit 1
fi

# Write a summary of runs
RUNS_SUMMARY="$BASE_DIR/runs-summary.txt"
echo "Runs for $SHA:" > "$RUNS_SUMMARY"
printf '%s\n' "$RUNS_TSV" | while IFS=$'\t' read -r RID RNAME RSTATUS RCONC; do
  echo "  - run:$RID  name:$RNAME  status:$RSTATUS  conclusion:${RCONC:-none}" >> "$RUNS_SUMMARY"
done
echo "Wrote $RUNS_SUMMARY"
echo

# -------- for each run, fetch per-job logs --------
COMBINED_PACK="$BASE_DIR/_pack"
mkdir -p "$COMBINED_PACK/snippets"

printf '%s\n' "$RUNS_TSV" | while IFS=$'\t' read -r RUN_ID RUN_NAME RUN_STATUS RUN_CONC; do
  RUN_DIR="$BASE_DIR/run-$RUN_ID"
  PACK_DIR="$RUN_DIR/_pack"
  mkdir -p "$RUN_DIR" "$PACK_DIR/snippets"

  echo "== Run $RUN_ID :: $RUN_NAME =="
  JOBS_JSON="$(gh api "repos/$REPO/actions/runs/$RUN_ID/jobs" --paginate)"
  printf '%s\n' "$JOBS_JSON" > "$RUN_DIR/jobs.json"

  { echo "Run: $RUN_ID  Repo: $REPO  Name: $RUN_NAME"
    jq -r '.jobs[] | "\(.name) • \(.conclusion) • \(.id)"' "$RUN_DIR/jobs.json"
  } > "$RUN_DIR/summary.txt" || true

  # Which jobs to fetch
  if [ "$MODE" = "failed-only" ]; then
    MAPQ='.jobs[] | select(.conclusion=="failure") | [.id, .name] | @tsv'
  else
    MAPQ='.jobs[] | [.id, .name] | @tsv'
  fi

  # Fetch per-job logs
  GOT_LOGS=0
  printf '%s\n' "$JOBS_JSON" \
  | jq -r "$MAPQ" \
  | while IFS=$'\t' read -r JOB_ID JOB_NAME; do
      [ -z "$JOB_ID" ] || [ "$JOB_ID" = "null" ] && continue
      SAFE="$(printf "%s" "$JOB_NAME" | tr -cd '[:alnum:]_.-')"
      [ -z "$SAFE" ] && SAFE="job-$JOB_ID"
      OUTFILE="$RUN_DIR/${SAFE}.log"
      if [ "$MODE" = "all" ]; then
        gh run view --job "$JOB_ID" --log > "$OUTFILE"
      else
        gh run view --job "$JOB_ID" --log-failed > "$OUTFILE"
      fi
      echo "  • $OUTFILE"
      GOT_LOGS=1
    done

  # Fallback: run-level log (failed steps)
  if ! find "$RUN_DIR" -maxdepth 1 -type f -name '*.log' | read -r _; then
    echo "  (no per-job logs; dumping run-level failed steps)"
    gh run view "$RUN_ID" --log-failed > "$RUN_DIR/run-$RUN_ID.log" || true
  fi

  # Build per-run pack (copy/trim logs)
  find "$RUN_DIR" -maxdepth 1 -type f -name '*.log' -print0 \
  | while IFS= read -r -d '' f; do
      bn="$(basename "$f")"
      out="$PACK_DIR/$bn"
      sz=$(wc -c < "$f" | tr -d ' ')
      if [ "$sz" -le "$TRIM_LIMIT" ]; then
        cp "$f" "$out"
      else
        {
          echo "===== HEAD: $bn (trimmed; original size ${sz}B) ====="
          head -n "$HEAD_LINES" "$f"
          echo
          echo "===== TAIL: $bn ====="
          tail -n "$TAIL_LINES" "$f"
        } > "$out"
      fi
      # also copy into combined pack (name-spaced with run id)
      cp "$out" "$COMBINED_PACK/${RUN_ID}_$bn"
    done

  # Per-run error summary & manifest
  grep -nEi '(^|[^a-z])(error|failed|fatal|undefined|no such file|permission denied|exit [1-9]|segmentation fault|undefined reference|compile|linker|cannot find|missing)([^a-z]|$)' \
    "$PACK_DIR"/*.log 2>/dev/null \
    | sed 's|'$PACK_DIR'/||' \
    > "$PACK_DIR/ERRORS_SUMMARY.txt" || true

  {
    echo "# CI Log Pack (Run $RUN_ID)"
    echo
    echo "- Source run dir: \`$RUN_DIR\`"
    echo "- Files included:"
    for f in "$PACK_DIR"/*.log; do [ -e "$f" ] || continue; echo "  - $(basename "$f")  ($(wc -l < "$f") lines)"; done
    echo
    echo "## Read order"
    [ -s "$PACK_DIR/ERRORS_SUMMARY.txt" ] && echo "1) ERRORS_SUMMARY.txt"
    idx=2; for f in "$PACK_DIR"/*.log; do [ -e "$f" ] || continue; echo "$idx) $(basename "$f")"; idx=$((idx+1)); done
  } > "$PACK_DIR/MANIFEST.md"

done

# -------- combined pack error summary & manifest --------
grep -nEi '(^|[^a-z])(error|failed|fatal|undefined|no such file|permission denied|exit [1-9]|segmentation fault|undefined reference|compile|linker|cannot find|missing)([^a-z]|$)' \
  "$COMBINED_PACK"/*.log 2>/dev/null \
  | sed 's|'$COMBINED_PACK'/||' \
  > "$COMBINED_PACK/ERRORS_SUMMARY.txt" || true

{
  echo "# CI Log Pack (Combined for SHA $SHA)"
  echo
  echo "- Source base: \`$BASE_DIR\`"
  echo "- Files included:"
  for f in "$COMBINED_PACK"/*.log; do [ -e "$f" ] || continue; echo "  - $(basename "$f")  ($(wc -l < "$f") lines)"; done
  echo
  echo "## Read order"
  [ -s "$COMBINED_PACK/ERRORS_SUMMARY.txt" ] && echo "1) ERRORS_SUMMARY.txt"
  idx=2; for f in "$COMBINED_PACK"/*.log; do [ -e "$f" ] || continue; echo "$idx) $(basename "$f")"; idx=$((idx+1)); done
} > "$COMBINED_PACK/MANIFEST.md"

# -------- keep out of git but visible to Windsurf --------
EXCLUDE_FILE=".git/info/exclude"
mkdir -p "$(dirname "$EXCLUDE_FILE")"
grep -q "^$OUTDIR/$" "$EXCLUDE_FILE" 2>/dev/null || printf "\n# local only\n%s/\n" "$OUTDIR" >> "$EXCLUDE_FILE"

# -------- strong Windsurf prompt --------
cat > .windsurf_ci_prompt.txt <<'EOF'
You have access to the workspace files. **Read these before answering**:

- ci-logs/sha-*/_pack/ERRORS_SUMMARY.txt
- All *.log files under ci-logs/sha-*/_pack/
- ci-logs/sha-*/_pack/MANIFEST.md

### Task
1) For each run in this commit, identify failing job(s) and exact failing step(s). Include:
   - file name and **line numbers**
   - the **exact error snippet** (quoted)
   - a 1–2 sentence **root cause**
2) Propose **minimal, concrete fixes**:
   - If workflow issue: show a unified **diff** for the affected `.github/workflows/*.yml`.
   - If code/toolchain: show diffs to the **actual files** referenced by the error.
   - If caching/permissions/env: specify exact keys/permissions/vars to add.
3) Provide a **local reproduce checklist** (commands, env vars, expected outputs).
4) **Prioritize** fixes that unblock the most jobs first.

### Strict rules
- Add a `(file:line-range)` citation and a short quoted snippet for every claim.
- If a file is huge, **analyze in chunks** and note which chunk.
- If something is missing, say exactly which file/path you need.

Start by summarizing the combined ERRORS_SUMMARY, then deep-dive the top 1–3 failing jobs by severity across runs.
EOF

echo
echo "✅ All runs for SHA stored under: $BASE_DIR"
echo "🧳 Combined pack:                 $COMBINED_PACK"
echo "🧠 Windsurf prompt:               .windsurf_ci_prompt.txt"
echo "🛑 Kept out of git via:           $EXCLUDE_FILE"
echo
echo "Open the prompt in Windsurf:"
echo "  open -a Windsurf .windsurf_ci_prompt.txt   # or: code .windsurf_ci_prompt.txt"
