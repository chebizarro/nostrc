#!/usr/bin/env bash
# The single provisioned namespace-policy testhost controller. Docker provisions a
# regression host, not a native selector/profile backend. Both the reusable
# regression workflow and the stable static-checks job invoke these operations,
# one per step, after proving their exact checkout; the per-job AppArmor setup
# and teardown scripts and the `if: always()` removal steps stay in the caller so
# always() requests owned cleanup while the runner is available. Hard timeout
# or VM-loss absence is unverified/failed, never evidence of cleanup success.
set -euo pipefail
: "${RUNNER_TEMP:?}" "${REQUESTED_SHA:?}"
control="$RUNNER_TEMP/namespace-policy/control"
evidence="$RUNNER_TEMP/namespace-policy/evidence"
case "${1:-}" in
  bind)
    test "$(git rev-parse HEAD)" = "$REQUESTED_SHA"
    mkdir -p "$control" "$evidence"
    git submodule status --recursive > "$control/submodules.txt"
    sha256sum tests/selection/namespace_host_probe.py scripts/adapter_namespace_custody.py \
      tests/selection/namespace-testhost.Dockerfile > "$control/sources.sha256"
    set +e
    python3 tests/selection/namespace_host_probe.py > "$control/native-host-probe.jsonl" 2>&1
    status=$?
    set -e
    # Diagnostic refusal is retained, never counted as positive coverage.
    printf '%s\n' "$status" > "$control/native-host-probe.exit"
    ;;
  record)
    docker info --format '{{json .SecurityOptions}}' > "$control/docker-security.json"
    docker version > "$control/docker-version.txt"
    python3 - <<'PY'
import json, os
from pathlib import Path
control = Path(os.environ['RUNNER_TEMP']) / 'namespace-policy/control'
security = json.loads((control / 'docker-security.json').read_text())
state = dict(kind='namespace_testhost_setup_v1', docker_security=security,
    container_apparmor='per-job profile named by control/apparmor/setup.json', source_sha=os.environ['REQUESTED_SHA'],
    scope='provisioned_container_regressions_not_authored_native_profile',
    complete=False, complete_gate_eligible=False,
    policy_authority='disposable GitHub-hosted runner only; fresh per-job path-unattached userns profile added and exactly removed; kernel settings unchanged')
(control / 'setup.json').write_text(json.dumps(state, indent=2) + '\n')
PY
    ;;
  build)
    docker build --progress plain --iidfile "$control/image.id" \
      -f tests/selection/namespace-testhost.Dockerfile tests/selection \
      2>&1 | tee "$control/image-build.log"
    docker image inspect "$(cat "$control/image.id")" > "$control/image.json"
    ;;
  execute)
    # No Docker socket, host PID namespace, added capabilities or writable checkout.
    chmod 0777 "$evidence"
    python3 tests/selection/namespace_testhost_lifecycle.py prepare "$control"
    profile="$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["name"])' "$control/apparmor/setup.json")"
    docker create --cidfile "$control/container.cid" --name "$(cat "$control/container.name")" \
      --label "nostrc.namespace-policy=$(cat "$control/owner.token")" \
      --user 1001:1001 --cap-drop=ALL --security-opt=no-new-privileges \
      --security-opt=seccomp=unconfined --security-opt=systempaths=unconfined \
      --security-opt="apparmor=$profile" --network none --read-only \
      --tmpfs /tmp:rw,exec,nosuid,nodev,mode=1777 \
      --tmpfs /tmp/.X11-unix:rw,nosuid,nodev,mode=1777 \
      --mount "type=bind,source=$GITHUB_WORKSPACE,target=/source,readonly" \
      --mount "type=bind,source=$evidence,target=/evidence" \
      -e HOME=/tmp/namespace-home -e NOSTRC_CUSTODY_TEST_EVIDENCE=/evidence/custody \
      -e "NOSTRC_NAMESPACE_APPARMOR_PROFILE=$profile" \
      --entrypoint /bin/bash "$(cat "$control/image.id")" \
      tests/selection/namespace_policy_entry.sh
    cid="$(cat "$control/container.cid")"
    docker inspect "$cid" > "$control/container-before.json"
    set +e
    docker start --attach "$cid" 2>&1 | tee "$control/container.log"
    attach_status=${PIPESTATUS[0]}
    set -e
    docker inspect "$cid" > "$control/container-after.json"
    printf '%s\n' "$attach_status" > "$control/attach.exit"
    status="$(docker wait "$cid")"
    printf '%s\n' "$status" > "$control/container.exit"
    test "$attach_status" -eq 0
    test "$status" -eq 0
    ;;
  remove)
    if [[ -f "$control/owner.json" ]]; then
      python3 tests/selection/namespace_testhost_lifecycle.py cleanup "$control"
    fi
    ;;
  *)
    echo 'usage: namespace_policy_controller.sh bind|record|build|execute|remove' >&2
    exit 2
    ;;
esac
