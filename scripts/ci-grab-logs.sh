# scripts/ci-one.sh
#!/usr/bin/env bash
set -euo pipefail

# -------- defaults --------
STATUS="failure"          # when --run not provided
MODE="failed-only"        # or "all"
OUTDIR="ci-logs"          # where logs go
RUN_ID=""                 # pass with --run to override
TRIM_LIMIT=$((2*1024*1024))  # >2MB logs get head/tail trimmed
HEAD_LINES=300
TAIL_LINES=400

usage() {
  cat <<USAGE
Usage: $(basename "$0") [options]

Options:
  --run <RUN_ID>           Use a specific Actions run ID (default: latest by --status)
  --status <state>         failure|success|cancelled|in_progress|queued (default: failure)
  --all                    Download FULL logs per job (default is failed-only steps)
  --failed-only            Only failed steps per job (default)
  --outdir <dir>           Output directory (default: ci-logs)
  --trim-bytes <N>         Trim logs larger than N bytes using head/tail (default: 2097152)
  --head <N>               Lines from start when trimming (default: 300)
  --tail <N>               Lines from end when trimming (default: 400)
  -h, --help               Show help

Examples:
  $(basename "$0")                          # latest failed run, failed steps per job
  $(basename "$0") --all                    # latest failed run, full logs per job
  $(basename "$0") --run 1234567890         # specific run id
USAGE
}

# -------- arg parse (POSIX style) --------
while [ $# -gt 0 ]; do
  case "$1" in
    --run) RUN_ID="${2:-}"; shift 2 ;;
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

# -------- choose run --------
if [ -z "$RUN_ID" ]; then
  RUN_ID="$(gh run list -s "$STATUS" -L 1 --json databaseId -q '.[0].databaseId' || true)"
  [ -z "$RUN_ID" ] || [ "$RUN_ID" = "null" ] && { echo "No runs found with status '$STATUS'"; exit 1; }
fi

RUN_DIR="$OUTDIR/run-$RUN_ID"
PACK_DIR="$RUN_DIR/_pack"
mkdir -p "$RUN_DIR" "$PACK_DIR/snippets"

echo "Repo: $REPO"
echo "Run:  $RUN_ID"
echo "Out:  $RUN_DIR"
echo

# -------- list jobs for the run (REST: /actions/runs/{run_id}/jobs) --------
JOBS_JSON="$(gh api "repos/$REPO/actions/runs/$RUN_ID/jobs" --paginate)"
printf '%s\n' "$JOBS_JSON" > "$RUN_DIR/jobs.json"

# human summary
{
  echo "Run: $RUN_ID  Repo: $REPO"
  jq -r '.jobs[] | "\(.name) • \(.conclusion) • \(.id)"' "$RUN_DIR/jobs.json"
} > "$RUN_DIR/summary.txt" || true
echo "Wrote $RUN_DIR/summary.txt"

# which jobs to fetch
if [ "$MODE" = "failed-only" ]; then
  MAPQ='.jobs[] | select(.conclusion=="failure") | [.id, .name] | @tsv'
else
  MAPQ='.jobs[] | [.id, .name] | @tsv'
fi

# -------- fetch per-job logs --------
echo "Fetching per-job logs ($MODE)..."
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
  done

# fallback: if no logs captured (matrix died early), dump whole-run failed steps
if ! find "$RUN_DIR" -maxdepth 1 -type f -name '*.log' | read -r _; then
  echo "No per-job logs found; dumping whole-run failed steps as fallback."
  gh run view "$RUN_ID" --log-failed > "$RUN_DIR/run-$RUN_ID.log" || true
fi

# -------- build curated _pack --------
echo
echo "Building curated pack in $PACK_DIR ..."

# copy/trim logs into _pack
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
  done

# error-focused summary
grep -nEi '(^|[^a-z])(error|failed|fatal|undefined|no such file|permission denied|exit [1-9]|segmentation fault|undefined reference|compile|linker|cannot find|missing)([^a-z]|$)' \
  "$PACK_DIR"/*.log 2>/dev/null \
  | sed 's|'$PACK_DIR'/||' \
  > "$PACK_DIR/ERRORS_SUMMARY.txt" || true

# manifest
{
  echo "# CI Log Pack"
  echo
  echo "- Source run dir: \`$RUN_DIR\`"
  echo "- Files included:"
  for f in "$PACK_DIR"/*.log; do
    [ -e "$f" ] || continue
    echo "  - $(basename "$f")  ($(wc -l < "$f") lines)"
  done
  echo
  echo "## Read order"
  if [ -s "$PACK_DIR/ERRORS_SUMMARY.txt" ]; then
    echo "1) ERRORS_SUMMARY.txt"
  fi
  idx=2
  for f in "$PACK_DIR"/*.log; do
    [ -e "$f" ] || continue
    bn="$(basename "$f")"
    echo "$idx) $bn"
    idx=$((idx+1))
  done
} > "$PACK_DIR/MANIFEST.md"

echo "Pack files:"
ls -1 "$PACK_DIR" || true

# -------- keep out of git but visible to Windsurf --------
EXCLUDE_FILE=".git/info/exclude"
mkdir -p "$(dirname "$EXCLUDE_FILE")"
grep -q "^$OUTDIR/$" "$EXCLUDE_FILE" 2>/dev/null || printf "\n# local only\n%s/\n" "$OUTDIR" >> "$EXCLUDE_FILE"

# -------- strong Windsurf prompt --------
cat > .windsurf_ci_prompt.txt <<'EOF'
You have access to the workspace files. **Read these before answering**:

- ci-logs/run-*/_pack/ERRORS_SUMMARY.txt
- All *.log files under ci-logs/run-*/_pack/
- ci-logs/run-*/_pack/MANIFEST.md

### Task
1) Identify failing job(s) and exact failing step(s). For each failure, include:
   - file name and **line numbers**
   - the **exact error snippet** (quote it)
   - a 1–2 sentence **root cause**
2) Propose **minimal, concrete fixes**:
   - If workflow issue: show a unified **diff** for the affected `.github/workflows/*.yml`.
   - If code/toolchain: show diffs to the **actual files** referenced by the error.
   - If caching/permissions/env: specify exact keys/permissions/vars to add.
3) Provide a **local reproduce checklist** with shell commands, env vars, and expected outputs.
4) If multiple failures exist, **prioritize** fixes that unblock the most jobs first.

### Strict rules
- Add a `(file:line-range)` citation and a short quoted snippet for every claim.
- If a file is huge, **analyze in chunks** and note which chunk.
- If something is missing, say exactly which file/path you need.

Start by summarizing ERRORS_SUMMARY.txt, then deep-dive the top 1–3 logs by severity.
EOF

echo
echo "✅ Logs:       $RUN_DIR"
echo "🧳 Curated:    $PACK_DIR"
echo "🧠 Prompt:     .windsurf_ci_prompt.txt"
echo "🛑 Git-ignore: $EXCLUDE_FILE (keeps $OUTDIR/ out of git)"
echo
echo "Open the prompt in Windsurf and hit Ask:"
echo "  open -a Windsurf .windsurf_ci_prompt.txt   # or: code .windsurf_ci_prompt.txt"
