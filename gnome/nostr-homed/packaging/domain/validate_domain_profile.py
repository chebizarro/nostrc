#!/usr/bin/env python3
"""Strict, side-effect-free validation for the candidate winbind route."""

import argparse
import configparser
import hashlib
import json
from pathlib import Path
import re
import stat
import sys

HEX64 = re.compile(r"^[0-9a-f]{64}$")
PACKAGE_KEYS = {"samba", "winbind", "libpam-winbind", "libpam-runtime"}
ROUTE_SECTIONS = {
    "routing": {"nostr_prefix", "domain_separator", "require_fully_qualified", "no_cross_route_fallback"},
    "identity_ranges": {"nostr", "standalone_smb", "idmap_default", "idmap_primary"},
    "domain": {"name", "realm", "offline_login", "home_template", "home_mode"},
}
SMB_KEYS = {
    "security", "workgroup", "realm", "kerberos method", "dedicated keytab file",
    "winbind use default domain", "winbind offline logon", "winbind refresh tickets",
    "winbind enum users", "winbind enum groups", "idmap config * : backend",
    "idmap config * : range", "template homedir", "template shell",
}

class ValidationError(Exception):
    pass

def read_ini(path):
    parser = configparser.ConfigParser(interpolation=None, strict=True, delimiters=("=",))
    parser.optionxform = str.lower
    try:
        with path.open("r", encoding="utf-8") as stream:
            parser.read_file(stream)
    except (OSError, UnicodeError, configparser.Error) as exc:
        raise ValidationError(f"cannot parse {path}: {exc}") from exc
    return parser

def exact_sections(parser, expected, label):
    if set(parser.sections()) != set(expected):
        raise ValidationError(f"{label} sections must be exactly {sorted(expected)}")
    for section, keys in expected.items():
        observed = set(parser[section])
        if observed != keys:
            raise ValidationError(f"{label} [{section}] keys must be exactly {sorted(keys)}")

def yes(value, name):
    value = value.strip().lower()
    if value not in {"true", "false", "yes", "no"}:
        raise ValidationError(f"{name} must be true/false")
    return value in {"true", "yes"}

def uid_range(value, name):
    match = re.fullmatch(r"([0-9]+)-([0-9]+)", value.strip())
    if not match:
        raise ValidationError(f"{name} must be MIN-MAX")
    low, high = map(int, match.groups())
    if low < 1 or high > 2**31 - 1 or low > high:
        raise ValidationError(f"{name} is outside the supported UID/GID range")
    return low, high

def validate_configs(route_path, smb_path):
    route = read_ini(route_path)
    exact_sections(route, ROUTE_SECTIONS, "route")
    routing = route["routing"]
    domain = route["domain"]
    ranges = {key: uid_range(value, key) for key, value in route["identity_ranges"].items()}
    ordered = sorted((low, high, name) for name, (low, high) in ranges.items())
    for previous, current in zip(ordered, ordered[1:]):
        if previous[1] >= current[0]:
            raise ValidationError(f"identity ranges overlap: {previous[2]} and {current[2]}")
    if routing["nostr_prefix"] != "n_":
        raise ValidationError("Nostr names must use the reserved n_ prefix")
    if routing["domain_separator"] != "\\":
        raise ValidationError("domain_separator must be one backslash")
    if not yes(routing["require_fully_qualified"], "require_fully_qualified"):
        raise ValidationError("fully-qualified DOMAIN\\user names are required")
    if not yes(routing["no_cross_route_fallback"], "no_cross_route_fallback"):
        raise ValidationError("cross-route fallback must be disabled")
    if yes(domain["offline_login"], "offline_login"):
        raise ValidationError("offline domain login is unsupported until separately accepted")
    if domain["home_template"] != "/home/%D/%U" or domain["home_mode"] != "0700":
        raise ValidationError("domain homes must use /home/%D/%U with mode 0700")
    name = domain["name"].strip()
    realm = domain["realm"].strip()
    if not re.fullmatch(r"[A-Z][A-Z0-9_-]{0,14}", name):
        raise ValidationError("domain name must be an uppercase NetBIOS name")
    if not re.fullmatch(r"[A-Z0-9][A-Z0-9.-]+", realm) or "." not in realm:
        raise ValidationError("realm must be an uppercase DNS realm")

    smb = read_ini(smb_path)
    if smb.sections() != ["global"]:
        raise ValidationError("smb.conf sample must contain only [global]")
    options = set(smb["global"])
    domain_backend = f"idmap config {name.lower()} : backend"
    domain_range = f"idmap config {name.lower()} : range"
    expected = SMB_KEYS | {domain_backend, domain_range}
    if options != expected:
        raise ValidationError(f"smb.conf [global] keys must be exactly {sorted(expected)}")
    g = smb["global"]
    required = {
        "security": "ads", "workgroup": name, "realm": realm,
        "kerberos method": "secrets and keytab",
        "dedicated keytab file": "/etc/krb5.keytab",
        "winbind use default domain": "no", "winbind offline logon": "no",
        "winbind refresh tickets": "yes", "winbind enum users": "no",
        "winbind enum groups": "no", "idmap config * : backend": "tdb",
        domain_backend: "rid", "template homedir": "/home/%D/%U",
        "template shell": "/bin/bash",
    }
    for key, expected_value in required.items():
        if g[key].strip().lower() != expected_value.lower():
            raise ValidationError(f"{key} must be {expected_value}")
    if uid_range(g["idmap config * : range"], "default idmap range") != ranges["idmap_default"]:
        raise ValidationError("default idmap range differs from route contract")
    if uid_range(g[domain_range], "primary idmap range") != ranges["idmap_primary"]:
        raise ValidationError("primary idmap range differs from route contract")

