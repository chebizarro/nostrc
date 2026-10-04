#!/usr/bin/env bash
# Shared path policy for per-user W27 VM helpers (Linux hosts only).

w27_fail() { printf 'w27: %s\n' "$*" >&2; return 1; }

w27_absolute_path() {
  local path=${1-}
  [[ $path == /* && $path != / && "/$path/" != */../* && "/$path/" != */./* ]] ||
    w27_fail "expected an absolute path without dot components: $path"
}

w27_run_file() {
  local path=$1
  [[ $path == "$W27_RUN_PATH/"* ]] || w27_fail "file is outside the run directory: $path" || return 1
  [[ ! -L $path && ( ! -e $path || -f $path ) ]] ||
    w27_fail "refusing a symlink or non-file: $path" || return 1
  if [[ -e $path ]]; then
    [[ $(stat -c %u -- "$path") == "$(id -u)" && $(stat -c %h -- "$path") == 1 ]] ||
      w27_fail "refusing a foreign-owned or hard-linked file: $path"
  fi
}

w27_run_subdir() {
  local path=$1
  [[ $path == "$W27_RUN_PATH/"* ]] || w27_fail "directory is outside the run directory: $path" || return 1
  [[ ! -L $path ]] || w27_fail "refusing a symlink: $path" || return 1
  mkdir -p -- "$path"
  [[ ! -L $path && -d $path && $(stat -c %u -- "$path") == "$(id -u)" ]] ||
    w27_fail "unsafe directory: $path" || return 1
  chmod 700 -- "$path"
}

w27_init_run_paths() {
  [[ ${W27_RUN_ID-} =~ ^[0-9]{8}T[0-9]{6}$ ]] ||
    w27_fail 'W27_RUN_ID must be a single UTC timestamp component (YYYYMMDDTHHMMSS)' || return 1
  [[ ! ${W27_RUN_DIR+x} ]] ||
    w27_fail 'W27_RUN_DIR override is unsupported; runs live only under the account home/gh-run' || return 1

  W27_HOME=$(getent passwd "$(id -u)" | cut -d: -f6) ||
    w27_fail 'cannot resolve account home' || return 1
  w27_absolute_path "$W27_HOME" || return 1
  [[ -d $W27_HOME && ! -L $W27_HOME ]] || w27_fail 'account home is missing or a symlink' || return 1
  W27_HOME=$(realpath -e -- "$W27_HOME") || return 1
  W27_RUN_ROOT="$W27_HOME/gh-run"
  W27_RUN_PATH="$W27_RUN_ROOT/$W27_RUN_ID"
  [[ ! -L $W27_RUN_ROOT && ! -L $W27_RUN_PATH ]] ||
    w27_fail 'refusing a symlinked run root or run directory' || return 1
  umask 077
  mkdir -p -- "$W27_RUN_ROOT" "$W27_RUN_PATH"
  [[ ! -L $W27_RUN_ROOT && ! -L $W27_RUN_PATH &&
     $(realpath -e -- "$W27_RUN_PATH") == "$W27_RUN_ROOT/$W27_RUN_ID" ]] ||
    w27_fail 'run directory escaped the fixed root' || return 1
  [[ $(stat -c %u -- "$W27_RUN_ROOT") == "$(id -u)" &&
     $(stat -c %u -- "$W27_RUN_PATH") == "$(id -u)" ]] ||
    w27_fail 'run root or run directory is not owned by this user' || return 1
  chmod 700 -- "$W27_RUN_ROOT" "$W27_RUN_PATH"
}
