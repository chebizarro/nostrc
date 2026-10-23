#!/usr/bin/env bash
# Unconditional per-job teardown for namespace_apparmor_setup.sh. Removes ONLY
# the exact owned profile, ONLY when no task still runs in it, and proves the
# policy list returns to the pre-setup snapshot. Anything else is retained as
# incomplete and fails; nothing pre-existing is ever removed or changed.
set -euo pipefail
out="$1"
record() { python3 -I -c 'import json, sys; json.dump(dict(kind="namespace_apparmor_teardown_v1", status=sys.argv[2], detail=sys.argv[3]), open(sys.argv[1], "w"), indent=2, sort_keys=True)' "$out/teardown.json" "$1" "$2"; }
# Any unanticipated failure (corrupt journal, failed snapshot/removal) still
# leaves a typed incomplete record; it never becomes success.
on_error() { local code=$?; [[ -f "$out/teardown.json" ]] || record incomplete "teardown failed (exit $code) at line $1"; }
trap 'on_error $LINENO' ERR
if [[ ! -f "$out/setup.json" ]]; then
  mkdir -p "$out"; record not_provisioned "setup journal absent: nothing was rendered or added"; exit 0
fi
read -r name parser state < <(python3 -I -c 'import json, sys; d = json.load(open(sys.argv[1])); print(d["name"], d["parser"], d["state"])' "$out/setup.json")
[[ "$name" =~ ^nostrc-ns-[0-9a-f]{32}$ ]]
policy() { sudo cat /sys/kernel/security/apparmor/profiles | LC_ALL=C sort; }
if ! sha256sum --check --status "$out/apparmor-inputs.sha256"; then
  record incomplete "rendered profile/template/parser bytes changed; refusing removal"; exit 1
fi
policy > "$out/profiles-teardown-before.txt"
if ! grep -q -x -F -- "$name (unconfined)" "$out/profiles-teardown-before.txt"; then
  if [[ "$state" == rendered ]]; then
    record not_loaded "profile was rendered but never added"; exit 0
  fi
  record incomplete "owned profile missing before teardown"; exit 1
fi
# Remove only after every native/container consumer exited. Read-only scan of
# EVERY task (labels are per task, not per process); only tasks that vanish
# mid-scan are ignored, any other error refuses.
if ! users="$(sudo python3 -I - "$name" <<'PY'
import os, sys
label = sys.argv[1] + ' (unconfined)'
for pid in filter(str.isdigit, os.listdir('/proc')):
    try:
        tasks = os.listdir('/proc/' + pid + '/task')
    except (FileNotFoundError, ProcessLookupError):
        continue
    for tid in tasks:
        try:
            with open('/proc/' + pid + '/task/' + tid + '/attr/current') as stream:
                value = stream.read().strip('\0\n ')
        except (FileNotFoundError, ProcessLookupError):
            continue
        if value == label:
            print(pid + '/' + tid)
PY
)"; then
  record incomplete "could not inspect task AppArmor labels; refusing removal"; exit 1
fi
if [[ -n "$users" ]]; then
  printf '%s\n' "$users" > "$out/profile-users.txt"
  record incomplete "tasks still run in the owned profile; refusing removal"; exit 1
fi
sudo "$parser" --remove "$out/profile" 2>&1 | tee "$out/apparmor-remove.log"
policy > "$out/profiles-after.txt"
if ! cmp -s "$out/profiles-before.txt" "$out/profiles-after.txt"; then
  record incomplete "policy list differs from the pre-setup snapshot after removal"; exit 1
fi
record removed "exact owned profile removed; policy list equals pre-setup snapshot"
