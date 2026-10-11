#!/usr/bin/env bash
# Turn an AppDir holding one executable into an AppImage with linuxdeploy
# and appimagetool, for the runner's architecture (x86_64 or aarch64).
#
#   scripts/ci/make-appimage.sh APPDIR EXECUTABLE DESKTOP_FILE OUTPUT [--gtk] [-- LINUXDEPLOY_ARGS...]
#
# EXECUTABLE and DESKTOP_FILE are paths inside APPDIR. --gtk adds the
# linuxdeploy GTK plugin. Libraries under APPDIR/usr/lib resolve first.
set -euo pipefail

appdir=$(cd "${1:?appdir}" && pwd) exe=${2:?executable} desktop=${3:?desktop} output=${4:?output}
shift 4
gtk=0
if [[ ${1:-} == --gtk ]]; then gtk=1; shift; fi
[[ ${1:-} == -- ]] && shift

arch=$(uname -m)
tools=${RUNNER_TEMP:-/tmp}/appimage-tools
mkdir -p "$tools"
fetch() { [ -x "$tools/$1" ] || { wget -q -O "$tools/$1" "$2"; chmod +x "$tools/$1"; }; }
fetch "linuxdeploy-$arch.AppImage" \
  "https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-$arch.AppImage"
fetch "appimagetool-$arch.AppImage" \
  "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$arch.AppImage"
plugin=()
if [ "$gtk" = 1 ]; then
  fetch linuxdeploy-plugin-gtk.sh \
    https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh
  plugin=(--plugin gtk)
  export PATH="$tools:$PATH"
fi

export APPIMAGE_EXTRACT_AND_RUN=1 NO_APPSTREAM=1
export LD_LIBRARY_PATH="$appdir/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$tools/linuxdeploy-$arch.AppImage" --appdir "$appdir" \
  --desktop-file "$appdir/$desktop" --executable "$appdir/$exe" "${plugin[@]}" "$@"
ldd "$appdir/$exe" | tee "$tools/ldd.log"
if grep -Fq 'not found' "$tools/ldd.log"; then
  echo "make-appimage: unresolved shared library in $exe" >&2; exit 1
fi
mkdir -p "$(dirname "$output")"
"$tools/appimagetool-$arch.AppImage" "$appdir" "$output"
