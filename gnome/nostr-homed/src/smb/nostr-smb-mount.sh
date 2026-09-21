#!/usr/bin/env bash
#
# nostr-smb-mount — desktop-side mount helper that consumes a Samba
# credentials= file minted by `nostr-smb-acquire` and mounts an SMB share
# for the calling desktop user.
#
# Design (D7 / beads nostrc-rb0e.8):
#
#   The mint chain is already proven: `nostr-smb-acquire` drives a Nostr
#   proof against the local nostr-authd broker and writes a mode-0600
#   Samba credentials file (default $XDG_RUNTIME_DIR/nostr-smb/credentials)
#   with `username=` and `password=` lines.  The password is short-lived
#   and revocable — the SMB authority rotates it on the next issuance and
#   sweeps expired credentials.  This helper closes the last mile: turn
#   that credentials file into a mounted share.
#
#   Two mount mechanisms are supported.  The default is `mount.cifs`
#   (cifs-utils) because it works headlessly and reads a Samba
#   credentials= file natively — the exact same file layout the acquire
#   tool writes.  A `--mode gvfs` path is provided for interactive GNOME
#   Files sessions: it invokes `gio mount smb://user@host/share` after
#   priming the login by pushing the credentials via stdin (gio's own
#   ask-password flow).  On a headless VM without a session bus / gvfs
#   backends the gvfs path is expected to be unavailable; the helper
#   reports that clearly rather than pretending to succeed.
#
#   The helper does NOT itself run acquire — callers who want a one-shot
#   "mint + mount" pass --acquire, in which case we exec the acquire tool
#   first with default sink at $XDG_RUNTIME_DIR/nostr-smb/credentials.
#
# Exit codes:
#     0  mount (or unmount) succeeded
#     1  usage / bad arguments
#     2  credentials file missing / unreadable
#     3  mount backend (cifs / gvfs) unavailable
#     4  mount failed (credentials rejected by server, share missing, …)
#     5  unmount failed
#     6  acquire step failed (only with --acquire)
#
set -euo pipefail

progname="nostr-smb-mount"
usage() {
  cat >&2 <<USAGE
Usage: $progname [options] //HOST/SHARE MOUNTPOINT
       $progname --unmount MOUNTPOINT

Mount an SMB share using a short-lived credential minted by
nostr-smb-acquire.

Positional:
  //HOST/SHARE          UNC of the SMB share (e.g. //127.0.0.1/nh_share)
  MOUNTPOINT            Existing local directory to mount onto

Options:
  --creds PATH          Samba credentials= file to consume
                        (default: \$XDG_RUNTIME_DIR/nostr-smb/credentials)
  --mode cifs|gvfs      Mount mechanism (default: cifs).  gvfs requires
                        an active session bus with gvfs backends.
  --acquire             Invoke nostr-smb-acquire first to mint the
                        credentials file at the default path (or the
                        --creds path if supplied).  Passes --provider
                        via \$NOSTR_SMB_ACQUIRE_PROVIDER when set.
  --acquire-args ARGS   Extra arguments to pass to nostr-smb-acquire
                        (e.g. --socket /run/nostr-smb-d7/user.sock).
  --uid UID             uid= mount option for cifs (default: \$UID)
  --gid GID             gid= mount option for cifs (default: id -g)
  --ro                  Mount read-only
  --unmount             Unmount MOUNTPOINT and exit
  --dry-run             Print the mount command that would run and exit
  -h, --help            Show this help

Environment:
  NOSTR_SMB_ACQUIRE     Path to nostr-smb-acquire binary
                        (default: search PATH, then nostr-smb-acquire in
                        the same directory as this script)
USAGE
}

log() { printf '%s: %s\n' "$progname" "$*" >&2; }
die() { local code="$1"; shift; log "$*"; exit "$code"; }

# --- argument parsing -------------------------------------------------------
mode=cifs
creds=""
acquire=0
acquire_args=""
uid_opt=""
gid_opt=""
ro=0
unmount=0
dry_run=0
positional=()

