#!/usr/bin/env bash
#
# vm-linux-ci.sh — Linux build/test parity loop for nostr-homed against a
# disposable UTM (or any SSH-reachable) Ubuntu 24.04 VM.
#
# Tracks beads nostrc-rb0e.15 ([D-ci]). This proves the nostr-homed PORTABLE
# core (auth_core + identity_core + domain_config) builds and passes the 11
# portable tests on real Linux. It runs whatever architecture the VM is
# (arm64 on Apple Silicon via UTM/Apple Virtualization is expected and fine for
# the dev loop; the amd64 release pin — see nostrc-rb0e.14 — is satisfied
# elsewhere).
#
# It does NOT test PAM/NSS/GDM/Samba login: that runtime does not install yet
# (NOSTR_HOMED_ENABLE_AUTH_INSTALL is a configure-time FATAL_ERROR until the
# B4/A3 broker+PAM+NSS install work lands — see nostrc-zcll.5 / nostrc-nxpb.4).
#
# Usage:
#   VM_SSH=ubuntu@192.168.64.5 scripts/vm-linux-ci.sh
#   VM_SSH=ubuntu@vm SANITIZE=1 scripts/vm-linux-ci.sh
#
# Common env vars (all optional except VM_SSH):
#   VM_SSH        ssh destination, e.g. "ubuntu@192.168.64.5"   (REQUIRED)
#   VM_PORT       ssh port                                      (default 22)
#   VM_SSH_KEY    path to private key for -i                    (default: agent/config)
#   VM_WORKDIR    remote working dir                            (default ~/nostrc-ci)
#   BRANCH        branch to test           (default feat/nostr-linux-samba-login-20260920)
#   REPO_URL      git remote to clone from (default https://github.com/chebizarro/nostrc.git)
#   LOG_DIR       local dir for pulled logs    (default ./vm-ci-logs/<timestamp>)
#   SANITIZE      1 to build+test with ASan/UBSan               (default 0)
#   SKIP_DEPS     1 to skip apt-get install                     (default 0)
#   RUN_MESON     1 to also run the meson portable suite        (default 0)
#   SUDO          remote privilege command                     (default "sudo")
#   JOBS          parallel build jobs                           (default: remote nproc)
#
set -euo pipefail

