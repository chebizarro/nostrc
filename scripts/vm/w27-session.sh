#!/usr/bin/env bash
# Per-user, isolated VM display/session for a throwaway Groundhog acceptance run.
# Never starts or changes GDM or system services. Run separately on each VM.
set -euo pipefail

# shellcheck source=scripts/vm/w27-paths.sh
source "$(dirname -- "${BASH_SOURCE[0]}")/w27-paths.sh"
action=${1:-start}
[[ $action == start || $action == restart-groundhog || $action == status ]] || {
  echo "usage: W27_RUN_ID=YYYYMMDDTHHMMSS $0 {start|restart-groundhog|status}" >&2; exit 2;
}
w27_init_run_paths
run_dir=$W27_RUN_PATH
build=${W27_BUILD_DIR:-"$W27_HOME/nostrc/build-w27"}
w27_absolute_path "$build"
display=${W27_DISPLAY:-:99}
[[ $display =~ ^:[0-9]+$ ]] || w27_fail 'W27_DISPLAY must be an X11 display like :99'
for subdir in runtime data screenshots; do w27_run_subdir "$run_dir/$subdir"; done
export XDG_RUNTIME_DIR="$run_dir/runtime"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$run_dir/runtime/bus.sock"
export XDG_DATA_HOME="$run_dir/data"
export DISPLAY="$display" GDK_BACKEND=x11 G_MESSAGES_DEBUG=all

is_running() {
  local pid
  w27_run_file "$run_dir/$1.pid" || return 1
  [[ -s $run_dir/$1.pid ]] || return 1
  read -r pid <"$run_dir/$1.pid"
  [[ $pid =~ ^[1-9][0-9]*$ ]] && kill -0 "$pid" 2>/dev/null
}
launch() {
  local name=$1 log=$2; shift 2
  w27_run_file "$run_dir/$name.pid"
  w27_run_file "$run_dir/$log"
  if ! is_running "$name"; then
    nohup "$@" >>"$run_dir/$log" 2>&1 </dev/null &
    printf '%s\n' "$!" >"$run_dir/$name.pid"
  fi
}

groundhog_exe="$build/gnome/groundhog/groundhog"
groundhog_pidfile="$run_dir/groundhog.pid"
groundhog_log="$run_dir/groundhog.log"
proc_start_time() { awk '{print $22}' "/proc/$1/stat" 2>/dev/null; }
same_pid_instance() {
  [[ -r /proc/$1/stat ]] && [[ $(proc_start_time "$1") == "$2" ]] &&
    [[ $(awk '{print $3}' "/proc/$1/stat") != Z ]]
}
read_groundhog_record() {
  local extra
  w27_run_file "$groundhog_pidfile" || return 1
  [[ -s $groundhog_pidfile ]] || return 1
  read -r gh_pid gh_start gh_token extra <"$groundhog_pidfile"
  [[ -z ${extra-} && $gh_pid =~ ^[1-9][0-9]*$ &&
     $gh_start =~ ^[1-9][0-9]*$ && $gh_token =~ ^[0-9a-f]{32}$ ]]
}
groundhog_owned() {
  read_groundhog_record || return 1
  [[ -x $groundhog_exe ]] || return 1
  same_pid_instance "$gh_pid" "$gh_start" || return 1
  [[ $(stat -c %u "/proc/$gh_pid") == "$(id -u)" ]] || return 1
  [[ $(readlink -f "/proc/$gh_pid/exe") == "$(readlink -f "$groundhog_exe")" ]] || return 1
  tr '\0' '\n' <"/proc/$gh_pid/environ" |
    grep -Fx -- "W27_GROUNDHOG_TOKEN=$gh_token" >/dev/null
}
start_groundhog() {
  local pid token start attempts
  local -a extra=()
  w27_run_file "$groundhog_pidfile"
  w27_run_file "$groundhog_log"
  [[ -x $groundhog_exe ]] || w27_fail "Groundhog executable missing: $groundhog_exe" || return 1
  if [[ -e $groundhog_pidfile ]]; then
    if groundhog_owned; then return 0; fi
    w27_fail 'Groundhog PID record is stale or mismatched; inspect it before manual removal'
    return 1
  fi
  if [[ -e $run_dir/blossom.crt ]]; then
    [[ ! -L $run_dir/blossom.crt && -f $run_dir/blossom.crt ]] ||
      w27_fail 'unsafe blossom.crt' || return 1
    extra+=("G_TLS_CA_FILE=$run_dir/blossom.crt")
  fi
  token=$(openssl rand -hex 16)
  nohup env "W27_GROUNDHOG_TOKEN=$token" "${extra[@]}" \
    "GSETTINGS_SCHEMA_DIR=$build/gnome/groundhog" \
    "$groundhog_exe" >>"$groundhog_log" 2>&1 </dev/null &
  pid=$!
  for ((attempts=0; attempts<30; attempts++)); do
    if [[ -r /proc/$pid/stat ]] &&
       [[ $(readlink -f "/proc/$pid/exe" 2>/dev/null) == "$(readlink -f "$groundhog_exe")" ]]; then
      start=$(proc_start_time "$pid")
      printf '%s %s %s\n' "$pid" "$start" "$token" >"$groundhog_pidfile"
      groundhog_owned && return 0
      rm -f -- "$groundhog_pidfile"
      break
    fi
    sleep 0.1
  done
  w27_fail 'Groundhog did not become a verified process within 3 seconds'
}

