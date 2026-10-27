#!/usr/bin/env bash
# heartbeat-sched-probe.sh — record-only hosted macOS probe (nostrc-0cfsg.11.1).
#
# Runs the SAME test binary with the SAME command line CTest registers
# (test_nostr_gobject_main_thread_latency, no arguments) under explicit
# child-local process policies and prints what each run observed:
#
#   inherit          inherited policy, explicit glib-default wait (the control)
#   poll             inherited policy, explicit poll(2) wait (the Darwin default)
#                    instead of GLib's Darwin select()-based g_poll().
#                    select() timeouts get latency-QoS leeway (up to ~40 ms for
#                    10 ms under an inherited UTILITY clamp); poll(2) does not.
#                    inherit vs poll separates OS timer leeway from the
#                    remaining wake latency on the actual host.
#   inherit-control  inherit / poll with the opt-in no-work control sleeper
#   poll-control     thread (NOSTRC_HEARTBEAT_CONTROL=1). ONLY these runs set it.
#   fg               child-local setpriority(PRIO_DARWIN_PROCESS,0,0); kept once
#                    for continuity (hosted runs 39922245644/39922272026 showed
#                    darwin_bg_process=0 and fg failing like inherit)
#   bg-positive      positive control: PRIO_DARWIN_BG on this process only
#   diag-case        the opt-in metric-validity case
#                    (NOSTRC_HEARTBEAT_DIAGNOSTICS=1, run with -p; never in CTest)
#
# Diagnostic variables are set EXPLICITLY on every run, including the wait
# backend, so changing the harness default cannot relabel a comparison.
# Normal CTest sets none of them and keeps the original 8 cases and original
# single-thread workload, using poll(2) on Darwin (nostrc-8kzuf).
#
# Runs are sequential and quiet. The probe deliberately does NOT try to
# reproduce `ctest -j2` contention (no background load generators, no PID
# bookkeeping): the real concurrent state is observed by the gating test's own
# `SchedEnv:` lines in the actual `Test` step, and this probe answers only the
# policy question (does the inherited process policy matter?).
#
# It never changes a test, threshold, sleep, retry or exit status of the real
# `Test` step; it ALWAYS exits 0 and every failure is printed, not hidden.
# A failing `inherit` run is not retried into a pass: every run is reported.
#
# Usage: heartbeat-sched-probe.sh <path/to/test_nostr_gobject_main_thread_latency> [repetitions]
set -u

bin=${1:?usage: $0 <test binary> [repetitions]}
reps=${2:-3}
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
echo "-- timer coalescing parameters (read-only sysctl):"
sysctl kern.timer.coalescing_enabled 2>&1
sysctl -a 2>/dev/null | grep -E '^kern\.timer_coalesce_(tier[0-9]|bg)_(scale|ns_max):' || echo "(timer_coalesce sysctls unavailable)"
echo "-- top CPU consumers before runs (pid pri nice %cpu stat comm):"
ps -A -o pid=,pri=,nice=,%cpu=,stat=,comm= -r 2>&1 | head -12
echo "-- control timer microprobe, inherited policy:"
"$launch" --policy inherit --timer

export G_DEBUG=fatal-warnings,gc-friendly G_SLICE=always-malloc
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:strict_string_checks=1}"