fail() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m==>\033[0m %s\n' "$*" >&2; }

: "${VM_SSH:?set VM_SSH to the VM ssh destination, e.g. ubuntu@192.168.64.5}"
VM_PORT="${VM_PORT:-22}"
VM_WORKDIR="${VM_WORKDIR:-nostrc-ci}"   # relative => under remote $HOME
BRANCH="${BRANCH:-feat/nostr-linux-samba-login-20260920}"
REPO_URL="${REPO_URL:-https://github.com/chebizarro/nostrc.git}"
SANITIZE="${SANITIZE:-0}"
SKIP_DEPS="${SKIP_DEPS:-0}"
RUN_MESON="${RUN_MESON:-0}"
SUDO="${SUDO:-sudo}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
LOG_DIR="${LOG_DIR:-vm-ci-logs/$STAMP}"

SSH_OPTS=(-p "$VM_PORT" -o BatchMode=yes -o ConnectTimeout=15 -o StrictHostKeyChecking=accept-new)
SCP_OPTS=(-P "$VM_PORT" -o BatchMode=yes -o ConnectTimeout=15 -o StrictHostKeyChecking=accept-new)
if [ -n "${VM_SSH_KEY:-}" ]; then
  SSH_OPTS+=(-i "$VM_SSH_KEY")
  SCP_OPTS+=(-i "$VM_SSH_KEY")
fi

info "Preflight: checking SSH to $VM_SSH:$VM_PORT"
ssh "${SSH_OPTS[@]}" "$VM_SSH" 'echo ok >/dev/null' \
  || fail "cannot SSH to $VM_SSH (set VM_SSH/VM_PORT/VM_SSH_KEY; enable BatchMode key auth)"
REMOTE_ARCH="$(ssh "${SSH_OPTS[@]}" "$VM_SSH" 'uname -m')"
REMOTE_OS="$(ssh "${SSH_OPTS[@]}" "$VM_SSH" '. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}"')"
info "Remote: $REMOTE_OS ($REMOTE_ARCH)"

# The remote payload runs entirely on the VM. Config is passed via env.
REMOTE_PAYLOAD='
set -euo pipefail
log() { printf "\033[36m[vm]\033[0m %s\n" "$*"; }
WORKDIR="$1"; REPO_URL="$2"; BRANCH="$3"; SANITIZE="$4"; SKIP_DEPS="$5"; RUN_MESON="$6"; SUDO="$7"; JOBS="$8"
[ -n "$JOBS" ] || JOBS="$(nproc)"
mkdir -p "$WORKDIR"; cd "$WORKDIR"
SRC="$WORKDIR/src"; PREFIX="$WORKDIR/prefix"; BUILD="$WORKDIR/build"; STAGE="$WORKDIR/stage"; LOGS="$WORKDIR/ci-logs"
rm -rf "$BUILD" "$STAGE" "$LOGS"; mkdir -p "$LOGS"
: > "$LOGS/summary.txt"
note() { echo "$1" | tee -a "$LOGS/summary.txt"; }

if [ "$SKIP_DEPS" != "1" ]; then
  log "installing build dependencies (apt)"
  export DEBIAN_FRONTEND=noninteractive
  $SUDO apt-get update -qq
  $SUDO apt-get install -y -qq \
    build-essential cmake ninja-build pkg-config git ca-certificates python3 \
    libsecp256k1-dev libssl-dev libjansson-dev libsqlite3-dev libglib2.0-dev meson \
    >"$LOGS/00-apt.log" 2>&1 || { tail -40 "$LOGS/00-apt.log"; exit 1; }
fi

log "syncing $BRANCH from $REPO_URL"
if [ ! -d "$SRC/.git" ]; then
  git clone --no-recurse-submodules "$REPO_URL" "$SRC" >"$LOGS/01-clone.log" 2>&1
fi
cd "$SRC"
git fetch --prune origin "$BRANCH" >>"$LOGS/01-clone.log" 2>&1
git checkout -B "$BRANCH" "origin/$BRANCH" >>"$LOGS/01-clone.log" 2>&1
git reset --hard "origin/$BRANCH" >>"$LOGS/01-clone.log" 2>&1
note "commit: $(git rev-parse --short HEAD) $(git log -1 --pretty=%s)"

CMAKE_COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib)
if [ "$SANITIZE" = "1" ]; then
  SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
  CMAKE_COMMON+=(-DCMAKE_C_FLAGS="$SAN" -DCMAKE_CXX_FLAGS="$SAN" -DCMAKE_EXE_LINKER_FLAGS="$SAN" -DCMAKE_SHARED_LINKER_FLAGS="$SAN")
  note "sanitizers: ASan+UBSan ENABLED"
else
  note "sanitizers: off"
fi
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

log "building + installing libgo"
cmake -S libgo -B "$BUILD/libgo" "${CMAKE_COMMON[@]}" >"$LOGS/02-libgo-config.log" 2>&1 \
  || { tail -60 "$LOGS/02-libgo-config.log"; exit 1; }
cmake --build "$BUILD/libgo" -j "$JOBS" >"$LOGS/03-libgo-build.log" 2>&1 \
  || { tail -80 "$LOGS/03-libgo-build.log"; exit 1; }
cmake --install "$BUILD/libgo" >"$LOGS/03-libgo-install.log" 2>&1

log "building + installing libnostr (nostrdb OFF)"
cmake -S libnostr -B "$BUILD/libnostr" "${CMAKE_COMMON[@]}" -DLIBNOSTR_WITH_NOSTRDB=OFF >"$LOGS/04-libnostr-config.log" 2>&1 \
  || { tail -60 "$LOGS/04-libnostr-config.log"; exit 1; }
cmake --build "$BUILD/libnostr" -j "$JOBS" >"$LOGS/05-libnostr-build.log" 2>&1 \
  || { tail -80 "$LOGS/05-libnostr-build.log"; exit 1; }
cmake --install "$BUILD/libnostr" >"$LOGS/05-libnostr-install.log" 2>&1

log "configuring + building nostr-homed (auth_core + identity_core + domain_config)"
cmake -S gnome/nostr-homed -B "$BUILD/homed" "${CMAKE_COMMON[@]}" \
  -DNOSTR_HOMED_ENABLE_AUTH_CORE=ON \
  -DNOSTR_HOMED_ENABLE_IDENTITY_CORE=ON \
  -DNOSTR_HOMED_ENABLE_DOMAIN_CONFIG=ON >"$LOGS/06-homed-config.log" 2>&1 \
  || { tail -80 "$LOGS/06-homed-config.log"; exit 1; }