case "$action" in
  start)
    w27_run_file "$run_dir/throwaway.hex"
    if [[ ! -e $run_dir/throwaway.hex ]]; then
      (set -C; openssl rand -hex 32 >"$run_dir/throwaway.hex")
    fi
    [[ -s $run_dir/throwaway.hex ]] || w27_fail 'throwaway key file is empty'
    if ! dbus-send --session --dest=org.freedesktop.DBus --print-reply \
      /org/freedesktop/DBus org.freedesktop.DBus.GetId >/dev/null 2>&1; then
      rm -f -- "$run_dir/runtime/bus.sock"
      w27_run_file "$run_dir/dbus.pid"
      dbus-daemon --session --address="$DBUS_SESSION_BUS_ADDRESS" --fork \
        --print-pid=1 >"$run_dir/dbus.pid"
    fi
    launch xvfb xvfb.log Xvfb "$display" -screen 0 1280x800x24 -nolisten tcp
    sleep 1
    launch openbox openbox.log openbox
    if ! gdbus introspect --session --dest org.freedesktop.secrets \
      --object-path /org/freedesktop/secrets >/dev/null 2>&1; then
      w27_run_file "$run_dir/keyring.log"
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
    w27_run_file "$run_dir/store-key.log"
    gdbus call --session --dest org.nostr.Signer --object-path /org/nostr/signer \
      --method org.nostr.Signer.StoreKey "$(cat "$run_dir/throwaway.hex")" \
      "W27 throwaway $(hostname)" >"$run_dir/store-key.log"
    launch signer signer.log env \
      "GSETTINGS_SCHEMA_DIR=$build/apps/gnostr-signer" \
      "$build/apps/gnostr-signer/gnostr-signer"
    start_groundhog
    ;;
  restart-groundhog)
    groundhog_owned || w27_fail 'Groundhog PID record is stale or not script-owned; refusing TERM/relaunch' || exit 1
    pid=$gh_pid
    start=$gh_start
    kill -TERM "$pid" || w27_fail "could not signal verified Groundhog PID $pid" || exit 1
    for ((attempts=0; attempts<100; attempts++)); do
      if ! same_pid_instance "$pid" "$start"; then break; fi
      sleep 0.1
    done
    if same_pid_instance "$pid" "$start"; then
      w27_fail "Groundhog PID $pid did not exit within 10 seconds; refusing overlap"
      exit 1
    fi
    rm -f -- "$groundhog_pidfile"
    start_groundhog
    ;;
  status)
    for name in dbus xvfb openbox daemon signer; do
      if is_running "$name"; then printf '%s %s\n' "$name" "$(cat "$run_dir/$name.pid")"; fi
    done
    if groundhog_owned; then printf 'groundhog %s\n' "$gh_pid"; fi
    ;;
 esac
printf 'run directory: %s\n' "$run_dir"
