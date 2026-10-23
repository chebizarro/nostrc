#!/usr/bin/env bash
# heartbeat-sched-probe.sh — record-only hosted macOS probe (nostrc-0cfsg.11.1).
#
# Runs the SAME test binary with the SAME command line CTest registers
# (test_nostr_gobject_main_thread_latency, no arguments) under explicit
# child-local process policies and prints what each run observed:
#
#   inherit          policy exactly as the CI step handed it down (the control)
#   fg               child-local setpriority(PRIO_DARWIN_PROCESS,0,0) —
#                    unprivileged, this process only, no global policy
#   inherit-loaded   inherit / fg with $loaders busy-loop siblings that
#   fg-loaded        approximate (NOT equal) the concurrency of `ctest -j2`
#   inherit-control  inherit / fg with the opt-in no-work control sleeper
#   fg-control       thread (NOSTRC_HEARTBEAT_CONTROL=1). ONLY these runs set it.
#   bg-positive      positive control: PRIO_DARWIN_BG on this process only
#   diag-case        the opt-in metric-validity case
#                    (NOSTRC_HEARTBEAT_DIAGNOSTICS=1, run with -p; never in CTest)
#
# Opt-in variables are set EXPLICITLY on every run (0/unset for the gating-
# equivalent runs), so a run's workload is stated in its own log line
# `probe-env:`. Normal CTest sets neither and keeps the original 8 cases and
# the original single-thread workload.
#
# It never changes a test, threshold, sleep, retry or exit status of the real
# `Test` step; it ALWAYS exits 0 and every failure is printed, not hidden.
# A failing `inherit` run is not retried into a pass: every run is reported.
#
# Usage: heartbeat-sched-probe.sh <path/to/test_nostr_gobject_main_thread_latency> [repetitions]
set -u

bin=${1:?usage: $0 <test binary> [repetitions]}
reps=${2:-3}
loaders=${HEARTBEAT_PROBE_LOADERS:-2}
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
work=$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/hb-sched.XXXXXX")
launch="$work/heartbeat_sched_launch"

if [[ ! -x "$bin" ]]; then
    echo "::warning::heartbeat sched probe: $bin not executable; nothing recorded"
    exit 0
fi
if [[ $(uname -s) != Darwin ]]; then
    echo "heartbeat sched probe: Darwin only; skipped on $(uname -s)"
    exit 0
fi
if ! cc -O1 -o "$launch" "$here/heartbeat_sched_launch.c"; then
    echo "::warning::heartbeat sched probe: launcher failed to compile; nothing recorded"
    exit 0
fi

echo "== host context (record-only)"
sw_vers 2>&1
sysctl -n hw.model hw.ncpu hw.memsize 2>&1 | paste -sd' ' -
uptime
echo "-- process chain of this shell (nice/pri/stat/flags):"
pid=$$
for _ in 1 2 3 4 5 6; do
    ps -o pid=,ppid=,nice=,pri=,stat=,flags=,comm= -p "$pid" 2>&1
    pid=$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ')
    [[ -n "$pid" && "$pid" -gt 1 ]] || break
done
echo "-- launchctl managername: $(launchctl managername 2>&1)"
echo "-- control timer microprobe, inherited policy:"
"$launch" --policy inherit --timer

export G_DEBUG=fatal-warnings,gc-friendly G_SLICE=always-malloc
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:strict_string_checks=1}"

declare -a rows=()
declare -a rcs=()
loader_pids=()
start_loaders() {
    local n
    for ((n = 0; n < loaders; n++)); do
        ( while :; do :; done ) &
        loader_pids+=("$!")
    done
}
stop_loaders() {
    local pid
    for pid in ${loader_pids[@]+"${loader_pids[@]}"}; do
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    done
    loader_pids=()
}
trap stop_loaders EXIT