cmake --build "$BUILD/homed" -j "$JOBS" >"$LOGS/07-homed-build.log" 2>&1 \
  || { tail -100 "$LOGS/07-homed-build.log"; exit 1; }

log "running portable ctest suite"
RC=0
( cd "$BUILD/homed" && ctest --output-on-failure ) >"$LOGS/08-ctest.log" 2>&1 || RC=$?
PASSLINE="$(grep -E 'tests passed|tests failed' "$LOGS/08-ctest.log" | tail -1 || true)"
note "cmake ctest: ${PASSLINE:-no summary} (exit $RC)"

log "verifying inert domain-config staged install"
DESTDIR="$STAGE" cmake --install "$BUILD/homed" >"$LOGS/09-stage-install.log" 2>&1 || true
( cd "$STAGE" && find . -type f | sort ) >"$LOGS/09-stage-manifest.txt" 2>&1 || true
note "staged files: $(wc -l < "$LOGS/09-stage-manifest.txt" 2>/dev/null || echo 0)"
VALIDATOR="$(find "$STAGE" -name validate_domain_profile.py | head -1 || true)"
SAMPLE="$SRC/gnome/nostr-homed/packaging/domain/domain-profile.manifest.json.sample"
if [ -n "$VALIDATOR" ] && [ -f "$SAMPLE" ]; then
  if python3 "$VALIDATOR" "$SAMPLE" >"$LOGS/10-validator.log" 2>&1; then
    note "domain validator: PASS on sample manifest"
  else
    note "domain validator: NON-ZERO (see 10-validator.log)"; RC=1
  fi
else
  note "domain validator: not staged / sample missing (skipped)"
fi

if [ "$RUN_MESON" = "1" ]; then
  log "running meson portable suite"
  meson setup "$BUILD/homed-meson" gnome/nostr-homed \
    -Dauth_core=enabled -Didentity_core=enabled -Ddomain_config=enabled \
    >"$LOGS/11-meson-setup.log" 2>&1 || { tail -60 "$LOGS/11-meson-setup.log"; RC=1; }
  if [ -d "$BUILD/homed-meson" ]; then
    if meson test -C "$BUILD/homed-meson" --print-errorlogs >"$LOGS/12-meson-test.log" 2>&1; then
      note "meson test: $(grep -E "^Ok:|^Fail:" "$LOGS/12-meson-test.log" | tr "\n" " ")"
    else
      note "meson test: FAILED (see 12-meson-test.log)"; RC=1
    fi
  fi
fi

note "RESULT: $([ "$RC" = "0" ] && echo PASS || echo FAIL) (arch $(uname -m))"
echo "$RC" > "$LOGS/exit-code"
exit "$RC"
'

info "Running CI payload on VM ($VM_WORKDIR, branch $BRANCH, sanitize=$SANITIZE)"
set +e
ssh "${SSH_OPTS[@]}" "$VM_SSH" \
  "bash -s -- '$VM_WORKDIR' '$REPO_URL' '$BRANCH' '$SANITIZE' '$SKIP_DEPS' '$RUN_MESON' '$SUDO' '${JOBS:-}'" \
  <<<"$REMOTE_PAYLOAD"
REMOTE_RC=$?
set -e

info "Pulling logs to $LOG_DIR"
mkdir -p "$LOG_DIR"
# Resolve remote absolute log path (VM_WORKDIR may be relative to $HOME).
REMOTE_LOGS="$(ssh "${SSH_OPTS[@]}" "$VM_SSH" "cd '$VM_WORKDIR/ci-logs' && pwd" 2>/dev/null || true)"
if [ -n "$REMOTE_LOGS" ]; then
  scp "${SCP_OPTS[@]}" -r "$VM_SSH:$REMOTE_LOGS/." "$LOG_DIR/" >/dev/null 2>&1 || info "log pull incomplete"
fi

echo
if [ -f "$LOG_DIR/summary.txt" ]; then
  info "Summary:"; sed 's/^/    /' "$LOG_DIR/summary.txt"
fi
if [ "$REMOTE_RC" = "0" ]; then
  info "PASS — Linux portable parity confirmed on $REMOTE_ARCH. Logs: $LOG_DIR"
else
  fail "FAIL (remote exit $REMOTE_RC). Inspect logs in $LOG_DIR"
fi
