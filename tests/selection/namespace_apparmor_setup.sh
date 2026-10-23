#!/usr/bin/env bash
# Per-job namespace-custody AppArmor provisioning on a disposable GitHub-hosted
# runner ONLY. Renders a fresh, provably absent profile name, adds it (fails
# if it exists; never replaces one), binds rendered/template/parser bytes and
# kernel/policy snapshots, then proves the effect without root. The unconfined
# probe is the retained negative control showing the host restriction remains.
# namespace_apparmor_teardown.sh must run afterwards unconditionally.
set -euo pipefail
out="$1"
mkdir -p "$out"
root="$(cd "$(dirname "$0")/../.." && pwd)"
template="$root/tests/selection/apparmor/nostrc-namespace-custody"
journal() { python3 -I -c 'import json, sys; json.dump(json.loads(sys.argv[2]), open(sys.argv[1], "w"), indent=2, sort_keys=True)' "$out/setup.json" "$1"; }
# Explicit admission: never a developer, shared or self-hosted machine.
if [[ "${GITHUB_ACTIONS:-}" != true || "${RUNNER_ENVIRONMENT:-}" != github-hosted ||
      "${RUNNER_OS:-}" != Linux || -z "${ImageOS:-}" ]]; then
  echo "namespace AppArmor provisioning requires an ephemeral GitHub-hosted Linux runner; refusing" >&2
  exit 2
fi
settings() {
  for key in apparmor_restrict_unprivileged_userns apparmor_restrict_unprivileged_unconfined \
             unprivileged_userns_clone; do
    printf '%s=%s\n' "$key" "$(cat "/proc/sys/kernel/$key" 2>/dev/null || echo absent)"
  done
}
policy() { sudo cat /sys/kernel/security/apparmor/profiles | LC_ALL=C sort; }
name="nostrc-ns-$(python3 -I -c 'import secrets; print(secrets.token_hex(16))')"
[[ "$name" =~ ^nostrc-ns-[0-9a-f]{32}$ ]]
parser="$(readlink -f "$(PATH="$PATH:/usr/sbin:/sbin" command -v apparmor_parser)")"
settings > "$out/apparmor-settings-before.txt"
policy > "$out/profiles-before.txt"
if grep -q -F -- "$name " "$out/profiles-before.txt"; then
  echo "generated AppArmor profile name unexpectedly exists; refusing" >&2
  exit 1
fi
# Policy lines only (comments name the placeholder too); exactly one substitution.
grep -v '^#' "$template" | sed "s/@PROFILE_NAME@/$name/" > "$out/profile"
test "$(grep -c -F -- "$name" "$out/profile")" -eq 1
if grep -q '@' "$out/profile"; then
  echo "unrendered AppArmor template placeholder; refusing" >&2
  exit 1
fi
sha256sum "$template" "$out/profile" "$parser" > "$out/apparmor-inputs.sha256"
journal "$(python3 -I -c 'import json, os, sys; print(json.dumps(dict(kind="namespace_apparmor_setup_v1", state="rendered", name=sys.argv[1], parser=sys.argv[2], admission={k: os.environ.get(k) for k in ("GITHUB_ACTIONS", "RUNNER_ENVIRONMENT", "RUNNER_OS", "ImageOS", "ImageVersion", "GITHUB_RUN_ID", "GITHUB_JOB")})))' "$name" "$parser")"
# The only mutation: add this one fresh profile.
sudo "$parser" --add --warn=all "$out/profile" 2>&1 | tee "$out/apparmor-load.log"
policy > "$out/profiles-loaded.txt"
settings > "$out/apparmor-settings-after.txt"
cmp "$out/apparmor-settings-before.txt" "$out/apparmor-settings-after.txt"
# Exactly one added policy entry, and it is ours; nothing else changed.
LC_ALL=C comm -13 "$out/profiles-before.txt" "$out/profiles-loaded.txt" > "$out/profiles-added.txt"
LC_ALL=C comm -23 "$out/profiles-before.txt" "$out/profiles-loaded.txt" > "$out/profiles-removed.txt"
test "$(cat "$out/profiles-added.txt")" = "$name (unconfined)"
test ! -s "$out/profiles-removed.txt"
journal "$(python3 -I -c 'import json, sys; d = json.load(open(sys.argv[1])); d["state"] = "loaded"; print(json.dumps(d))' "$out/setup.json")"
set +e
python3 "$root/tests/selection/namespace_host_probe.py" > "$out/probe-unconfined.jsonl" 2>&1
echo "$?" > "$out/probe-unconfined.exit"
python3 "$root/tests/selection/namespace_host_probe.py" --apparmor-profile "$name" > "$out/probe-profile.jsonl" 2>&1
status=$?
set -e
echo "$status" > "$out/probe-profile.exit"
test "$status" -eq 0
grep -q "\"stage\": \"profile-entered\".*\"label\": \"$name (unconfined)\"" "$out/probe-profile.jsonl"
