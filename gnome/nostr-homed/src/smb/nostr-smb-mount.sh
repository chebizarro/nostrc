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
# Batch and teardown modes (plan §4.1 A1, A4):
#
#   `--batch` reads every `/etc/nostr-auth/servers.d/*.conf` file (or a
#   caller-supplied `--batch-dir DIR`) and mounts each declared share
#   sequentially. Per-share failures do NOT abort the batch; they set
#   bits in an exit bitmap (see "Exit codes" below) so a caller can tell
#   "everything mounted", "at least one refused credential", "at least
#   one config was unparseable" apart without parsing the log stream.
#
#   Every successful `--mode gvfs` mount (including from --batch) is
#   recorded in a per-user mount ledger at
#   $XDG_STATE_HOME/nostr-smb/mounts.json (created 0600). The ledger is
#   line-delimited JSON — one entry per mount, keyed by the current
#   XDG_SESSION_ID — so concurrent sessions can be torn down
#   independently. `--unmount --all` iterates the ledger and unmounts
#   every entry that matches the current session id; a lone `--unmount
#   MOUNTPOINT-OR-URI` keeps its original single-target behaviour.
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
# Batch exit bitmap (return code with --batch or --unmount --all):
#     bit 0 (0x01) — at least one entry failed to mount / unmount
#     bit 1 (0x02) — at least one servers.d config was unparseable
#     bit 2 (0x04) — at least one credentials file was missing/unreadable
#     bit 3 (0x08) — mount backend unavailable for at least one entry
#     bit 4 (0x10) — no entries were processed (empty servers.d)
#   0 iff every entry mounted (or unmounted) cleanly.
#
set -euo pipefail

