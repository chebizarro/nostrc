#!/usr/bin/env python3
"""Create an annotated component release tag from generated CMake version data."""

from __future__ import annotations

import argparse
import os
import re
import stat
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Mapping

SEMVER_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
PRERELEASE_RE = re.compile(r"^[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*$")
COMPONENT_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
HEADER_VERSION_RE = re.compile(
    r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*_VERSION)\s+"([^"]+)"\s*$'
)
CACHE_ENTRY_RE = re.compile(r"^([^#/:=][^:=]*)(?::[^=]+)?=(.*)$")


class ReleaseTagError(RuntimeError):
    """Raised when release metadata cannot be validated or tagged."""


def normalize_version_key(value: str) -> str:
    key = re.sub(r"[^A-Za-z0-9]+", "_", value).strip("_").upper()
    if not key:
        raise ReleaseTagError("version key cannot be empty")
    if not key.endswith("_VERSION"):
        key += "_VERSION"
    return key


def component_version_keys(component: str) -> list[str]:
    keys = [normalize_version_key(component)]
    if component.lower().startswith("lib") and len(component) > 3:
        without_lib = normalize_version_key(component[3:])
        if without_lib not in keys:
            keys.append(without_lib)
    return keys


def read_header_versions(path: Path) -> dict[str, str]:
    versions: dict[str, str] = {}
    for line in read_text(path).splitlines():
        match = HEADER_VERSION_RE.match(line)
        if match:
            versions[match.group(1)] = match.group(2)
    return versions


def read_cache_versions(path: Path) -> dict[str, str]:
    versions: dict[str, str] = {}
    for line in read_text(path).splitlines():
        match = CACHE_ENTRY_RE.match(line)
        if not match:
            continue
        key, value = match.groups()
        if key.endswith("_VERSION"):
            versions[key] = value.strip()
    return versions


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ReleaseTagError(f"cannot read {path}: {exc}") from exc


def select_version(
    versions: Mapping[str, str],
    component: str,
    version_key: str | None,
    source: Path,
) -> str:
    if version_key:
        keys = [normalize_version_key(version_key)]
    else:
        keys = component_version_keys(component)

    for key in keys:
        if key in versions:
            return validate_version(versions[key], key, source)

    semver_entries = {
        key: value for key, value in versions.items() if SEMVER_RE.fullmatch(value)
    }
    if not version_key and len(semver_entries) == 1:
        key, value = next(iter(semver_entries.items()))
        return validate_version(value, key, source)

    available = ", ".join(sorted(versions)) or "none"
    expected = ", ".join(keys)
    raise ReleaseTagError(
        f"could not find version key {expected} in {source}; "
        f"available version keys: {available}. Use --version-key to select one."
    )


def validate_version(version: str, key: str, source: Path) -> str:
    if not SEMVER_RE.fullmatch(version):
        raise ReleaseTagError(
            f"{key} in {source} is not an X.Y.Z version: {version!r}"
        )
    return version


def git(repo: Path, *args: str) -> subprocess.CompletedProcess[str]:
    command = ["git", "-C", str(repo), *args]
    try:
        return subprocess.run(
            command,
            check=True,
            capture_output=True,
            text=True,
        )
    except FileNotFoundError as exc:
        raise ReleaseTagError("git executable was not found") from exc
    except subprocess.CalledProcessError as exc:
        detail = (exc.stderr or exc.stdout).strip()
        raise ReleaseTagError(
            f"{' '.join(command)} failed" + (f": {detail}" if detail else "")
        ) from exc


def release_tag_name(
    component: str,
    version: str,
    prerelease: str | None = None,
) -> str:
    if not COMPONENT_RE.fullmatch(component):
        raise ReleaseTagError(
            "component must contain only letters, digits, dots, underscores, or hyphens"
        )
    if prerelease is not None:
        invalid_numeric_identifier = any(
            identifier.isdigit()
            and len(identifier) > 1
            and identifier.startswith("0")
            for identifier in prerelease.split(".")
        )
        if not PRERELEASE_RE.fullmatch(prerelease) or invalid_numeric_identifier:
            raise ReleaseTagError(
                "prerelease must be a valid dot-separated SemVer prerelease identifier"
            )
    release_version = version if prerelease is None else f"{version}-{prerelease}"
    return f"{component}-v{release_version}"


