"""Reverse only this standalone enrollment for immutable historical witnesses."""
import copy
import json
import hashlib
from selector_support import ROOT, s


def before_homed_source(path, raw):
    from runtime_integration_support import before_runtime_source
    raw = before_runtime_source(path, raw)
    data = json.loads((ROOT / "tests/selection/fixtures/homed_profile_enrollment_20261023.json").read_text())
    record = data.get("sources", {}).get(path)
    if record is None or hashlib.sha256(raw).hexdigest() == record["before_sha256"]:
        return raw
    s.require(hashlib.sha256(raw).hexdigest() == record["after_sha256"], "homed source enrollment changed: " + path)
    for edit in record["edits"]:
        after, before = edit["after"].encode(), edit["before"].encode()
        s.require(raw.count(after) == 1, "ambiguous homed source reversal: " + path)
        raw = raw.replace(after, before, 1)
    s.require(hashlib.sha256(raw).hexdigest() == record["before_sha256"], "homed historical source changed: " + path)
    return raw


def before_homed_policy(policy):
    from runtime_integration_support import before_runtime_policy
    previous = before_runtime_policy(policy)
    if "homed.linux.units" not in previous.get("profiles", {}):
        return previous
    data = json.loads((ROOT / "tests/selection/fixtures/homed_profile_enrollment_20261023.json").read_text())
    for field, records in data["maps"].items():
        for ident, record in records.items():
            s.require(previous[field].get(ident) == record["after"], "homed enrollment changed: " + ident)
            if record["before"] is None:
                del previous[field][ident]
            else:
                previous[field][ident] = copy.deepcopy(record["before"])
    for rule in data["added_path_rules"]:
        s.require(previous["path_rules"].count(rule) == 1, "homed source ownership changed")
        previous["path_rules"].remove(rule)
    inputs = previous["checks"]["selection.contract"]["runtime_inputs"]
    for path in data["runtime_inputs"]:
        s.require(inputs.count(path) == 1, "homed command input enrollment changed")
        inputs.remove(path)
    nodes = previous["checks"]["selection.contract"]["nodes"]
    for node in data["selection_nodes"]:
        s.require(nodes.count(node) == 1, "homed command impact enrollment changed")
        nodes.remove(node)
    for record in data["gaps"]:
        matches = [g for g in previous["coverage_gaps"] if g.get("id") == record["after"]["id"]]
        s.require(matches == [record["after"]], "homed coverage remainder changed")
        previous["coverage_gaps"][previous["coverage_gaps"].index(matches[0])] = copy.deepcopy(record["before"])
    return previous