progname="nostr-smb-mount"
usage() {
  cat >&2 <<USAGE
Usage: $progname [options] //HOST/SHARE MOUNTPOINT
       $progname --unmount MOUNTPOINT
       $progname --unmount --all
       $progname --batch [--batch-dir DIR]

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
  --unmount             Unmount MOUNTPOINT and exit (or use --all)
  --all                 With --unmount: unmount every ledger entry for
                        this session (\$XDG_SESSION_ID scope)
  --batch               Mount every share declared under
                        /etc/nostr-auth/servers.d/*.conf.
                        Overrideable with --batch-dir.
  --batch-dir DIR       Directory to scan for *.conf in --batch mode
                        (default: /etc/nostr-auth/servers.d).
  --dry-run             Print the mount command that would run and exit
  -h, --help            Show this help

Environment:
  NOSTR_SMB_ACQUIRE     Path to nostr-smb-acquire binary
                        (default: search PATH, then nostr-smb-acquire in
                        the same directory as this script)
  NOSTR_SMB_LEDGER      Path to the mount ledger
                        (default: \$XDG_STATE_HOME/nostr-smb/mounts.json,
                        falling back to \$HOME/.local/state/nostr-smb/mounts.json)
USAGE
}

log() { printf '%s: %s\n' "$progname" "$*" >&2; }
die() { local code="$1"; shift; log "$*"; exit "$code"; }

# --- ledger helpers --------------------------------------------------------
#
# The ledger is line-delimited JSON — one compact record per line — so
# entries can be appended atomically with a single write() and consumed
# without a full JSON parser (we own both writer and reader).  Each
# record has the shape:
#     {"sid":"<session>","mode":"gvfs|cifs","target":"<uri-or-mp>",
#      "unc":"<//host/share>","user":"<username>","ts":<unix-seconds>}
# The `mode` and `target` fields are what the teardown path uses;
# everything else is diagnostic.
ledger_path() {
  if [ -n "${NOSTR_SMB_LEDGER:-}" ]; then
    printf '%s\n' "$NOSTR_SMB_LEDGER"; return 0
  fi
  local state="${XDG_STATE_HOME:-${HOME:-/tmp}/.local/state}"
  printf '%s\n' "$state/nostr-smb/mounts.json"
}

ledger_session_id() {
  # XDG_SESSION_ID is set by pam_systemd for graphical logins.  For
  # scripted/CI use we fall back to the UID so the ledger still
  # segregates concurrent shells.
  if [ -n "${XDG_SESSION_ID:-}" ]; then
    printf '%s\n' "$XDG_SESSION_ID"
  else
    printf 'uid-%s\n' "$(id -u)"
  fi
}

# JSON-escape a value for a ledger field.  Handles the characters that
# actually appear in mount metadata (backslash, double-quote); we do not
# ship arbitrary user text through here.
json_escape() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  printf '%s' "$s"
}

ledger_append() {
  # $1=mode $2=target $3=unc $4=user
  local mode="$1" target="$2" unc="$3" user="$4"
  local path
  path="$(ledger_path)"
  local dir
  dir="$(dirname "$path")"
  mkdir -p "$dir" 2>/dev/null || return 0  # best-effort; do not fail the mount
  chmod 700 "$dir" 2>/dev/null || true
  local sid ts
  sid="$(ledger_session_id)"
  ts="$(date -u +%s 2>/dev/null || echo 0)"
  local line
  line=$(printf '{"sid":"%s","mode":"%s","target":"%s","unc":"%s","user":"%s","ts":%s}\n' \
    "$(json_escape "$sid")" "$(json_escape "$mode")" \
    "$(json_escape "$target")" "$(json_escape "$unc")" \
    "$(json_escape "$user")" "$ts")
  # Create the file with 0600 on first write.
  if [ ! -e "$path" ]; then
    (umask 077 && : > "$path") 2>/dev/null || return 0
  fi
  printf '%s' "$line" >> "$path" 2>/dev/null || true
}

ledger_remove_matching() {
  # $1=sid $2=target — remove exact (sid,target) lines from the ledger.
  local sid="$1" target="$2"
  local path
  path="$(ledger_path)"
  [ -f "$path" ] || return 0
  local pat_sid pat_tgt
  pat_sid="$(json_escape "$sid")"
  pat_tgt="$(json_escape "$target")"
  # Use awk with fixed-string comparison to avoid regex surprises.
  local tmp
  tmp="$(mktemp "$path.XXXXXX" 2>/dev/null)" || return 0
  awk -v sid="\"sid\":\"$pat_sid\"" -v tgt="\"target\":\"$pat_tgt\"" \
    '{ if (index($0, sid) > 0 && index($0, tgt) > 0) next; print }' \
    "$path" > "$tmp" 2>/dev/null || { rm -f "$tmp"; return 0; }
  chmod 600 "$tmp" 2>/dev/null || true
  mv "$tmp" "$path" 2>/dev/null || rm -f "$tmp"
}

# Emit the target field of every ledger entry matching the current
# session id.  One target per output line.
ledger_targets_for_session() {
  local sid
  sid="$(ledger_session_id)"
  local path
  path="$(ledger_path)"
  [ -f "$path" ] || return 0
  local pat_sid
  pat_sid="$(json_escape "$sid")"
  awk -v sid="\"sid\":\"$pat_sid\"" '
    index($0, sid) > 0 {
      # Extract the "target":"..." field.  Values only contain JSON-escaped
      # characters, and we own the writer, so a linear scan is safe.
      s = $0
      p = index(s, "\"target\":\"")
      if (p == 0) next
      s = substr(s, p + 10)
      # Find the closing quote, honouring backslash-escapes.
      out = ""
      i = 1
      while (i <= length(s)) {
        c = substr(s, i, 1)
        if (c == "\\" && i < length(s)) {
          out = out substr(s, i + 1, 1)
          i += 2
          continue
        }
        if (c == "\"") break
        out = out c
        i += 1
      }
      if (out != "") print out
    }
  ' "$path"
}

# --- argument parsing -------------------------------------------------------
mode=cifs
creds=""
acquire=0
acquire_args=""
uid_opt=""
gid_opt=""
ro=0
unmount=0
unmount_all=0
batch=0
batch_dir="/etc/nostr-auth/servers.d"
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
    --all) unmount_all=1; shift ;;
    --batch) batch=1; shift ;;
    --batch-dir) batch_dir="${2:-}"; shift 2 ;;
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

# --- servers.d config parser ----------------------------------------------
#
# The batch config is deliberately a strict `key = value` line format
# (not a shell fragment).  Recognised keys per stanza:
#
#     unc          //host/share                   (required)
#     mountpoint   /path                          (required for --mode cifs)
#     mode         cifs|gvfs                      (default: cifs)
#     creds        /path/to/credentials           (default: acquire per-share)
#     acquire      yes|no                         (default: yes)
#     acquire_args extra args passed to acquire   (default: empty)
#     provider     acquire provider hint          (default: env inherit)
#     ro           yes|no                         (default: no)
#     uid          numeric uid= override
#     gid          numeric gid= override
#
# Each *.conf may declare a single share stanza (no INI [sections]);
# multiple shares belong in multiple files.  Blank lines and lines
# beginning with # are ignored.  Unknown keys are a hard parse error so
# a typo does not silently drop a share.
parse_server_conf() {
  local conf="$1"
  # Emit key=value lines on stdout; caller consumes with a while-read
  # loop.  Prints ERROR: <detail> on parse failure and returns non-zero.
  local line lineno=0 key val
  while IFS= read -r line || [ -n "$line" ]; do
    lineno=$((lineno + 1))
    # Strip CR (in case of CRLF), leading/trailing whitespace.
    line="${line%$'\r'}"
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    [ -z "$line" ] && continue
    case "$line" in \#*) continue ;; esac
    case "$line" in
      *=*) key="${line%%=*}"; val="${line#*=}" ;;
      *) printf 'ERROR: %s:%d: expected key=value\n' "$conf" "$lineno"; return 1 ;;
    esac
    key="${key%"${key##*[![:space:]]}"}"
    val="${val#"${val%%[![:space:]]*}"}"
    val="${val%"${val##*[![:space:]]}"}"
    # Strip matching quotes if present.
    case "$val" in
      \"*\") val="${val#\"}"; val="${val%\"}" ;;
      \'*\') val="${val#\'}"; val="${val%\'}" ;;
    esac
    case "$key" in
      unc|mountpoint|mode|creds|acquire|acquire_args|provider|ro|uid|gid) ;;
      # `label` is consumed by nostr-smb-browse (plan §4.1 A2) — the
      # mount helper ignores it, but MUST accept it so a single
      # servers.d stanza can drive both tools without a parse error.
      label) continue ;;
      *) printf 'ERROR: %s:%d: unknown key: %s\n' "$conf" "$lineno" "$key"; return 1 ;;
    esac
    printf '%s=%s\n' "$key" "$val"
  done < "$conf"
  return 0
}

# --- unmount branch --------------------------------------------------------
if [ "$unmount" = "1" ]; then
  if [ "$unmount_all" = "1" ]; then
    # Ledger-driven teardown.  Iterate every entry for this session and
    # unmount it, aggregating failures into the exit bitmap.  Missing
    # ledger or empty session is a no-op with exit 0.
    [ "${#positional[@]}" -eq 0 ] || die 1 "--unmount --all takes no positional args"
    rc=0
    count=0
    fail_count=0
    sid="$(ledger_session_id)"
    while IFS= read -r tgt; do
      [ -z "$tgt" ] && continue
      count=$((count + 1))
      case "$tgt" in
        smb://*)
          # gvfs teardown.  If no session bus, mark degraded but do NOT
          # block logout — pam_sm_close_session bounds us with a timeout.
          if command -v gio >/dev/null 2>&1 && [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
            if gio mount -u "$tgt" >/dev/null 2>&1; then
              ledger_remove_matching "$sid" "$tgt"
            else
              log "warning: gio mount -u failed for $tgt"
              rc=$(( rc | 0x01 ))
              fail_count=$((fail_count + 1))
            fi
          else
            log "warning: gvfs unavailable; leaving $tgt registered"
            rc=$(( rc | 0x08 ))
            fail_count=$((fail_count + 1))
          fi
          ;;
        *)
          # cifs mountpoint; best-effort umount.  On a hung mount we do
          # not spin — a single umount attempt, then move on.
          if command -v mountpoint >/dev/null 2>&1 && mountpoint -q "$tgt"; then
            if umount "$tgt" >/dev/null 2>&1 \
                 || (command -v sudo >/dev/null 2>&1 && sudo -n umount "$tgt" >/dev/null 2>&1); then
              ledger_remove_matching "$sid" "$tgt"
            else
              log "warning: umount failed for $tgt"
              rc=$(( rc | 0x01 ))
              fail_count=$((fail_count + 1))
            fi
          else
            # Nothing mounted here; drop the stale entry so the ledger
            # does not grow without bound.
            ledger_remove_matching "$sid" "$tgt"
          fi
          ;;
      esac
    done < <(ledger_targets_for_session)
    if [ "$count" = "0" ]; then
      log "no ledger entries for session $sid"
      exit 0
    fi
    log "unmount --all: $((count - fail_count))/$count ok (session $sid)"
    exit "$rc"
  fi

  [ "${#positional[@]}" -eq 1 ] || { usage; die 1 "--unmount takes exactly one argument (mountpoint-or-uri) or use --all"; }
  target="${positional[0]}"
  # If the caller passed an smb:// URI, this is unambiguously a gvfs unmount.
  case "$target" in
    smb://*)
      command -v gio >/dev/null 2>&1 || die 5 "gio not found; cannot unmount $target"
      [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ] \
        || die 3 "no DBUS_SESSION_BUS_ADDRESS — gvfs unmount needs a session bus"
      if gio mount -u "$target" >/dev/null 2>&1; then
        ledger_remove_matching "$(ledger_session_id)" "$target"
        exit 0
      fi
      die 5 "gio mount -u failed for $target"
      ;;
  esac
  mp="$target"
  # cifs path: MP is a real filesystem mountpoint we can umount(2).
  if command -v mountpoint >/dev/null 2>&1 && mountpoint -q "$mp"; then
    if umount "$mp" >/dev/null 2>&1; then
      ledger_remove_matching "$(ledger_session_id)" "$mp"
      exit 0
    fi
    if command -v sudo >/dev/null 2>&1 && sudo -n umount "$mp" >/dev/null 2>&1; then
      ledger_remove_matching "$(ledger_session_id)" "$mp"
      exit 0
    fi
    die 5 "umount failed: $mp"
  fi
  # Not a cifs mountpoint — try gvfs.  Iterate active smb:// mounts and
  # unmount every one whose fuse leaf corresponds to a mount this helper
  # could have created (server+share match if MP was passed as UNC-like
  # //HOST/SHARE; otherwise unmount all smb:// as a best-effort).
  if command -v gio >/dev/null 2>&1 && [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    match=""
    case "$mp" in
      //*/*) match="${mp#//}" ;;  # host/share
    esac
    unmounted=0
    while IFS= read -r line; do
      case "$line" in
        smb://*)
          if [ -z "$match" ] || [ "${line#smb://}" != "${line#smb://*"$match"*}" ]; then
            if gio mount -u "$line" >/dev/null 2>&1; then
              unmounted=$((unmounted+1))
              ledger_remove_matching "$(ledger_session_id)" "$line"
            fi
          fi
          ;;
      esac
    done < <(gio mount --list 2>/dev/null | awk '/-> smb:\/\// {print $NF}')
    [ "$unmounted" -gt 0 ] && exit 0
  fi
  # Nothing was a mountpoint and nothing matched under gvfs — no-op.
  log "not a mountpoint and no matching gvfs mount: $mp"
  exit 0
