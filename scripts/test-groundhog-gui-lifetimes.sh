#!/usr/bin/env bash
# Run the teardown-sensitive GUI suites with replayable seeded completion
# scheduling. All fixtures use local relays and private clipboards.
set -euo pipefail
build="${1:-build}"
repeats="${2:-2}"
case "$repeats" in ''|*[!0-9]*) echo 'repeats must be a positive integer' >&2; exit 2 ;; esac
[ "$repeats" -gt 0 ] || { echo 'repeats must be positive' >&2; exit 2; }
export GH_TEST_ASYNC_JITTER_MS="${GH_TEST_ASYNC_JITTER_MS:-50}"
seed_list="${GH_TEST_ASYNC_SEEDS:-${GH_TEST_ASYNC_SEED:-20261004 17 29}}"
read -r -a seeds <<< "$seed_list"
[ "${#seeds[@]}" -gt 0 ] || { echo 'at least one seed is required' >&2; exit 2; }
regex='^(groundhog-mls-ui-gui|groundhog-conversation-menu-gui|groundhog-composer|groundhog-attachment-ui)$'
trace_dir="$(mktemp -d)"
trap 'rm -rf "$trace_dir"' EXIT
for seed in "${seeds[@]}"; do
  case "$seed" in ''|*[!0-9]*) echo "invalid seed: $seed" >&2; exit 2 ;; esac
  export GH_TEST_ASYNC_SEED="$seed"
  export GH_TEST_ASYNC_TRACE="$trace_dir/seed-$seed.trace"
  : > "$GH_TEST_ASYNC_TRACE"
  echo "==> GUI lifetime jitter seed=$seed maximum=${GH_TEST_ASYNC_JITTER_MS}ms trace=$GH_TEST_ASYNC_TRACE"
  cmd=(ctest --test-dir "$build" --no-tests=error --output-on-failure --parallel 4
       --repeat "until-fail:$repeats" -R "$regex")
  if [ "$(uname -s)" = Linux ] && [ -z "${DISPLAY:-}" ]; then
    dbus-run-session -- xvfb-run -a -s '-screen 0 1280x800x24' "${cmd[@]}"
  else
    "${cmd[@]}"
  fi
  sed -n 's/^queued .* pid=\([^ ]*\) id=\([^ ]*\)$/\1:\2/p' "$GH_TEST_ASYNC_TRACE" | sort > "$trace_dir/queued"
  sed -n 's/^delivered .* pid=\([^ ]*\) id=\([^ ]*\)$/\1:\2/p' "$GH_TEST_ASYNC_TRACE" | sort > "$trace_dir/delivered"
  if ! diff -u "$trace_dir/queued" "$trace_dir/delivered"; then
    echo "seed=$seed left injected completions undelivered" >&2
    exit 1
  fi
  for site in conversation-menu clipboard mls-invitee media-attachment group-info-service new-group-service; do
    if ! grep -q "^delivered $site " "$GH_TEST_ASYNC_TRACE"; then
      echo "seed=$seed did not deliver an injected $site completion" >&2
      echo "Recorded sites:" >&2
      sed -n 's/^delivered \([^ ]*\).*/\1/p' "$GH_TEST_ASYNC_TRACE" | sort -u >&2
      exit 1
    fi
  done
  echo "==> Seed $seed: all six injected completion families delivered"
done
