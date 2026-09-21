#!/usr/bin/env bash
#
# vm-linux-ci.sh — Linux build/test parity loop for nostr-homed against a
# disposable UTM (or any SSH-reachable) Ubuntu 24.04 VM.
#
# Tracks beads nostrc-rb0e.15 ([D-ci]). Builds the nostr-homed portable core
# (auth_core + identity_core + domain_config) in-tree via the root CMake build
# and runs the 11 portable tests on real Linux. Runs whatever architecture the
# VM is (arm64 on Apple Silicon is expected; the amd64 release pin — nostrc-rb0e.14
# — is satisfied elsewhere).
#
# Builds Debug for better diagnostics. The tests use NH_CHECK (always-on), so
# they are no longer vacuous under NDEBUG (nostrc-rb0e.16); Debug just keeps
# symbols/asserts handy when a failure needs gdb.
#
# It does NOT test PAM/NSS/GDM/Samba login: that runtime does not install yet
# (NOSTR_HOMED_ENABLE_AUTH_INSTALL is a configure-time FATAL_ERROR).
#
# Usage:
#   VM_SSH=bizarro@192.168.64.3 scripts/vm-linux-ci.sh
#   VM_SSH=bizarro@192.168.64.3 SANITIZE=1 scripts/vm-linux-ci.sh
#
# Env (all optional except VM_SSH): VM_PORT, VM_SSH_KEY, VM_WORKDIR, BRANCH,
#   REPO_URL, LOG_DIR, SANITIZE, SKIP_DEPS, SUDO, JOBS.
#
set -euo pipefail

