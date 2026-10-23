"""Exact legacy macOS unconfigured NIP-5F TCP applicability (nostrc-0cfsg.36.9).

The legacy nostrc-ci macOS ASan/UBSan job (BUILD_APPS=OFF) provisions no signer
endpoint or token. ``test_nip5f_tcp`` then takes its unconfigured branch and
exits 77, which CTest reports as skipped (SKIP_RETURN_CODE 77). That never
validated a live TCP signer. This module authors the one exact consequence for
the selector: on ``core.macos.asan`` only, the ``signer_endpoint`` check's
``test_nip5f_tcp`` registration is accounted ``not_executed`` with
``validation: nonvalidation``. It is built and audited like every other target,
is never passed/skipped-as-pass and is never signing coverage.

Everything is bound to the authored record: profile variant, legacy workflow
job, test source, CMake registration and selection metadata, and the absence
of any endpoint/token in profile, workflow or actual test environment. Any
drift refuses; a configured endpoint needs a genuine provisioned signer under
the existing ``signer_endpoint`` capability instead. Mandatory loopback and
negative-environment registrations must stay claimed and executed. There is no
other profile, registration or general missing-resource waiver here.

Engine wiring (resolve/execute/receipt/aggregation) is a separate, coordinated
seam; this module only validates and returns the exact record.
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
# The unconfigured branch whose exit 77 the legacy job observed.
UNCONFIGURED = re.compile(
    r'const char \*ep = getenv\("NOSTR_SIGNER_ENDPOINT"\);\s*'
    r'const char \*tok = getenv\("NOSTR_SIGNER_TOKEN"\);\s*'
    r'if \(!ep \|\| strncmp\(ep, "tcp:", 4\) != 0 \|\| !tok \|\| !\*tok\) \{[^{}]*?return 77;[^{}]*?\}')
LEGACY_REGISTRATION = ("  add_test(NAME test_nip5f_tcp COMMAND test_nip5f_tcp)\n"
                       "  set_tests_properties(test_nip5f_tcp PROPERTIES SKIP_RETURN_CODE 77)\n")
SIGNER_PROPERTIES = ("ENVIRONMENT", "ENVIRONMENT_MODIFICATION")

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
        "source_sha256": "618c01fdb6d46b0eae8902f1cf543472cac0e5691cbef4945502210a476acbe0",
        "registration_sha256": "5b73b22a6357188a482762aeaa0f1930dbb6afe0a4abc49f67906189a8593c80",
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
    _require(isinstance(check, dict) and check.get("requires") == ["signer_endpoint"]
             and PROFILE in check.get("profiles", [])
             and (check.get("ctest_tests") or {}).get(PROFILE) == [REGISTRATION],
             "signer_endpoint check membership changed")
    mandatory = inventory.get("checks", {}).get(MANDATORY_CHECK)
    _require(isinstance(mandatory, dict) and PROFILE in mandatory.get("profiles", [])
             and mandatory.get("requirement") == "blocking" and mandatory.get("expected_outcome") == "pass"
             and not mandatory.get("requires")
             and set(MANDATORY) <= set((mandatory.get("ctest_tests") or {}).get(PROFILE, [])),
             "mandatory NIP-5F loopback/negative coverage is not blocking for the profile")
    _require(inventory.get("enforcement_ready") is False, "applicability does not authorize enforcement")


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
    desc = descriptors.get(REGISTRATION, {})
    _require(desc.get("owner") == ["nip5f.test_inputs", "nip5f.fixture_inputs"]
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


def require_unconfigured_environment(environment):
    """Execution-time guard: any endpoint/token (even empty) refuses the record."""
    present = sorted(k for k in SIGNER_ENVIRONMENT if k in environment)
    _require(not present, f"signer endpoint/token present at execution ({present}): "
                          "a configured endpoint needs a genuine provisioned signer")