while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --creds) creds="${2:-}"; shift 2 ;;
    --mode) mode="${2:-}"; shift 2 ;;
    --acquire) acquire=1; shift ;;
    --acquire-args) acquire_args="${2:-}"; shift 2 ;;
    --uid) uid_opt="${2:-}"; shift 2 ;;
    --gid) gid_opt="${2:-}"; shift 2 ;;
    --ro) ro=1; shift ;;
    --unmount) unmount=1; shift ;;
    --dry-run) dry_run=1; shift ;;
    --) shift; while [ $# -gt 0 ]; do positional+=("$1"); shift; done ;;
    -*) usage; die 1 "unknown option: $1" ;;
    *) positional+=("$1"); shift ;;
  esac
done

case "$mode" in
  cifs|gvfs) ;;
  *) usage; die 1 "unknown --mode: $mode" ;;
esac

# --- unmount branch --------------------------------------------------------
if [ "$unmount" = "1" ]; then
  [ "${#positional[@]}" -eq 1 ] || { usage; die 1 "--unmount takes exactly one argument (mountpoint)"; }
  mp="${positional[0]}"
  if command -v mountpoint >/dev/null 2>&1 && ! mountpoint -q "$mp"; then
    log "not a mountpoint: $mp"
    exit 0
  fi
  # Try cifs (umount) first, then gvfs.
  if umount "$mp" >/dev/null 2>&1; then exit 0; fi
  if [ -n "${SUDO:-sudo}" ] && command -v sudo >/dev/null 2>&1; then
    if sudo -n umount "$mp" >/dev/null 2>&1; then exit 0; fi
  fi
  # gvfs uses `gio mount -u smb://...`; ask the caller to specify the URI
  # for gvfs unmount instead.
  if command -v gio >/dev/null 2>&1; then
    # Best-effort: iterate active gvfs mounts and unmount matches.
    while IFS= read -r line; do
      case "$line" in
        smb://*) gio mount -u "$line" >/dev/null 2>&1 && exit 0 ;;
      esac
    done < <(gio mount --list 2>/dev/null | awk '/Mount\(/ {print $NF}')
  fi
  die 5 "unmount failed: $mp"
fi

# --- positional args -------------------------------------------------------
[ "${#positional[@]}" -eq 2 ] || { usage; die 1 "expected //HOST/SHARE MOUNTPOINT"; }
unc="${positional[0]}"
mp="${positional[1]}"

case "$unc" in
  //*/*) ;;
  *) die 1 "expected UNC //HOST/SHARE, got: $unc" ;;
esac
[ -d "$mp" ] || die 1 "mountpoint not a directory: $mp"

# Resolve credentials path.
if [ -z "$creds" ]; then
  runtime_dir="${XDG_RUNTIME_DIR:-/tmp/nostr-smb-$(id -u)}"
  creds="$runtime_dir/nostr-smb/credentials"
fi

# --- optional acquire step -------------------------------------------------
if [ "$acquire" = "1" ]; then
  acquire_bin="${NOSTR_SMB_ACQUIRE:-}"
  if [ -z "$acquire_bin" ]; then
    if command -v nostr-smb-acquire >/dev/null 2>&1; then
      acquire_bin="$(command -v nostr-smb-acquire)"
    else
      here="$(cd "$(dirname "$0")" && pwd)"
      if [ -x "$here/nostr-smb-acquire" ]; then
        acquire_bin="$here/nostr-smb-acquire"
      fi
    fi
  fi
  [ -n "$acquire_bin" ] && [ -x "$acquire_bin" ] \
    || die 6 "cannot find nostr-smb-acquire (set NOSTR_SMB_ACQUIRE=/path)"
  provider_arg=""
  if [ -n "${NOSTR_SMB_ACQUIRE_PROVIDER:-}" ]; then
    provider_arg="--provider ${NOSTR_SMB_ACQUIRE_PROVIDER}"
  fi
  # shellcheck disable=SC2086
  "$acquire_bin" --file "$creds" $provider_arg $acquire_args \
    || die 6 "nostr-smb-acquire failed (see stderr above)"