fi

# --- batch branch ----------------------------------------------------------
#
# Iterate `${batch_dir}/*.conf` in sorted order, mount each declared
# share, aggregate failures into a bitmap (see "Batch exit bitmap"
# header comment).  We re-exec THIS script for each entry rather than
# recursing in-process so a per-share segfault / -euo trip cannot
# poison the batch loop.
if [ "$batch" = "1" ]; then
  [ "${#positional[@]}" -eq 0 ] || die 1 "--batch takes no positional args"
  self="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
  [ -x "$self" ] || self="$0"  # tolerate a non-exec install path
  shopt -s nullglob
  confs=("$batch_dir"/*.conf)
  shopt -u nullglob
  if [ "${#confs[@]}" -eq 0 ]; then
    log "batch: no *.conf in $batch_dir"
    exit 16  # 0x10 — nothing to do
  fi
  rc=0
  ok_count=0
  total=0
  for conf in "${confs[@]}"; do
    total=$((total + 1))
    [ -r "$conf" ] || { log "$conf: unreadable"; rc=$(( rc | 0x02 )); continue; }
    # Parse into plain scalars — avoid `declare -A` so the helper
    # runs unmodified on any bash 3.2+ (macOS still ships 3.2; some
    # CI paths reach into that).  parse_server_conf emits either
    # "key=value" lines or an "ERROR:..." diagnostic on parse failure.
    entry_unc=""
    entry_mp=""
    entry_mode="cifs"
    entry_creds=""
    entry_acquire="yes"
    entry_aargs=""
    entry_provider=""
    entry_ro="no"
    entry_uid=""
    entry_gid=""
    parse_out="$(parse_server_conf "$conf")"
    parse_err=0
    while IFS= read -r kv; do
      [ -z "$kv" ] && continue
      case "$kv" in
        ERROR:*) log "${kv#ERROR: }"; parse_err=1 ;;
        *=*)
          case "${kv%%=*}" in
            unc)          entry_unc="${kv#*=}" ;;
            mountpoint)   entry_mp="${kv#*=}" ;;
            mode)         entry_mode="${kv#*=}" ;;
            creds)        entry_creds="${kv#*=}" ;;
            acquire)      entry_acquire="${kv#*=}" ;;
            acquire_args) entry_aargs="${kv#*=}" ;;
            provider)     entry_provider="${kv#*=}" ;;
            ro)           entry_ro="${kv#*=}" ;;
            uid)          entry_uid="${kv#*=}" ;;
            gid)          entry_gid="${kv#*=}" ;;
          esac
          ;;
      esac
    done <<< "$parse_out"
    if [ "$parse_err" = "1" ]; then
      rc=$(( rc | 0x02 ))
      continue
    fi
    if [ -z "$entry_unc" ]; then
      log "$conf: missing required key: unc"
      rc=$(( rc | 0x02 )); continue
    fi
    if [ "$entry_mode" = "cifs" ] && [ -z "$entry_mp" ]; then
      log "$conf: missing required key for cifs: mountpoint"
      rc=$(( rc | 0x02 )); continue
    fi
    # Build the argv for a single-share invocation of ourselves.
    args=()
    args+=("--mode" "$entry_mode")
    [ "$entry_ro" = "yes" ] && args+=("--ro")
    [ -n "$entry_creds" ] && args+=("--creds" "$entry_creds")
    [ -n "$entry_uid" ] && args+=("--uid" "$entry_uid")
    [ -n "$entry_gid" ] && args+=("--gid" "$entry_gid")
    if [ "$entry_acquire" = "yes" ]; then
      args+=("--acquire")
      [ -n "$entry_aargs" ] && args+=("--acquire-args" "$entry_aargs")
    fi
    # For gvfs entries mountpoint is informational; pass a placeholder
    # to satisfy positional-count validation without confusing users.
    entry_mp_arg="${entry_mp:-.}"
    args+=("$entry_unc" "$entry_mp_arg")
    log "batch: $conf -> $entry_unc via $entry_mode"
    # Build the env prefix for this stanza.  Empty-array expansion
    # trips `set -u`, so branch on whether provider was set.
    if [ -n "$entry_provider" ]; then
      env_prefix=(env "NOSTR_SMB_ACQUIRE_PROVIDER=$entry_provider")
    else
      env_prefix=(env)
    fi
    if "${env_prefix[@]}" "$self" "${args[@]}"; then
      ok_count=$((ok_count + 1))
    else
      sub_rc=$?
      case "$sub_rc" in
        2) rc=$(( rc | 0x04 )); log "$conf: credentials issue" ;;
        3) rc=$(( rc | 0x08 )); log "$conf: mount backend unavailable" ;;
        *) rc=$(( rc | 0x01 )); log "$conf: mount failed (exit $sub_rc)" ;;
      esac
    fi
  done
  log "batch: $ok_count/$total ok"
  exit "$rc"
fi

# --- positional args -------------------------------------------------------
[ "${#positional[@]}" -eq 2 ] || { usage; die 1 "expected //HOST/SHARE MOUNTPOINT"; }
unc="${positional[0]}"
mp="${positional[1]}"

case "$unc" in
  //*/*) ;;
  *) die 1 "expected UNC //HOST/SHARE, got: $unc" ;;
