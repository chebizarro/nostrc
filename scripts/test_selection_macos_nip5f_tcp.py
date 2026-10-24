"""Exact legacy macOS unconfigured NIP-5F TCP applicability (nostrc-0cfsg.36.9).

The legacy nostrc-ci macOS ASan/UBSan job (BUILD_APPS=OFF) provisions no signer
endpoint or token. ``test_nip5f_tcp`` then takes its unconfigured branch and
exits 77, which CTest reports as skipped (SKIP_RETURN_CODE 77). That never
validated a live TCP signer. This module authors the one exact consequence for
the selector: on ``core.macos.asan`` only, ``test_nip5f_tcp`` is excluded
before execution and accounted ``not_executed`` with ``validation:
nonvalidation``. Like the libgo stress baseline, the profile is not eligible for
the ``signer_endpoint`` check (which stays authored and executed for every
profile that provisions a signer) and this record is the registration's only
authority there. The target is built and audited like every other target, and
the registration is never passed/skipped-as-pass and never signing coverage.

Everything is bound to the authored record: profile variant, legacy workflow
job, test source, CMake registration and selection metadata, and the absence
of any endpoint/token in profile, workflow or actual test environment. Any
drift refuses; a configured endpoint needs a genuine provisioned signer under
the existing ``signer_endpoint`` capability instead. Mandatory loopback and
negative-environment registrations must stay claimed and executed. There is no
other profile, registration or general missing-resource waiver here.

The realization engine calls validate_policy/validate_realization while
resolving, re-verifies sources and environments before CTest, and the receipt
aggregator calls validate_receipt_record. Each step refuses on any mismatch.
"""
import hashlib
import json
import re
from pathlib import Path

IDENT = "core_macos_asan_nip5f_tcp_unconfigured"
PROFILE = "core.macos.asan"
CHECK = "supported.nips.nip5f.signer_endpoint"
MANDATORY_CHECK = "root.asan.nip5f"
REGISTRATION = "test_nip5f_tcp"
TARGET = "test_nip5f_tcp"
AUTHORITY = "legacy_unconfigured_nonvalidation"
TRACKING = "nostrc-0cfsg.36.9"
LEGACY_JOB = ".github/workflows/nostrc-ci.yml:build-and-test"
LEGACY_WORKFLOW = ".github/workflows/nostrc-ci.yml"
SOURCE = "nips/nip5f/tests/test_nip5f_tcp.c"
REGISTRATION_FILE = "nips/nip5f/CMakeLists.txt"
SELECTION_FILE = "nips/nip5f/TestInventory.cmake"
MANDATORY = ("test_nip5f_loopback", "test_nip5f_negative_env")
SIGNER_ENVIRONMENT = ("NOSTR_SIGNER_ENDPOINT", "NOSTR_SIGNER_TOKEN")
RECORD_KEYS = {"id", "profile", "check", "registrations", "reason", "tracking", "binding"}
BINDING_KEYS = {"source_sha256", "registration_sha256", "selection_metadata_sha256",
                "legacy_job_source_sha256", "legacy_workflow_sha256"}
VARIANT = {"platform": "macos", "architecture": "arm64", "compiler": "clang", "build_type": "Debug",
           "instrumentation": "asan_ubsan", "source_dir": ".", "setup": LEGACY_JOB}
# The unconfigured branch whose exit 77 the legacy job observed, preceded by the
# resource-scheduled skip that never falls back to inherited credentials.
UNCONFIGURED = re.compile(
    r'if \(getenv\("CTEST_RESOURCE_GROUP_COUNT"\)\) \{[^{}]*?return 77;[^{}]*?\}\s*'
    r'const char \*ep = getenv\("NOSTR_SIGNER_ENDPOINT"\);\s*'
    r'const char \*tok = getenv\("NOSTR_SIGNER_TOKEN"\);\s*'
    r'if \(!ep \|\| strncmp\(ep, "tcp:", 4\) != 0 \|\| !tok \|\| !\*tok\) \{[^{}]*?return 77;[^{}]*?\}')
LEGACY_REGISTRATION = ("  add_test(NAME test_nip5f_tcp COMMAND test_nip5f_tcp)\n"
                       "  set_tests_properties(test_nip5f_tcp PROPERTIES SKIP_RETURN_CODE 77)\n")
SIGNER_PROPERTIES = ("ENVIRONMENT", "ENVIRONMENT_MODIFICATION")
# Producers that exist only when applications/the signer fixture are configured.
APP_PRODUCERS = ("grotto-daemon", "grotto-schemas", "test_nip5f_approver")
CACHE_ENTRY = re.compile(r"BUILD_APPS(?::[A-Za-z_]+)?=(.*)\Z")

