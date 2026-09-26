#!/usr/bin/env bash
#
# nostr-smb-browse — enumerate enrolled SMB shares and (optionally)
# install GVfs bookmarks so they appear under "Other Locations" in
# GNOME Files.
#
# Plan §4.1 A2 — discovery hook.  This is NOT a network-neighborhood
# browser: unauthenticated broadcast browsing is exactly the surface
# `net view`/nmblookup expose and we do not want that on a login-time
# path.  Sources are strictly the enrolled `/etc/nostr-auth/servers.d/`
# config files (same format the `--batch` mode in nostr-smb-mount
# consumes).
#
# Modes:
#   nostr-smb-browse                  # list enrolled shares (one per line)
#   nostr-smb-browse --json           # machine-readable listing
#   nostr-smb-browse --install-bookmarks
#                                     # append missing entries to
#                                     # ~/.config/gtk-3.0/bookmarks and
#                                     # ~/.config/gtk-4.0/bookmarks
#   nostr-smb-browse --autostart      # silent if no enrolled servers
#                                     # (used by the .desktop autostart)
#
# Exit codes:
#     0  success (including "no enrolled servers" under --autostart)
#     1  usage / bad arguments
#     2  no enrolled servers under --list mode
#     3  bookmark file unwritable
#
set -euo pipefail

progname="nostr-smb-browse"
usage() {
  cat >&2 <<USAGE
Usage: $progname [--list|--json|--install-bookmarks|--autostart]
                 [--batch-dir DIR]

List Nostr-enrolled SMB shares and optionally install GVfs bookmarks.

Options:
  --list                (default) Print one smb:// URI per line.
  --json                Print a compact JSON array of {unc, label, uri}.
  --install-bookmarks   Append missing entries to GNOME Files bookmarks.
  --autostart           Same as --install-bookmarks but silent if the
                        enrollment set is empty (no servers.d, no *.conf).
  --batch-dir DIR       Directory to scan (default: /etc/nostr-auth/servers.d).
  -h, --help            Show this help.
USAGE
}

log() { printf '%s: %s\n' "$progname" "$*" >&2; }
die() { local code="$1"; shift; log "$*"; exit "$code"; }

mode="list"
batch_dir="/etc/nostr-auth/servers.d"
while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --list) mode="list"; shift ;;
    --json) mode="json"; shift ;;
    --install-bookmarks) mode="bookmarks"; shift ;;
    --autostart) mode="autostart"; shift ;;
    --batch-dir) batch_dir="${2:-}"; shift 2 ;;
    *) usage; die 1 "unknown option: $1" ;;
  esac
done

# Enumerate stanzas → emit one "unc<TAB>mp<TAB>mode<TAB>label" line per
# valid entry.  Anything the parser rejects is logged (never on stdout,
# except under --autostart where all diagnostics are silenced).
enumerate() {
  shopt -s nullglob
  local confs=("$batch_dir"/*.conf)
  shopt -u nullglob
  [ "${#confs[@]}" -eq 0 ] && return 0
  local conf label unc mp confmode line kv key val
  for conf in "${confs[@]}"; do
    [ -r "$conf" ] || continue
    unc=""; mp=""; confmode="cifs"; label=""
    while IFS= read -r line || [ -n "$line" ]; do
      line="${line%$'\r'}"
      line="${line#"${line%%[![:space:]]*}"}"
      line="${line%"${line##*[![:space:]]}"}"
      [ -z "$line" ] && continue
      case "$line" in \#*) continue ;; esac
      case "$line" in
        *=*) key="${line%%=*}"; val="${line#*=}" ;;
        *) continue ;;
      esac
      key="${key%"${key##*[![:space:]]}"}"
      val="${val#"${val%%[![:space:]]*}"}"
      val="${val%"${val##*[![:space:]]}"}"
      case "$val" in
        \"*\") val="${val#\"}"; val="${val%\"}" ;;
        \'*\') val="${val#\'}"; val="${val%\'}" ;;
      esac
      case "$key" in
        unc) unc="$val" ;;
        mountpoint) mp="$val" ;;
        mode) confmode="$val" ;;
        label) label="$val" ;;
      esac
    done < "$conf"
    [ -z "$unc" ] && continue
    if [ -z "$label" ]; then
      # Default label = filename without extension.
      label="$(basename "$conf" .conf)"
    fi
    printf '%s\t%s\t%s\t%s\n' "$unc" "$mp" "$confmode" "$label"
  done
}

# Build the smb:// URI a share should appear as in Files.  We do NOT
# embed a username — the desktop-side mount helper handles login;
# bookmarks are just navigation aids.
build_uri() {
  local unc="$1"
  # unc is guaranteed to start with //HOST/SHARE by the batch parser
  # accepting only that form; be defensive anyway.
  case "$unc" in
    //*/*) printf 'smb:%s\n' "$unc" ;;
    *) printf '%s\n' "$unc" ;;
  esac
}