# run_one label policy index loaded(0|1) control(0|1) [test args...]
run_one() {
    local label=$1 policy=$2 idx=$3 loaded=$4 control=$5
    shift 5
    local log="$work/$label-$idx.log" rc diag=unset
    local -a diag_env=(-u NOSTRC_HEARTBEAT_DIAGNOSTICS)
    if [[ $label == diag-case ]]; then
        diag=1
        diag_env=(NOSTRC_HEARTBEAT_DIAGNOSTICS=1)
    fi
    [[ $loaded == 1 ]] && start_loaders
    {
        echo "probe-env: label=$label policy=$policy loaded=$loaded NOSTRC_HEARTBEAT_CONTROL=$control NOSTRC_HEARTBEAT_DIAGNOSTICS=$diag args=${*:-<none>}"
        env "${diag_env[@]}" NOSTRC_HEARTBEAT_CONTROL="$control" \
            "$launch" --policy "$policy" -- "$bin" "$@"
    } >"$log" 2>&1
    rc=$?
    [[ $loaded == 1 ]] && stop_loaders
    echo "== $label run $idx policy=$policy loaded=$loaded control=$control rc=$rc"
    grep -E 'probe-env:|sched-probe: (before|after|setpriority|requested)|^1\.\.|Heartbeat:|Attribution:|SchedEnv:|^not ok|Bail out' "$log"
    local before after setrc bgflag hb cases
    before=$(grep -m1 'sched-probe: before' "$log" | grep -o 'darwin_bg_process=[-0-9]*')
    after=$(grep 'sched-probe: after' "$log" | grep -o 'darwin_bg_process=[-0-9]*')
    setrc=$(grep -m1 -o 'setpriority([^)]*) rc=[-0-9]*' "$log" | sed 's/.*) //')
    bgflag=$(grep -m1 -o 'SchedEnv: darwin_bg_process=[0-9-]*' "$log" | sed 's/SchedEnv: /test_/')
    hb=$(grep -m1 -o 'Heartbeat: .*cycles=[0-9]*' "$log")
    cases=$(grep -m1 -o '^1\.\.[0-9]*' "$log" | sed 's/^1\.\.//')
    rcs+=("$label-$idx=$rc")
    rows+=("$label|$idx|rc=$rc|cases=${cases:-none}|before:${before:-none}|after:${after:-none (policy unchanged)}|setpriority:${setrc:-none}|${bgflag:-test_darwin_bg_process=none}|${hb:-Heartbeat: none (test aborted or not a heartbeat run)}")
    if [[ $rc -ne 0 && $label == bg-positive ]]; then
        echo "-- bg-positive is expected to fail the guard (positive control); not a CI finding"
    elif [[ $rc -ne 0 ]]; then
        echo "::warning::heartbeat sched probe: $label run $idx exited $rc (retained, not retried)"
        echo "-- full log of failed run $label-$idx:"
        cat "$log"
    fi
}

for ((i = 1; i <= reps; i++)); do
    run_one inherit inherit "$i" 0 0
    run_one fg fg "$i" 0 0
    run_one inherit-loaded inherit "$i" 1 0
    run_one fg-loaded fg "$i" 1 0
    run_one inherit-control inherit "$i" 0 1
    run_one fg-control fg "$i" 0 1
done
run_one bg-positive bg 1 0 0
run_one diag-case inherit 1 0 0 -p /nostr-gobject/latency-diagnostic/sched-env-thread-cpu

echo "== summary (label|run|rc|TAP cases|policy before|after|setpriority|in-test bg flag|heartbeat)"
printf '%s\n' "${rows[@]}"
echo "== all return codes: ${rcs[*]}"
if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    {
        echo "### heartbeat scheduling probe (record-only)"
        echo '```'
        printf '%s\n' "${rows[@]}"
        echo "all return codes: ${rcs[*]}"
        echo '```'
    } >>"$GITHUB_STEP_SUMMARY"
fi
echo "logs kept in $work"
exit 0