# The single authored record. The inventory's coverage_gaps entry with this id
# must equal it exactly; any source/workflow change requires re-authoring here.
AUTHORED_RECORD = {
    "id": IDENT,
    "profile": PROFILE,
    "check": CHECK,
    "registrations": [REGISTRATION],
    "reason": ("Legacy nostrc-ci macOS ASan/UBSan (BUILD_APPS=OFF) provisions no NIP-5F TCP endpoint or token; "
               "test_nip5f_tcp then exits 77 (SKIP_RETURN_CODE 77), which never validated a live signer. "
               "Accounted not_executed/nonvalidation: never passed, skipped-as-pass or signing coverage."),
    "tracking": TRACKING,
    "binding": {
        "source_sha256": "1721a905f7fcb1007be4e393c55349e3f71c9cd957242623536e84dc3a8c0984",
        "registration_sha256": "d85dfaa2b4a907b02a2f16bdef81e051dbf00ffb8684ce3bbdf2c92d03e6228e",
        "selection_metadata_sha256": "8a54cc3454613a5450717c1c193f99dbb98ba0b6ce10154d74c160805236b0af",
        "legacy_job_source_sha256": "35ecf5ab6debc5277f69556b6e106e7b9191638ebbb08bddf1e6ff45590e158c",
        "legacy_workflow_sha256": "10a675396b64c928f3d5bb79025ab2f6be0acbacaa3410155090e186ecdcdcbc",
    },
}


def authored_record():
    return json.loads(json.dumps(AUTHORED_RECORD))


class ApplicabilityError(ValueError):
    pass


def _require(condition, message):
    if not condition:
        raise ApplicabilityError(message)


def _sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _options(profile):
    """Every cache definition, in both -DKEY=V and split -D KEY=V forms.

    Unsetting (-U), preload scripts (-C) and duplicates refuse: each could
    change BUILD_APPS or instrumentation behind a literal -DKEY=V check."""
    options, args = {}, list(profile.get("configure_args", []))
    for index, arg in enumerate(args):
        _require(isinstance(arg, str), "non-string configure argument")
        if arg == "-D":
            _require(index + 1 < len(args) and isinstance(args[index + 1], str), "dangling -D")
            definition = args[index + 1]
        elif arg.startswith("-D"):
            definition = arg[2:]
        else:
            _require(not arg.startswith(("-U", "-C")), f"unsupported cache-altering argument {arg}")
            continue
        _require("=" in definition, f"malformed cache definition {definition}")
        key, value = definition.split("=", 1)
        key = key.split(":", 1)[0]
        _require(key not in options, f"duplicate configure option {key}")
        options[key] = value.upper()
    return options


def _mentions_signer(value):
    return "NOSTR_SIGNER" in json.dumps(value, sort_keys=True)


def records(inventory):
    """The authored applicability records (coverage_gaps entries with this id)."""
    gaps = inventory.get("coverage_gaps", [])
    _require(isinstance(gaps, list), "coverage_gaps must be a list")
    found = [g for g in gaps if isinstance(g, dict) and g.get("id") == IDENT]
    _require(len(found) <= 1, "duplicate macOS NIP-5F TCP applicability authority")
    others = [g for g in gaps if isinstance(g, dict) and g.get("id") != IDENT
              and REGISTRATION in (g.get("registrations") or [])]
    _require(not others, "another authority claims test_nip5f_tcp applicability")
    return found


