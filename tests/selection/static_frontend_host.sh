#!/usr/bin/env bash
# Scheduling only (nostrc-0cfsg.36.26): the stable static-checks job's nine
# original host self-tests keep their exact argv, host environment, checkout
# cwd and order, but start as ONE owned process group beside the image build.
#   start          prove the checkout, then launch the group (its own session)
#   wait           join it before the testhost; a cancelled job terminates it
#   replay NAME    each original named step reports exactly one command's
#                  retained log and recorded exit (missing => failure)
# Physical logs, exits and intervals are retained under the testhost artifact
# path. Nothing here is a gate, a timeout, or a synthetic success.
set -euo pipefail
: "${RUNNER_TEMP:?}"
root="$RUNNER_TEMP/namespace-policy/host-frontend"
NAMES=(unsequenced gui-frame linux-packages pre-push full-gate linux-smoke sanitizer signer-names keychain-guard)
argv() {
  case "$1" in
    unsequenced) echo 'python3 scripts/check-unsequenced-args.py --self-test' ;;
    gui-frame) echo 'python3 scripts/check-gui-test-gdk-frame.py --self-test' ;;
    linux-packages) echo 'python3 scripts/check-linux-ci-packages.py --self-test' ;;
    pre-push) echo 'bash scripts/test-pre-push.sh' ;;
    full-gate) echo 'bash scripts/test-full-gate.sh' ;;
    linux-smoke) echo 'bash scripts/test-linux-gate-smoke.sh' ;;
    sanitizer) echo 'bash scripts/test-sanitizer-gate.sh' ;;
    signer-names) echo 'bash scripts/check-grotto-names.sh' ;;
    keychain-guard) echo 'bash scripts/test-check-macos-keychain.sh' ;;
    *) return 1 ;;
  esac
}
# Liveness excludes zombies: an exited process stays visible until a reaper
# collects it, and the reaper (PID 1) is not guaranteed to do so promptly.
alive() {
  local state
  state="$(sed 's/.*) //' "/proc/$1/stat" 2>/dev/null | cut -d ' ' -f 1)"
  [[ -n "$state" && "$state" != Z ]]
}
members() {
  local member
  for member in $(pgrep -g "$1" || true); do alive "$member" && echo "$member"; done
  return 0
}
status_of() { awk -F '\t' -v name="$1" '$1 == name {print $2}' "$root/commands.tsv" 2>/dev/null; }
case "${1:-}" in
  start)
    : "${REQUESTED_SHA:?}"
    test "$(git rev-parse HEAD)" = "$REQUESTED_SHA"
    test ! -e "$root"
    mkdir -p "$root/logs"
    : > "$root/commands.tsv"
    : > "$root/timings.tsv"
    setsid -f bash "$0" group < /dev/null > "$root/group.log" 2>&1
    for _ in $(seq 1 300); do [[ -s "$root/group.pid" ]] && break; sleep 0.1; done
    test -s "$root/group.pid"
    echo "host self-test group $(cat "$root/group.pid") started beside the image build"
    ;;
  group)
    # Session leader: its pid is the process-group id that `wait` owns.
    printf '%s\n' "$$" > "$root/group.pid.partial"
    mv "$root/group.pid.partial" "$root/group.pid"
    set +e
    for name in "${NAMES[@]}"; do
      read -r -a command <<< "$(argv "$name")"
      start="$(date +%s.%N)"
      "${command[@]}" > "$root/logs/$name.log" 2>&1
      status=$?
      printf '%s\t%s\n' "$name" "$status" >> "$root/commands.tsv"
      printf '%s\t%s\t%s\n' "$name" "$start" "$(date +%s.%N)" >> "$root/timings.tsv"
    done
    : > "$root/group.done"
    ;;
  wait)
    if [[ ! -e "$root" ]]; then
      echo 'no host self-test group was started by this job; nothing owned to join'
      exit 0
    fi
    pid="$(cat "$root/group.pid")"
    terminated=false
    if [[ "${JOB_STATUS:-}" == cancelled ]] && alive "$pid"; then
      terminated=true
      kill -TERM -- "-$pid" 2>/dev/null || true
      for _ in $(seq 1 300); do [[ -n "$(members "$pid")" ]] || break; sleep 0.1; done
      kill -KILL -- "-$pid" 2>/dev/null || true
    fi
    while alive "$pid"; do sleep 0.5; done
    # Processes the commands themselves orphaned in this owned group (the
    # full-gate self-test's cancelled mock leaves its `sleep 60`) would
    # otherwise outlive the join; record and end them, never count them.
    for member in $(members "$pid"); do
      printf '%s %s\n' "$member" "$(tr '\0' ' ' < "/proc/$member/cmdline" 2>/dev/null | sed 's/ $//')"
    done > "$root/leftovers.txt"
    if [[ -s "$root/leftovers.txt" ]]; then
      kill -TERM -- "-$pid" 2>/dev/null || true
      for _ in $(seq 1 300); do [[ -n "$(members "$pid")" ]] || break; sleep 0.1; done
      kill -KILL -- "-$pid" 2>/dev/null || true
      sleep 0.2
    fi
    remaining="$(members "$pid" | tr '\n' ' ')"
    done=false; [[ -e "$root/group.done" ]] && done=true
    python3 -I - "$root" "$pid" "$terminated" "$done" "$remaining" <<'PY'
import json, sys
root, pid, terminated, done, remaining = sys.argv[1:]
lines = [l.split('\t') for l in open(root + '/commands.tsv').read().splitlines()]
record = dict(kind='static_host_frontend_group_v1', scope='scheduling_only_not_acceptance', group_pid=int(pid),
              completed=done == 'true', terminated_for_cancellation=terminated == 'true',
              orphans_ended_at_join=open(root + '/leftovers.txt').read().splitlines(),
              surviving_pids=remaining.split(), commands=[dict(name=n, exit=int(s)) for n, s in lines],
              complete=False, complete_gate_eligible=False)
open(root + '/group-exit.json', 'w').write(json.dumps(record, indent=2, sort_keys=True) + '\n')
print(json.dumps(record, sort_keys=True))
PY
    [[ "$done" == true && -z "$remaining" ]]
    ;;
  replay)
    name="${2:?}"
    command="$(argv "$name")"
    test -e "$root/group-exit.json"  # joined before any report
    echo "\$ $command  (executed in the owned host group; retained log follows)"
    cat "$root/logs/$name.log" 2>/dev/null || true
    status="$(status_of "$name")"
    if [[ ! "$status" =~ ^[0-9]+$ ]]; then echo "not executed: $name" >&2; exit 1; fi
    exit "$status"
    ;;
  *)
    echo 'usage: static_frontend_host.sh start|wait|replay NAME' >&2
    exit 2
    ;;
esac