fail() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m==>\033[0m %s\n' "$*" >&2; }

: "${VM_SSH:?set VM_SSH to the VM ssh destination, e.g. bizarro@192.168.64.3}"
VM_PORT="${VM_PORT:-22}"
VM_WORKDIR="${VM_WORKDIR:-nostrc-ci}"
BRANCH="${BRANCH:-master}"
REPO_URL="${REPO_URL:-https://github.com/chebizarro/nostrc.git}"
SANITIZE="${SANITIZE:-0}"
SKIP_DEPS="${SKIP_DEPS:-0}"
SUDO="${SUDO:-sudo}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
LOG_DIR="${LOG_DIR:-vm-ci-logs/$STAMP}"

SSH_OPTS=(-p "$VM_PORT" -o BatchMode=yes -o ConnectTimeout=15 -o StrictHostKeyChecking=accept-new)
SCP_OPTS=(-P "$VM_PORT" -o BatchMode=yes -o ConnectTimeout=15 -o StrictHostKeyChecking=accept-new)
if [ -n "${VM_SSH_KEY:-}" ]; then SSH_OPTS+=(-i "$VM_SSH_KEY"); SCP_OPTS+=(-i "$VM_SSH_KEY"); fi

info "Preflight: checking SSH to $VM_SSH:$VM_PORT"
ssh "${SSH_OPTS[@]}" "$VM_SSH" 'echo ok >/dev/null' \
  || fail "cannot SSH to $VM_SSH (set VM_SSH/VM_PORT/VM_SSH_KEY; enable BatchMode key auth)"
REMOTE_ARCH="$(ssh "${SSH_OPTS[@]}" "$VM_SSH" 'uname -m')"
REMOTE_OS="$(ssh "${SSH_OPTS[@]}" "$VM_SSH" '. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}"')"
info "Remote: $REMOTE_OS ($REMOTE_ARCH)"

# Remote payload runs on the VM. No single quotes below this line inside the
# single-quoted string — use double quotes only.
REMOTE_PAYLOAD='
set -euo pipefail
log() { printf "\033[36m[vm]\033[0m %s\n" "$*"; }
WORKDIR="$1"; REPO_URL="$2"; BRANCH="$3"; SANITIZE="$4"; SKIP_DEPS="$5"; SUDO="$6"; JOBS="$7"
[ -n "$JOBS" ] || JOBS="$(nproc)"
case "$WORKDIR" in /*) ;; *) WORKDIR="$HOME/$WORKDIR" ;; esac
SRC="$WORKDIR/src"; BUILD="$WORKDIR/build"; STAGE="$WORKDIR/stage"; LOGS="$WORKDIR/ci-logs"
mkdir -p "$WORKDIR"; rm -rf "$BUILD" "$STAGE" "$LOGS"; mkdir -p "$LOGS"
: > "$LOGS/summary.txt"
note() { echo "$1" | tee -a "$LOGS/summary.txt"; }

if [ "$SKIP_DEPS" != "1" ]; then
  log "installing build dependencies (apt)"
  export DEBIAN_FRONTEND=noninteractive
  $SUDO apt-get update -qq
  $SUDO apt-get install -y -qq \
    build-essential cmake ninja-build pkg-config git ca-certificates python3 gdb \
    libsecp256k1-dev libssl-dev libjansson-dev libsqlite3-dev libglib2.0-dev \
    libnsync-dev libwebsockets-dev libcurl4-openssl-dev libsodium-dev libgit2-dev libzstd-dev libpam0g-dev \
    >"$LOGS/00-apt.log" 2>&1 || { tail -40 "$LOGS/00-apt.log"; exit 1; }
fi

log "syncing $BRANCH from $REPO_URL"
if [ ! -d "$SRC/.git" ]; then
  git clone --no-recurse-submodules "$REPO_URL" "$SRC" >"$LOGS/01-clone.log" 2>&1
fi
git -C "$SRC" fetch --prune origin "$BRANCH" >>"$LOGS/01-clone.log" 2>&1
git -C "$SRC" reset --hard HEAD >>"$LOGS/01-clone.log" 2>&1 || true
git -C "$SRC" checkout -f -B "$BRANCH" "origin/$BRANCH" >>"$LOGS/01-clone.log" 2>&1
git -C "$SRC" reset --hard "origin/$BRANCH" >>"$LOGS/01-clone.log" 2>&1
note "commit: $(git -C "$SRC" rev-parse --short HEAD) $(git -C "$SRC" log -1 --pretty=%s)"

CMAKE_ARGS=(-G Ninja -DCMAKE_BUILD_TYPE=Debug
  -DENABLE_NOSTR_HOMED=ON -DLIBNOSTR_WITH_NOSTRDB=OFF
  -DNOSTR_HOMED_ENABLE_AUTH_CORE=ON -DNOSTR_HOMED_ENABLE_IDENTITY_CORE=ON
  -DNOSTR_HOMED_ENABLE_DOMAIN_CONFIG=ON
  -DNOSTR_HOMED_ENABLE_NSS=ON -DNOSTR_HOMED_ENABLE_AUTH_RUNTIME=ON -DNOSTR_HOMED_ENABLE_PAM=ON
  -DBUILD_APPS=OFF -DBUILD_NOSTR_GTK=OFF)
if [ "$SANITIZE" = "1" ]; then
  SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
  CMAKE_ARGS+=(-DCMAKE_C_FLAGS="$SAN" -DCMAKE_CXX_FLAGS="$SAN" -DCMAKE_EXE_LINKER_FLAGS="$SAN")
  note "sanitizers: ASan+UBSan ENABLED"
else
  note "sanitizers: off"
fi

log "configuring root build (nostr-homed portable, GUI/apps off)"
cmake -S "$SRC" -B "$BUILD" "${CMAKE_ARGS[@]}" >"$LOGS/02-configure.log" 2>&1 \
  || { tail -40 "$LOGS/02-configure.log"; exit 1; }

log "building nostr-homed test targets"
TARGETS="$(grep -oE "add_executable\(test_[a-z_0-9]+" "$SRC/gnome/nostr-homed/CMakeLists.txt" | sed "s/add_executable(//" | sort -u)"
note "test targets: $(echo $TARGETS | wc -w)"
cmake --build "$BUILD" -j "$JOBS" --target $TARGETS nss_nostr nostr-authd pam_nostr nh-seed-authority >"$LOGS/03-build.log" 2>&1 \
  || { tail -60 "$LOGS/03-build.log"; exit 1; }

log "running portable ctest suite"
RC=0
ctest --test-dir "$BUILD" -L "portable|auth-runtime" --output-on-failure >"$LOGS/04-ctest.log" 2>&1 || RC=$?
note "$(grep -E "tests passed|tests failed" "$LOGS/04-ctest.log" | tail -1)"
FAILED="$(grep -E "\*\*\*Failed|SEGFAULT|Failed " "$LOGS/04-ctest.log" | grep -oE "homed_[a-z_]+" | sort -u | tr "\n" " " || true)"
[ -n "$FAILED" ] && note "failing: $FAILED"

note "RESULT: $([ "$RC" = "0" ] && echo PASS || echo FAIL) (arch $(uname -m), Debug)"
echo "$RC" > "$LOGS/exit-code"
exit "$RC"
'

info "Running CI payload on VM ($VM_WORKDIR, branch $BRANCH, sanitize=$SANITIZE)"
set +e
ssh "${SSH_OPTS[@]}" "$VM_SSH" \
  "bash -s -- '$VM_WORKDIR' '$REPO_URL' '$BRANCH' '$SANITIZE' '$SKIP_DEPS' '$SUDO' '${JOBS:-}'" \
  <<<"$REMOTE_PAYLOAD"
REMOTE_RC=$?
set -e

info "Pulling logs to $LOG_DIR"
mkdir -p "$LOG_DIR"
REMOTE_LOGS="$(ssh "${SSH_OPTS[@]}" "$VM_SSH" "cd '$VM_WORKDIR/ci-logs' 2>/dev/null && pwd" 2>/dev/null || true)"
[ -n "$REMOTE_LOGS" ] && scp "${SCP_OPTS[@]}" -r "$VM_SSH:$REMOTE_LOGS/." "$LOG_DIR/" >/dev/null 2>&1 || info "log pull incomplete"

echo
[ -f "$LOG_DIR/summary.txt" ] && { info "Summary:"; sed 's/^/    /' "$LOG_DIR/summary.txt"; }
if [ "$REMOTE_RC" = "0" ]; then
  info "PASS — Linux portable parity confirmed on $REMOTE_ARCH. Logs: $LOG_DIR"
else
  fail "FAIL (remote exit $REMOTE_RC). Inspect logs in $LOG_DIR"
fi