def validate_policy(inventory, record, root):
    """Source/config/profile/workflow-bound validation of the authored record."""
    root = Path(root)
    _require(isinstance(record, dict) and set(record) == RECORD_KEYS, "malformed applicability record")
    _require(record == AUTHORED_RECORD, "applicability record differs from the authored legacy record")
    _require(record["id"] == IDENT and record["profile"] == PROFILE and record["check"] == CHECK
             and record["registrations"] == [REGISTRATION], "applicability record is not the exact legacy pair")
    _require(record["tracking"] == TRACKING, "applicability record tracking mismatch")
    _require(isinstance(record["reason"], str) and record["reason"].strip(), "applicability reason missing")
    binding = record["binding"]
    _require(isinstance(binding, dict) and set(binding) == BINDING_KEYS, "malformed applicability binding")

    profile = inventory.get("profiles", {}).get(PROFILE)
    _require(isinstance(profile, dict), "legacy macOS profile is missing")
    _require(all(profile.get(k) == v for k, v in VARIANT.items()), "profile is not the established legacy macOS variant")
    options = _options(profile)
    _require(options.get("BUILD_APPS") == "OFF", "legacy macOS profile must keep BUILD_APPS=OFF")
    _require(options.get("GO_ENABLE_ASAN") == "ON" and options.get("GO_ENABLE_UBSAN") == "ON",
             "legacy macOS profile instrumentation changed")
    _require("signer_endpoint" not in profile.get("capabilities", []),
             "profile provisions signer_endpoint: use the genuine signer route, not this record")
    _require(not _mentions_signer(profile.get("environment", {})) and not _mentions_signer(profile.get("phases")),
             "profile environment configures a signer endpoint/token")

    job = inventory.get("workflow_jobs", {}).get(LEGACY_JOB)
    _require(isinstance(job, dict) and PROFILE in job.get("profiles", []), "legacy workflow job does not own the profile")
    _require(job.get("source_sha256") == binding["legacy_job_source_sha256"]
             and job.get("workflow_sha256") == binding["legacy_workflow_sha256"], "legacy workflow job drifted")
    workflow = root / LEGACY_WORKFLOW
    _require(_sha256(workflow) == binding["legacy_workflow_sha256"], "legacy workflow source drifted")
    _require("NOSTR_SIGNER" not in workflow.read_text(), "legacy workflow provisions a signer endpoint/token")

    for path, key in ((SOURCE, "source_sha256"), (REGISTRATION_FILE, "registration_sha256"),
                      (SELECTION_FILE, "selection_metadata_sha256")):
        _require(_sha256(root / path) == binding[key], f"{path} drifted from the authored legacy semantics")
    _require(UNCONFIGURED.search((root / SOURCE).read_text()), "unconfigured exit-77 branch changed")
    _require(LEGACY_REGISTRATION in (root / REGISTRATION_FILE).read_text(), "legacy SKIP_RETURN_CODE 77 registration changed")

    check = inventory.get("checks", {}).get(CHECK)
    _require(isinstance(check, dict) and check.get("kind") == "ctest" and check.get("requires") == ["signer_endpoint"]
             and check.get("requirement") == "blocking" and check.get("expected_outcome") == "pass",
             "signer_endpoint check semantics changed")
    others = [pid for pid in check.get("profiles", []) if pid != PROFILE]
    _require(PROFILE not in check.get("profiles", []) and PROFILE not in (check.get("ctest_tests") or {})
             and others and all((check.get("ctest_tests") or {}).get(pid) == [REGISTRATION] for pid in others),
             "signer_endpoint check must keep test_nip5f_tcp for signer profiles and exclude only legacy macOS")
    for ident, other in inventory.get("checks", {}).items():
        _require(not (PROFILE in other.get("profiles", [])
                      and REGISTRATION in (other.get("ctest_tests") or {}).get(PROFILE, [])),
                 f"test_nip5f_tcp is also claimed on legacy macOS by {ident}")
    mandatory = inventory.get("checks", {}).get(MANDATORY_CHECK)
    _require(isinstance(mandatory, dict) and PROFILE in mandatory.get("profiles", [])
             and mandatory.get("requirement") == "blocking" and mandatory.get("expected_outcome") == "pass"
             and not mandatory.get("requires")
             and set(MANDATORY) <= set((mandatory.get("ctest_tests") or {}).get(PROFILE, [])),
             "mandatory NIP-5F loopback/negative coverage is not blocking for the profile")
    _require(inventory.get("enforcement_ready") is False, "applicability does not authorize enforcement")


def observed_build_apps(build_dir):
    """The effective cache value, not the authored argv: catches FORCE/preload/
    toolchain cache overrides. Exactly one entry, exactly OFF."""
    path = Path(build_dir) / "CMakeCache.txt"
    _require(path.is_file() and not path.is_symlink(), "native CMake cache missing for BUILD_APPS proof")
    values = [m[1] for m in map(CACHE_ENTRY.match, path.read_text().splitlines()) if m]
    _require(values == ["OFF"], f"effective cache BUILD_APPS is not exactly OFF: {values}")


def require_no_application_producers(realization, relative):
    """A plain variable can shadow the cache: prove the configured graph itself
    contains no application or signer-fixture producer."""
    for target in realization.get("targets", {}).values():
        sources = {relative(src["path"], realization["repo_root"]) for src in target.get("sources", [])}
        _require(target.get("name") not in APP_PRODUCERS
                 and not any(path and path.startswith("apps/") for path in sources),
                 f"configured graph contains an application/signer producer: {target.get('name')}")


