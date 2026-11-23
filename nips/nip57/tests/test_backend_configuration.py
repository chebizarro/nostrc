#!/usr/bin/env python3
"""Exercise the root build's storage and decoder configure contract."""
import argparse
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build-root", type=Path, required=True)
args = parser.parse_args()
source = Path(__file__).resolve().parents[3]
common = ["-DBUILD_TESTING=OFF", "-DBUILD_TESTING_FRAMEWORK=OFF",
          "-DBUILD_APPS=OFF", "-DBUILD_RELAYD=OFF", "-DBUILD_FIPS=OFF",
          "-DBUILD_LIBHANAMI=OFF", "-DBUILD_LIBMARMOT=OFF",
          "-DBUILD_MARMOT_GOBJECT=OFF", "-DBUILD_NOSTR_GOBJECT=OFF",
          "-DBUILD_NOSTR_GTK=OFF", "-DSIGNET_ENABLE=OFF",
          "-DNIP55L_SECRET_BACKEND=libsecret"]
cases = [
    ("groundhog-no-storage", ["-DBUILD_GROUNDHOG=ON", "-DWITH_NOSTRDB=OFF",
                              "-DLIBNOSTR_WITH_NOSTRDB=ON"],
     "Groundhog qualification requires"),
    ("groundhog-no-backend", ["-DBUILD_GROUNDHOG=ON", "-DWITH_NOSTRDB=ON",
                              "-DLIBNOSTR_WITH_NOSTRDB=OFF"],
     "Groundhog qualification requires"),
    ("nip57-no-decoder", ["-DBUILD_GROUNDHOG=OFF", "-DWITH_NOSTRDB=OFF",
                          "-DLIBNOSTR_WITH_NOSTRDB=OFF", "-DENABLE_NIP57=ON"],
     "NIP-57 requires LIBNOSTR_WITH_NOSTRDB=ON"),
    ("core-no-backend", ["-DBUILD_GROUNDHOG=OFF", "-DWITH_NOSTRDB=OFF",
                         "-DLIBNOSTR_WITH_NOSTRDB=OFF"], None),
]
args.build_root.mkdir(parents=True, exist_ok=True)
for name, flags, diagnostic in cases:
    build = args.build_root / name
    if build.exists():
        raise SystemExit(f"refusing to overwrite {build}")
    try:
        result = subprocess.run(["cmake", "-S", str(source), "-B", str(build),
                                 *common, *flags], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if diagnostic is None:
            if result.returncode != 0:
                raise SystemExit(f"{name}: configure unexpectedly failed\n{result.stdout}")
            cache = (build / "CMakeCache.txt").read_text().splitlines()
            if "ENABLE_NIP57:BOOL=OFF" not in cache:
                raise SystemExit(f"{name}: expected NIP-57 to default OFF")
            print(f"{name}: PASS (configured with NIP-57 OFF by default)")
        else:
            if result.returncode == 0 or diagnostic not in result.stdout:
                raise SystemExit(f"{name}: wrong configure result\n{result.stdout}")
            print(f"{name}: PASS (rejected with required diagnostic)")
    finally:
        shutil.rmtree(build, ignore_errors=True)