def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

def validate_manifest(path, require_ready, root):
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValidationError(f"cannot parse activation manifest: {exc}") from exc
    expected = {"schema_version", "activation_ready", "d4_evidence_sha256", "package_pins", "pam_files", "rollback_dir", "notes"}
    if not isinstance(manifest, dict) or set(manifest) != expected or type(manifest["schema_version"]) is not int or manifest["schema_version"] != 1:
        raise ValidationError("activation manifest has an unknown schema or field")
    if not isinstance(manifest["activation_ready"], bool):
        raise ValidationError("activation_ready must be boolean")
    if not isinstance(manifest["package_pins"], dict) or set(manifest["package_pins"]) != PACKAGE_KEYS:
        raise ValidationError("package_pins must contain the exact required package set")
    if not isinstance(manifest["rollback_dir"], str):
        raise ValidationError("rollback_dir must be a path string")
    rollback = Path(manifest["rollback_dir"])
    if ".." in rollback.parts or not rollback.is_absolute() or str(rollback) in {"/", "/etc", "/etc/pam.d"}:
        raise ValidationError("rollback_dir must be a dedicated absolute path")
    if not isinstance(manifest["pam_files"], list):
        raise ValidationError("pam_files must be a list")
    if not require_ready:
        return
    if not manifest["activation_ready"]:
        raise ValidationError("manifest is explicitly not activation-ready")
    if not isinstance(manifest["d4_evidence_sha256"], str) or not HEX64.fullmatch(manifest["d4_evidence_sha256"]):
        raise ValidationError("D4 joined-host evidence is not pinned")
    if any(not isinstance(value, str) or not value.strip() for value in manifest["package_pins"].values()):
        raise ValidationError("every required package version must be pinned")
    if not manifest["pam_files"]:
        raise ValidationError("activation requires expected hashes for existing PAM files")
    if root is None:
        raise ValidationError("activation readiness requires --root to verify the installed PAM layout")
    root = root.resolve(strict=True)
    seen = set()
    for entry in manifest["pam_files"]:
        if not isinstance(entry, dict) or set(entry) != {"path", "sha256"}:
            raise ValidationError("pam_files entries require only path and sha256")
        if not isinstance(entry["path"], str):
            raise ValidationError("PAM path must be a string")
        logical = Path(entry["path"])
        if ".." in logical.parts or not logical.is_absolute() or not str(logical).startswith("/etc/pam.d/") or logical in seen:
            raise ValidationError("PAM paths must be unique absolute files below /etc/pam.d")
        seen.add(logical)
        if not isinstance(entry["sha256"], str) or not HEX64.fullmatch(entry["sha256"]):
            raise ValidationError(f"invalid SHA-256 for {logical}")
        physical = root / logical.relative_to("/")
        try:
            current = root
            for part in logical.parts[1:]:
                current = current / part
                if current.is_symlink():
                    raise ValidationError(f"PAM path traverses a symlink: {logical}")
            physical.resolve(strict=True).relative_to(root)
            mode = physical.lstat().st_mode
        except OSError as exc:
            raise ValidationError(f"required PAM file is unavailable: {logical}: {exc}") from exc
        if stat.S_ISLNK(mode) or not stat.S_ISREG(mode):
            raise ValidationError(f"PAM path is not a regular non-symlink file: {logical}")
        if sha256_file(physical) != entry["sha256"]:
            raise ValidationError(f"installed PAM file differs from reviewed hash: {logical}")

def main(argv=None):
    parser = argparse.ArgumentParser()
    base = Path(__file__).resolve().parents[2]
    parser.add_argument("--route", type=Path, default=base / "config/domain-route.conf.sample")
    parser.add_argument("--smb", type=Path, default=base / "config/smb.conf.winbind.sample")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--require-activation-ready", action="store_true")
    parser.add_argument("--root", type=Path)
    args = parser.parse_args(argv)
    try:
        validate_configs(args.route, args.smb)
        if args.require_activation_ready and args.manifest is None:
            raise ValidationError("--require-activation-ready requires --manifest")
        if args.manifest is not None:
            validate_manifest(args.manifest, args.require_activation_ready, args.root)
    except (ValidationError, OSError, ValueError) as exc:
        print(json.dumps({"status": "invalid", "error": str(exc)}), file=sys.stderr)
        return 1
    print(json.dumps({"status": "valid", "activation_ready": bool(args.require_activation_ready)}))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