json_escape() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  printf '%s' "$s"
}

emit_list() {
  local unc mp confmode label uri
  while IFS=$'\t' read -r unc mp confmode label; do
    uri="$(build_uri "$unc")"
    printf '%s\n' "$uri"
  done
}

emit_json() {
  local first=1
  local unc mp confmode label uri
  printf '['
  while IFS=$'\t' read -r unc mp confmode label; do
    uri="$(build_uri "$unc")"
    if [ "$first" = "1" ]; then
      first=0
    else
      printf ','
    fi
    printf '{"unc":"%s","label":"%s","uri":"%s","mode":"%s"}' \
      "$(json_escape "$unc")" "$(json_escape "$label")" \
      "$(json_escape "$uri")" "$(json_escape "$confmode")"
  done
  printf ']\n'
}

# GNOME Files reads bookmarks from ~/.config/gtk-3.0/bookmarks and
# ~/.config/gtk-4.0/bookmarks — one URI per line, an optional label
# separated by whitespace.  We install into BOTH so the "Other
# Locations" sidebar picks the enrollment up on both Nautilus versions
# a distro might ship.
install_bookmarks() {
  local cfg="${XDG_CONFIG_HOME:-$HOME/.config}"
  local files=("$cfg/gtk-3.0/bookmarks" "$cfg/gtk-4.0/bookmarks")
  local written=0
  local unc mp confmode label uri
  # Snapshot the enumeration once — we iterate it per bookmark file.
  local snapshot
  snapshot="$(enumerate | while IFS=$'\t' read -r unc mp confmode label; do
    printf '%s\t%s\n' "$(build_uri "$unc")" "$label"
  done)"
  [ -z "$snapshot" ] && return 0
  local bmfile
  for bmfile in "${files[@]}"; do
    mkdir -p "$(dirname "$bmfile")" 2>/dev/null || {
      log "cannot create $(dirname "$bmfile")"
      continue
    }
    [ -e "$bmfile" ] || : > "$bmfile"
    if [ ! -w "$bmfile" ]; then
      log "bookmark file not writable: $bmfile"
      continue
    fi
    # Append entries whose URI is not already present.  Preserving
    # existing user bookmarks and ordering is important — this file is
    # user-owned and rewriting it wholesale would clobber their picks.
    while IFS=$'\t' read -r uri label; do
      [ -z "$uri" ] && continue
      if ! grep -Fxq "$uri" "$bmfile" 2>/dev/null \
         && ! grep -Fq "$uri " "$bmfile" 2>/dev/null; then
        printf '%s %s\n' "$uri" "$label" >> "$bmfile"
        written=$((written + 1))
      fi
    done <<< "$snapshot"
  done
  [ "$written" -gt 0 ] && log "installed $written bookmark(s)"
  return 0
}

# ---------------------------------------------------------------------
listing="$(enumerate)"
case "$mode" in
  list)
    if [ -z "$listing" ]; then
      log "no enrolled servers in $batch_dir"
      exit 2
    fi
    printf '%s\n' "$listing" | emit_list
    ;;
  json)
    if [ -z "$listing" ]; then
      printf '[]\n'
      exit 0
    fi
    printf '%s\n' "$listing" | emit_json
    ;;
  bookmarks|autostart)
    if [ -z "$listing" ]; then
      [ "$mode" = "autostart" ] || log "no enrolled servers in $batch_dir"
      exit 0
    fi
    install_bookmarks
    ;;
esac
exit 0
