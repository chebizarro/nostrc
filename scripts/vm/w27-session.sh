#!/usr/bin/env bash
# Per-user, isolated VM display/session for a throwaway Groundhog acceptance run.
# Never starts or changes GDM or system services. Run separately on each VM.
set -euo pipefail

: "${W27_RUN_ID:?Set W27_RUN_ID, for example 20261003T171803}"
action=${1:-start}
run_dir=${W27_RUN_DIR:-"$HOME/gh-run/$W27_RUN_ID"}
build=${W27_BUILD_DIR:-"$HOME/nostrc/build-w27"}
display=${W27_DISPLAY:-:99}
mkdir -p "$run_dir/runtime" "$run_dir/data" "$run_dir/screenshots"
chmod 700 "$run_dir" "$run_dir/runtime" "$run_dir/data"
export XDG_RUNTIME_DIR="$run_dir/runtime"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$run_dir/runtime/bus.sock"
export XDG_DATA_HOME="$run_dir/data"
export DISPLAY="$display" GDK_BACKEND=x11 G_MESSAGES_DEBUG=all

is_running() { [[ -s "$run_dir/$1.pid" ]] && kill -0 "$(cat "$run_dir/$1.pid")" 2>/dev/null; }
launch() {
  local name=$1 log=$2; shift 2
  if ! is_running "$name"; then
    nohup "$@" >>"$run_dir/$log" 2>&1 </dev/null &
    echo $! >"$run_dir/$name.pid"
  fi
}
start_groundhog() {
  local -a extra=()
  [[ ! -f "$run_dir/blossom.crt" ]] || extra+=("G_TLS_CA_FILE=$run_dir/blossom.crt")
  launch groundhog groundhog.log env "${extra[@]}" \
    "GSETTINGS_SCHEMA_DIR=$build/gnome/groundhog" \
    "$build/gnome/groundhog/groundhog"
}

case "$action" in
  start)
    if [[ ! -s "$run_dir/throwaway.hex" ]]; then
      (umask 077; openssl rand -hex 32 >"$run_dir/throwaway.hex")
    fi
    if ! DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" \
      dbus-send --session --dest=org.freedesktop.DBus --print-reply \
      /org/freedesktop/DBus org.freedesktop.DBus.GetId >/dev/null 2>&1; then
      rm -f "$run_dir/runtime/bus.sock"
      dbus-daemon --session --address="$DBUS_SESSION_BUS_ADDRESS" --fork \
        --print-pid=1 >"$run_dir/dbus.pid"
    fi
    # Headless GNOME Shell is preferable when its screenshot interface works;
    # these VMs needed the documented Xvfb fallback.
    launch xvfb xvfb.log Xvfb "$display" -screen 0 1280x800x24 -nolisten tcp
    sleep 1
    launch openbox openbox.log openbox
    # The daemon forks after receiving an empty unlock password on stdin.
    if ! DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" \
      gdbus introspect --session --dest org.freedesktop.secrets \
      --object-path /org/freedesktop/secrets >/dev/null 2>&1; then
      printf '\n' | gnome-keyring-daemon --unlock --components=secrets \
        >>"$run_dir/keyring.log" 2>&1
    fi
    launch daemon daemon.log env \
      "GSETTINGS_SCHEMA_DIR=$build/apps/gnostr-signer" \
      NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1 \
      NOSTR_SIGNER_TEST_PENDING_TTL_S=1800 \
      "NOSTR_SIGNER_TEST_APPROVERS=$build/apps/gnostr-signer/gnostr-signer" \
      "NOSTR_SIGNER_SECKEY_HEX=$(cat "$run_dir/throwaway.hex")" \
      "$build/apps/gnostr-signer/gnostr-signer-daemon"
    sleep 1
    # StoreKey is the real signer D-Bus API, not a Groundhog MLS test hook.
    gdbus call --session --dest org.nostr.Signer --object-path /org/nostr/signer \
      --method org.nostr.Signer.StoreKey "$(cat "$run_dir/throwaway.hex")" \
      "W27 throwaway $(hostname)" >"$run_dir/store-key.log"
    launch signer signer.log env \
      "GSETTINGS_SCHEMA_DIR=$build/apps/gnostr-signer" \
      "$build/apps/gnostr-signer/gnostr-signer"
    start_groundhog
    ;;
  restart-groundhog)
    if is_running groundhog; then kill "$(cat "$run_dir/groundhog.pid")"; fi
    rm -f "$run_dir/groundhog.pid"
    start_groundhog
    ;;
  status)
    for name in dbus xvfb openbox daemon signer groundhog; do
      if is_running "$name"; then printf '%s %s\n' "$name" "$(cat "$run_dir/$name.pid")"; fi
    done
    ;;
  *) echo "usage: W27_RUN_ID=... $0 {start|restart-groundhog|status}" >&2; exit 2 ;;
esac
printf 'run directory: %s\n' "$run_dir"
