#!/usr/bin/env bash
# Provision the path-unattached nostrc-namespace-custody AppArmor profile on an
# ephemeral hosted Linux runner, then prove its effect without root.
# The only privileged action is loading this one checked-in profile; kernel
# settings are recorded before/after and must be unchanged. The unconfined
# probe is the retained negative control showing the host restriction remains.
set -euo pipefail
out="$1"
mkdir -p "$out"
root="$(cd "$(dirname "$0")/../.." && pwd)"
profile_file="$root/tests/selection/apparmor/nostrc-namespace-custody"
settings() {
  for key in apparmor_restrict_unprivileged_userns apparmor_restrict_unprivileged_unconfined \
             unprivileged_userns_clone; do
    printf '%s=%s\n' "$key" "$(cat "/proc/sys/kernel/$key" 2>/dev/null || echo absent)"
  done
}
settings > "$out/apparmor-settings-before.txt"
parser="$(PATH="$PATH:/usr/sbin:/sbin" command -v apparmor_parser)"
sha256sum "$profile_file" "$(readlink -f "$parser")" > "$out/apparmor-inputs.sha256"
sudo "$parser" --replace --warn=all "$profile_file" 2>&1 | tee "$out/apparmor-load.log"
settings > "$out/apparmor-settings-after.txt"
cmp "$out/apparmor-settings-before.txt" "$out/apparmor-settings-after.txt"
set +e
python3 "$root/tests/selection/namespace_host_probe.py" > "$out/probe-unconfined.jsonl" 2>&1
echo "$?" > "$out/probe-unconfined.exit"
python3 "$root/tests/selection/namespace_host_probe.py" --apparmor-profile nostrc-namespace-custody \
  > "$out/probe-profile.jsonl" 2>&1
status=$?
set -e
echo "$status" > "$out/probe-profile.exit"
# The named profile must actually grant userns to an explicitly placed child.
test "$status" -eq 0
grep -q '"stage": "profile-entered".*"label": "nostrc-namespace-custody (unconfined)"' "$out/probe-profile.jsonl"
