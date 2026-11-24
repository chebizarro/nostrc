"""Shared named-quarantine rules for the CTest runners."""
import re
import sys
from pathlib import Path


REGISTRY = "scripts/grotto-ci-quarantine.tsv"
SANITIZER_REPORT = re.compile(
    r"(ERROR|SUMMARY|WARNING|FATAL): [A-Za-z]+Sanitizer|: runtime error: |[A-Za-z]+Sanitizer:DEADLYSIGNAL"
)
GTK_ASSERTION = "Gtk-FATAL-CRITICAL: gtk_box_remove: assertion 'GTK_IS_BOX (box)' failed"
GTK_FAILURE = f"not ok /signer/layout/backup-file-keyboard - {GTK_ASSERTION}"


def registry(path, platform):
    names = {}
    for line in path.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        key, name, bead = line.split("\t")
        if key == platform:
            if name in names or not re.fullmatch(r"nostrc-[a-z0-9.]+", bead):
                raise ValueError(f"invalid named quarantine entry: {line}")
            names[name] = bead
    return names


def matching_bead(names, name, status, output):
    bead = names.get(name)
    if (not bead or not status or status in {"Passed", "not run"}
            or "Not Run" in status or "Skipped" in status or SANITIZER_REPORT.search(output)):
        return None
    # CTest reports the known GTK #8534 crash differently across runners.
    # Require the exact TAP subtest and assertion, not just an abort or a signal.
    if bead == "nostrc-hmrgk" and (
        status not in {"SIGTRAP", "Subprocess aborted"} or GTK_FAILURE not in output.splitlines()
    ):
        return None
    return bead


if __name__ == "__main__":
    path, platform, name, status = sys.argv[1:]
    print(matching_bead(registry(Path(path), platform), name, status, sys.stdin.read()) or "")