declare -a rows=()
declare -a rcs=()
# run_one label policy index control(0|1) pollfunc(glib-default|poll) [test args...]
run_one() {
    local label=$1 policy=$2 idx=$3 control=$4 pollfunc=$5
    shift 5
    local log="$work/$label-$idx.log" rc diag=unset
    # BSD env(1) stops option parsing at the first NAME=value, so every -u
    # must precede every assignment.
    local -a unset_env=(-u NOSTRC_HEARTBEAT_DIAGNOSTICS) set_env=(NOSTRC_HEARTBEAT_CONTROL="$control")
    if [[ $label == diag-case ]]; then
        diag=1
        set_env+=(NOSTRC_HEARTBEAT_DIAGNOSTICS=1)
    fi
    set_env+=(NOSTRC_HEARTBEAT_POLL_FUNC="$pollfunc")
    {
        echo "probe-env: label=$label policy=$policy NOSTRC_HEARTBEAT_CONTROL=$control NOSTRC_HEARTBEAT_DIAGNOSTICS=$diag NOSTRC_HEARTBEAT_POLL_FUNC=$pollfunc args=${*:-<none>}"
        env "${unset_env[@]}" "${set_env[@]}" "$launch" --policy "$policy" -- "$bin" "$@"
    } >"$log" 2>&1
    rc=$?
    echo "== $label run $idx policy=$policy control=$control pollfunc=$pollfunc rc=$rc"
    grep -E 'probe-env:|sched-probe: (before|after|setpriority|requested)|^1\.\.|Heartbeat:|Attribution:|SchedEnv:|^not ok|Bail out' "$log"
    local before after setrc bgflag hb cases pf pri
    before=$(grep -m1 'sched-probe: before' "$log" | grep -o 'darwin_bg_process=[-0-9]*')
    after=$(grep 'sched-probe: after' "$log" | grep -o 'darwin_bg_process=[-0-9]*')
    setrc=$(grep -m1 -o 'setpriority([^)]*) rc=[-0-9]*' "$log" | sed 's/.*) //')
    bgflag=$(grep -m1 -o 'SchedEnv: darwin_bg_process=[0-9-]*' "$log" | sed 's/SchedEnv: /test_/')
    hb=$(grep -m1 -o 'Heartbeat: .*cycles=[0-9]*' "$log")
    cases=$(grep -m1 -o '^1\.\.[0-9]*' "$log" | sed 's/^1\.\.//')
    pf=$(grep -m1 -o 'SchedEnv: poll_func=[^ ]*' "$log" | sed 's/SchedEnv: //')
    pri=$(grep -m1 -o 'main_thread_sched_pri cur=[-0-9]* base=[-0-9]*' "$log" | sed 's/main_thread_sched_pri /pri_/')
    rcs+=("$label-$idx=$rc")
    rows+=("$label|$idx|rc=$rc|cases=${cases:-none}|before:${before:-none}|after:${after:-none (policy unchanged)}|setpriority:${setrc:-none}|${bgflag:-test_darwin_bg_process=none}|${pf:-poll_func=none}|${pri:-pri_none}|${hb:-Heartbeat: none (test aborted or not a heartbeat run)}")
    if [[ $rc -ne 0 && $label == bg-positive ]]; then
        echo "-- bg-positive is expected to fail the guard (positive control); not a CI finding"
    elif [[ $rc -ne 0 ]]; then
        echo "::warning::heartbeat sched probe: $label run $idx exited $rc (retained, not retried)"
        echo "-- full log of failed run $label-$idx:"
        cat "$log"
    fi
}

# inherit and poll alternate within each repetition so both see the same
# host state; every run is reported, none is retried.
for ((i = 1; i <= reps; i++)); do
    run_one inherit inherit "$i" 0 glib-default
    run_one poll inherit "$i" 0 poll
    run_one inherit-control inherit "$i" 1 glib-default
    run_one poll-control inherit "$i" 1 poll
done
run_one fg fg 1 0 glib-default
run_one bg-positive bg 1 0 glib-default
run_one diag-case inherit 1 0 glib-default -p /nostr-gobject/latency-diagnostic/sched-env-thread-cpu

echo "-- top CPU consumers after runs (pid pri nice %cpu stat comm):"
ps -A -o pid=,pri=,nice=,%cpu=,stat=,comm= -r 2>&1 | head -12
echo "== summary (label|run|rc|TAP cases|policy before|after|setpriority|in-test bg flag|poll func|main-thread sched pri|heartbeat)"
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