def prepare_manifest_update(
    path: Path,
    component: str,
    version: str,
    release_version: str,
    tag: str,
) -> str:
    lines = read_text(path).splitlines(keepends=True)
    for index, line in enumerate(lines):
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue

        cells = [cell.strip() for cell in stripped.strip("|").split("|")]
        if len(cells) < 6 or cells[0] != component:
            continue
        if cells[2] != version:
            raise ReleaseTagError(
                f"{component} version mismatch: {path} declares {cells[2]}, "
                f"but the generated version data contains {version}"
            )

        cells[3] = release_version
        cells[4] = f"`{tag}`"
        newline = "\n" if line.endswith("\n") else ""
        lines[index] = "| " + " | ".join(cells) + " |" + newline
        return "".join(lines)

    raise ReleaseTagError(f"component {component!r} is not listed in {path}")


def write_text_atomic(path: Path, content: str) -> None:
    try:
        mode = stat.S_IMODE(path.stat().st_mode)
        fd, temporary_name = tempfile.mkstemp(
            prefix=f".{path.name}.",
            dir=path.parent,
            text=True,
        )
        temporary_path = Path(temporary_name)
        try:
            with os.fdopen(fd, "w", encoding="utf-8", newline="") as handle:
                handle.write(content)
                handle.flush()
                os.fsync(handle.fileno())
            os.chmod(temporary_path, mode)
            os.replace(temporary_path, path)
        except Exception:
            temporary_path.unlink(missing_ok=True)
            raise
    except OSError as exc:
        raise ReleaseTagError(f"cannot update {path}: {exc}") from exc


def create_release(
    repo: Path,
    manifest: Path,
    component: str,
    version: str,
    prerelease: str | None = None,
) -> str:
    git(repo, "rev-parse", "--is-inside-work-tree")
    release_version = version if prerelease is None else f"{version}-{prerelease}"
    tag = release_tag_name(component, version, prerelease)
    if git(repo, "tag", "--list", tag).stdout.strip():
        raise ReleaseTagError(f"tag already exists: {tag}")

    updated_manifest = prepare_manifest_update(
        manifest, component, version, release_version, tag
    )
    git(repo, "tag", "-a", tag, "-m", f"Release {component} v{release_version}")
    try:
        write_text_atomic(manifest, updated_manifest)
    except ReleaseTagError as update_error:
        try:
            git(repo, "tag", "-d", tag)
        except ReleaseTagError as rollback_error:
            raise ReleaseTagError(
                f"{update_error}; rollback also failed, so tag {tag} still exists: "
                f"{rollback_error}"
            ) from update_error
        raise ReleaseTagError(
            f"{update_error}; removed tag {tag} to keep the release ledger consistent"
        ) from update_error
    return tag


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create <component>-vX.Y.Z[-PRERELEASE] as an annotated Git tag."
    )
    parser.add_argument("component", help="component tag prefix, for example gnostr")
    parser.add_argument(
        "--prerelease",
        help="optional SemVer prerelease identifier, for example preview or rc.1",
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument(
        "--header",
        type=Path,
        help="generated C version header containing a quoted *_VERSION macro",
    )
    source.add_argument(
        "--cache",
        type=Path,
        help="CMakeCache.txt containing a *_VERSION cache entry",
    )
    parser.add_argument(
        "--version-key",
        help="macro/cache key to use when it cannot be inferred from the component",
    )
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path.cwd(),
        help="Git work tree to tag (default: current directory)",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        help="release ledger (default: <repo>/VERSION_MANIFEST.md)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    source = args.header or args.cache

    try:
        if args.header:
            versions = read_header_versions(source)
        else:
            versions = read_cache_versions(source)
        version = select_version(
            versions,
            args.component,
            args.version_key,
            source,
        )
        manifest = args.manifest or args.repo / "VERSION_MANIFEST.md"
        tag = create_release(
            args.repo, manifest, args.component, version, args.prerelease
        )
    except ReleaseTagError as exc:
        print(f"tag_release.py: error: {exc}", file=sys.stderr)
        return 1

    print(f"Created annotated tag {tag} and updated {manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