esac
# For gvfs, MP is informational and may be a non-directory placeholder.
if [ "$mode" = "cifs" ] && [ ! -d "$mp" ]; then
  die 1 "mountpoint not a directory: $mp"
fi

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
    ledger_append "cifs" "$mp" "$unc" "$cred_user"
    ;;
  gvfs)
    command -v gio >/dev/null 2>&1 \
      || die 3 "gio not found (install gvfs)"
    # gio can't consume a credentials file directly; it drives an
    # interactive GMountOperation.  A missing session bus / gvfs
    # backends yields a clear diagnostic, mapped to exit code 3.
    if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
      die 3 "no DBUS_SESSION_BUS_ADDRESS — gvfs needs a session bus (try dbus-run-session --)"
    fi
    # Extract host + share from the UNC and build the smb:// URI.  The
    # username is embedded in the URI so the gvfs SMB backend advertises
    # a default and skips the USER prompt in its ask-password call.  We
    # therefore only need to feed the remaining prompts (Domain and
    # Password, in that order) on stdin.
    host_share="${unc#//}"
    smb_uri="smb://$cred_user@$host_share"
    pw="$(awk -F= '/^password=/{sub(/^password=/,""); print; exit}' "$creds" || true)"
    [ -n "$pw" ] || die 2 "credentials file missing password= line: $creds"
    if [ "$dry_run" = "1" ]; then
      printf 'gio mount %q\n' "$smb_uri"
      exit 0
    fi
    # `gio mount` reads Domain (blank -> WORKGROUP default) then Password
    # from stdin when the server issues an auth challenge.  Feeding just
    # those two lines means that on auth failure the second ask-password
    # loop hits EOF and gio exits non-zero rather than hanging forever
    # on the retry prompt.  We never assign pw to a subshell variable
    # that would show in /proc/*/environ.
    #
    # gvfs mounts land at $XDG_RUNTIME_DIR/gvfs/smb-share:server=...,
    # not at MOUNTPOINT — the MOUNTPOINT arg is retained for parity with
    # the cifs branch but is not the on-disk location for gvfs.
    if ! printf '\n%s\n' "$pw" | gio mount "$smb_uri"; then
      die 4 "gio mount failed for $smb_uri (revoked credential? auth flags mismatch?)"
    fi
    # Post-mount verification: the fuse mirror must be visible.
    fuse_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/gvfs"
    fuse_leaf="smb-share:server=${host_share%%/*},share=${host_share#*/},user=$cred_user"
    if [ -d "$fuse_dir/$fuse_leaf" ]; then
      log "gvfs mount visible at $fuse_dir/$fuse_leaf"
    else
      log "warning: expected gvfs fuse entry not found at $fuse_dir/$fuse_leaf"
    fi
    log "note: gvfs mounts appear under \$XDG_RUNTIME_DIR/gvfs/, not at $mp"
    ledger_append "gvfs" "$smb_uri" "$unc" "$cred_user"
    ;;
esac

log "mounted $unc at $mp"
exit 0
