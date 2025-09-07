#!/usr/bin/env bash
set -euo pipefail

# Defaults
STATUS="failure"          # which run status to pick by default
MODE="failed-only"        # "failed-only" or "all"
OUTDIR="ci-logs"
RUN_ID=""

usage() {
  cat <<USAGE
Usage: $(basename "$0") [options]

Options:
  --run <RUN_ID>        Use a specific Actions run ID (default: latest run matching --status)
  --status <state>      Run status to select when --run omitted (failure|success|cancelled|in_progress|queued)
                        Default: failure
  --all                 Download full logs for each job (default: failed steps only)
  --failed-only         Download only failed steps (default)
  --outdir <dir>        Output dir (default: ci-logs)
  -h, --help            Show this help

Examples:
  $(basename "$0")                       # grab latest failed run's failed-step logs
  $(basename "$0") --all                 # latest failed run, full logs per job
  $(basename "$0") --status success      # latest successful run, failed-step logs (likely empty)
  $(basename "$0") --run 1234567890      # specific run id
USAGE
}

# Parse args
while [[ $# -gt 0 ]]; do
  case "$1" in
    --run) RUN_ID="${2:-}"; shift 2 ;;
    --status) STATUS="${2:-}"; shift 2 ;;
    --all) MODE="all"; shift ;;
    --failed-only) MODE="failed-only"; shift ;;
    --outdir) OUTDIR="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

# Require gh
if ! command -v gh >/dev/null 2>&1; then
  echo "Error: 'gh' not found. Install GitHub CLI: https://cli.github.com/" >&2
  exit 1
fi

# Ensure authed
if ! gh auth status >/dev/null 2>&1; then
  echo "Error: 'gh' is not authenticated. Run: gh auth login" >&2
  exit 1
fi

# Resolve repo
REPO="$(gh repo view --json nameWithOwner -q .nameWithOwner)"

# Resolve RUN_ID if not provided
if [[ -z "$RUN_ID" ]]; then
  RUN_ID="$(gh run list -s "$STATUS" -L 1 --json databaseId -q '.[0].databaseId' || true)"
  if [[ -z "$RUN_ID" || "$RUN_ID" == "null" ]]; then
    echo "No runs found with status '$STATUS'." >&2
    exit 1
  fi
fi

# Prepare output dir
RUN_DIR="$OUTDIR/run-$RUN_ID"
mkdir -p "$RUN_DIR"

# Summarize jobs via REST (ensures real numeric job IDs)
# --paginate to handle >100 jobs, and select fields we care about
JOBS_JSON="$(gh api "repos/$REPO/actions/runs/$RUN_ID/jobs" --paginate)"

# Write a machine- and human-friendly summary
printf "%s\n" "$JOBS_JSON" > "$RUN_DIR/jobs.json"
echo "Run: $RUN_ID  Repo: $REPO" > "$RUN_DIR/summary.txt"
echo "Jobs (name • conclusion • id):" >> "$RUN_DIR/summary.txt"
echo "$JOBS_JSON" | gh api --template '
{{- range .jobs -}}
{{ .name }} • {{ .conclusion }} • {{ .id }}
{{ end -}}
' 2>/dev/null >> "$RUN_DIR/summary.txt" || true

# Decide which jobs to fetch
if [[ "$MODE" == "failed-only" ]]; then
  # Only failed jobs
  MAPQ='.jobs[] | select(.conclusion=="failure") | [.id, .name] | @tsv'
else
  MAPQ='.jobs[] | [.id, .name] | @tsv'
fi

# Iterate jobs and fetch logs
COUNT=0
echo "$JOBS_JSON" | jq -r "$MAPQ" | while IFS=$'\t' read -r JOB_ID JOB_NAME; do
  [[ -z "$JOB_ID" || "$JOB_ID" == "null" ]] && continue
  SAFE="$(printf "%s" "$JOB_NAME" | tr -cd '[:alnum:]_.-')"
  [[ -z "$SAFE" ]] && SAFE="job-$JOB_ID"
  OUTFILE="$RUN_DIR/${SAFE}.log"
  if [[ "$MODE" == "all" ]]; then
    gh run view --job "$JOB_ID" --log > "$OUTFILE"
  else
    # gh prints only failed steps of that job
    gh run view --job "$JOB_ID" --log-failed > "$OUTFILE"
  fi
  echo "Wrote $OUTFILE"
  COUNT=$((COUNT+1))
done

# If no per-job logs (e.g., matrix failure before steps), fall back to whole run log
if [[ ! -s "$RUN_DIR"/*.log 2>/dev/null ]]; then
  echo "No per-job logs captured; dumping whole-run log (failed steps) as fallback."
  gh run view "$RUN_ID" --log-failed > "$RUN_DIR/run-$RUN_ID.log" || true
fi

# Keep logs out of git but visible to editor
EXCLUDE_FILE=".git/info/exclude"
mkdir -p "$(dirname "$EXCLUDE_FILE")"
if ! grep -q "^$OUTDIR/$" "$EXCLUDE_FILE" 2>/dev/null; then
  printf "\n# local only\n%s/\n" "$OUTDIR" >> "$EXCLUDE_FILE" || true
fi

# Print a ready-to-use Windsurf prompt file
PROMPT_FILE=".windsurf_ci_prompt.txt"
cat > "$PROMPT_FILE" <<EOF
Analyze CI logs in: $RUN_DIR

1) List failing job(s)/step(s) and root cause(s) with file/line references.
2) Propose minimal fixes with diffs to .github/workflows/* and source code.
3) Recommend caching/permissions/matrix tweaks to reduce flakiness.
4) Provide a local reproduce checklist (commands, env vars, tools).
EOF

echo
echo "✅ Logs are in: $RUN_DIR"
echo "🛑 Kept out of git via: $EXCLUDE_FILE"
echo "🧠 Open this in Windsurf and ask it to analyze: $PROMPT_FILE"