def validate_realization(inventory, record, realization, tests, descriptors, mapped_targets, relative):
    """Configured-graph validation; returns the not_executed record or None.

    ``inventory`` must already have passed validate_policy with ``record``;
    the realized profile must equal its authored profile exactly.
    ``mapped_targets``/``relative`` are the realization engine's own helpers."""
    if realization.get("profile_id") != PROFILE:
        return None
    _require(realization.get("profile") == inventory.get("profiles", {}).get(PROFILE),
             "realized profile differs from the validated legacy macOS profile")
    _require("signer_endpoint" not in realization["profile"].get("capabilities", []),
             "realized profile provisions signer_endpoint")
    observed_build_apps(realization["build_dir"])
    require_no_application_producers(realization, relative)
    _require(REGISTRATION in tests, "legacy test_nip5f_tcp registration is missing")
    for name in MANDATORY:
        _require(name in tests, f"mandatory registration missing: {name}")
    props = {p["name"]: p["value"] for p in tests[REGISTRATION].get("properties", [])}
    _require(str(props.get("SKIP_RETURN_CODE")) == "77", "registration no longer maps exit 77 to CTest skip")
    _require(not any(props.get(k) for k in ("DISABLED", "WILL_FAIL", "FIXTURES_SETUP", "FIXTURES_CLEANUP",
                                            "FIXTURES_REQUIRED", "DEPENDS")),
             "registration metadata changed")
    _require(not any(_mentions_signer(props.get(key)) for key in SIGNER_PROPERTIES),
             "registration environment configures a signer")
    # Its only credential route is the signer resource allocation, which this
    # profile never provisions (no resource spec): CTest allocates nothing.
    from test_selection_services import SIGNER_RESOURCE_GROUPS
    _require(props.get("RESOURCE_GROUPS") == SIGNER_RESOURCE_GROUPS,
             "registration signer resource route changed")
    desc = descriptors.get(REGISTRATION, {})
    # Owner list exactly as the real configured graph reports it: the nips/nip5f
    # directory owner plus TestInventory.cmake's supplements.
    _require(desc.get("owner") == ["nip5f", "nip5f.test_inputs", "nip5f.fixture_inputs"]
             and desc.get("requires") == ["signer_endpoint"]
             and not desc.get("targets") and not desc.get("inputs")
             and desc.get("expected") in (None, [], ["pass"]),
             "selection metadata is not the unconfigured legacy shape (approver/fixture present?)")
    target_ids = mapped_targets(tests[REGISTRATION], desc, realization)
    targets = [realization["targets"][key] for key in target_ids]
    _require(len(targets) == 1 and targets[0]["name"] == TARGET and targets[0]["type"] == "EXECUTABLE"
             and SOURCE in {relative(s["path"], realization["repo_root"]) for s in targets[0]["sources"]},
             "test_nip5f_tcp executable/source mapping changed")
    fragments = " ".join(targets[0].get("compile_fragments", []))
    sanitizers = {v for m in re.finditer(r"(?:^|\s)-fsanitize=([^\s]+)", fragments) for v in m[1].split(",")}
    _require({"address", "undefined"} <= sanitizers and "thread" not in sanitizers and "-fno-sanitize=" not in fragments,
             "actual sanitizer instrumentation differs")
    return dict(record, status="not_executed", authority=AUTHORITY, validation="nonvalidation",
                target_ids=sorted(target_ids), audit_build_targets=[TARGET])


def verify_sources(root):
    """Execution-time re-check of the authored source/registration binding."""
    binding = AUTHORED_RECORD["binding"]
    for path, key in ((SOURCE, "source_sha256"), (REGISTRATION_FILE, "registration_sha256"),
                      (SELECTION_FILE, "selection_metadata_sha256"), (LEGACY_WORKFLOW, "legacy_workflow_sha256")):
        _require(_sha256(Path(root) / path) == binding[key], f"{path} changed after resolution")


def validate_receipt_record(exclusion, policy, lane_profile):
    """Receipt/aggregate boundary: the exact authored accounting, nothing else."""
    authored = authored_record()
    known = [g for g in policy.get("coverage_gaps", []) if isinstance(g, dict) and g.get("id") == IDENT]
    _require(known == [authored], "policy lacks the exact authored legacy macOS record")
    _require(lane_profile == PROFILE, "legacy macOS NIP-5F TCP accounting on another profile")
    _require(isinstance(exclusion, dict)
             and set(exclusion) == set(authored) | {"status", "authority", "validation", "target_ids", "audit_build_targets"}
             and all(exclusion.get(k) == v for k, v in authored.items())
             and exclusion.get("status") == "not_executed" and exclusion.get("authority") == AUTHORITY
             and exclusion.get("validation") == "nonvalidation" and exclusion.get("audit_build_targets") == [TARGET]
             and isinstance(exclusion.get("target_ids"), list) and len(exclusion["target_ids"]) == 1
             and isinstance(exclusion["target_ids"][0], str) and exclusion["target_ids"][0],
             "unknown/mismatched legacy macOS NIP-5F TCP accounting")
    for ident, check in policy.get("checks", {}).items():
        _require(REGISTRATION not in (check.get("ctest_tests") or {}).get(PROFILE, []),
                 f"test_nip5f_tcp is claimed on legacy macOS by {ident}")


def require_unconfigured_environment(environment):
    """Execution-time guard: any endpoint/token (even empty) refuses the record."""
    present = sorted(k for k in SIGNER_ENVIRONMENT if k in environment)
    _require(not present, f"signer endpoint/token present at execution ({present}): "
                          "a configured endpoint needs a genuine provisioned signer")