fi

# --- credentials sanity ----------------------------------------------------
[ -r "$creds" ] || die 2 "credentials file not readable: $creds"
# Guard against a world-readable creds file — mount.cifs will otherwise
# expose the password to `ps`-style scrapes because it echoes the path.
mode_bits="$(stat -c '%a' "$creds" 2>/dev/null || stat -f '%OLp' "$creds" 2>/dev/null || echo '')"
if [ -n "$mode_bits" ]; then
  case "$mode_bits" in
    600|400) ;;
    *) log "warning: credentials mode $mode_bits is broader than 0600" ;;
  esac
fi

# Pull username/domain out of the credentials file so we can log who we
# are mounting as (never log the password).
cred_user="$(awk -F= '/^username=/{print $2; exit}' "$creds" | tr -d '[:space:]' || true)"
[ -n "$cred_user" ] || die 2 "credentials file missing username= line: $creds"
log "mounting //$(echo "$unc" | sed 's,^//,,;') as $cred_user via $mode"

# --- do the mount ----------------------------------------------------------
case "$mode" in
  cifs)
    command -v mount.cifs >/dev/null 2>&1 \
      || die 3 "mount.cifs not found (install cifs-utils)"
    opts="credentials=$creds,vers=default"
    [ "$ro" = "1" ] && opts="$opts,ro" || opts="$opts,rw"
    [ -n "$uid_opt" ] && opts="$opts,uid=$uid_opt" || opts="$opts,uid=$(id -u)"
    [ -n "$gid_opt" ] && opts="$opts,gid=$gid_opt" || opts="$opts,gid=$(id -g)"
    opts="$opts,forceuid,forcegid,file_mode=0640,dir_mode=0750"
    # mount.cifs requires root (or CAP_SYS_ADMIN / setuid).  Use sudo -n
    # so we fail fast rather than hang on a password prompt in scripts.
    if [ "$(id -u)" != "0" ]; then
      MOUNTER=(sudo -n mount.cifs)
    else
      MOUNTER=(mount.cifs)
    fi
    cmd=("${MOUNTER[@]}" "$unc" "$mp" -o "$opts")
    if [ "$dry_run" = "1" ]; then
      printf '%q ' "${cmd[@]}"; printf '\n'
      exit 0
    fi
    if ! "${cmd[@]}"; then
      die 4 "mount.cifs failed for $unc -> $mp"
    fi
    ;;
  gvfs)
    command -v gio >/dev/null 2>&1 \
      || die 3 "gio not found (install gvfs)"
    # gio can't consume a credentials file directly; it drives an
    # interactive GMountOperation.  We prime it by writing 3 lines to
    # stdin: username, domain, password.  Domain is left blank for
    # standalone Samba.  A missing session bus / gvfs-backends yields a
    # clear diagnostic, mapped to exit code 3.
    if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
      die 3 "no DBUS_SESSION_BUS_ADDRESS — gvfs needs a session bus"
    fi
    # Extract host + share from the UNC and build the smb:// URI.
    host_share="${unc#//}"
    smb_uri="smb://$cred_user@$host_share"
    pw="$(awk -F= '/^password=/{sub(/^password=/,""); print; exit}' "$creds" || true)"
    [ -n "$pw" ] || die 2 "credentials file missing password= line: $creds"
    if [ "$dry_run" = "1" ]; then
      printf 'gio mount %q\n' "$smb_uri"
      exit 0
    fi
    # `gio mount` reads username/domain/password from stdin when the
    # server issues an auth challenge.  We wipe pw from the environment
    # by only piping into gio, and never assigning it to a subshell var
    # visible via /proc/*/environ.
    if ! printf '%s\n\n%s\n' "$cred_user" "$pw" \
         | gio mount "$smb_uri"; then
      die 4 "gio mount failed for $smb_uri"
    fi
    log "note: gvfs mounts appear under \$XDG_RUNTIME_DIR/gvfs/, not at $mp"
    ;;
esac

log "mounted $unc at $mp"
exit 0
