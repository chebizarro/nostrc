"""Finite JSON-GLib enrollment inverse for historical tests, not admission."""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/w11_json_glib_enrollment_20261023.json'
SHA256 = 'b5a7f13a37aed96a499db2eea5237cf25c04854044adbc16c7d7ec1c6283657d'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'JSON-GLib enrollment witness changed')
    return json.loads(raw)


def before_jobs(records):
    previous = copy.deepcopy(records)
    for ident, record in witness()['jobs'].items():
        value = previous.get(ident)
        keys = set(value) if isinstance(value, dict) else set()
        s.require(keys in (set(record['before']), {'source_sha256', 'workflow_sha256', 'matrix_source'}),
                  'JSON-GLib workflow record shape changed: ' + ident)
        before = {key: record['before'][key] for key in keys}
        after = {key: record['after'][key] for key in keys}
        s.require(value in (before, after), 'JSON-GLib workflow record changed: ' + ident)
        previous[ident] = copy.deepcopy(before)
    return previous


def before_source(path, raw):
    data = witness()['workflow']
    if path != data['path'] or hashlib.sha256(raw).hexdigest() == data['before_sha256']:
        return raw
    s.require(hashlib.sha256(raw).hexdigest() == data['after_sha256'], 'JSON-GLib workflow source changed')
    for addition in data['additions']:
        s.require(raw.count(addition.encode()) == 1, 'JSON-GLib setup source insertion changed')
        raw = raw.replace(addition.encode(), b'', 1)
    s.require(hashlib.sha256(raw).hexdigest() == data['before_sha256'], 'JSON-GLib historical source changed')
    return raw


def before_policy(policy):
    previous = copy.deepcopy(policy)
    data = witness()
    inputs = previous['checks']['selection.contract']['runtime_inputs']
    if not any(value in inputs for value in data['input_additions']):
        s.require(all(previous['workflow_jobs'][key] == value['before'] for key, value in data['jobs'].items()),
                  'JSON-GLib enrollment partially stripped')
        return previous
    previous['workflow_jobs'] = before_jobs(previous['workflow_jobs'])
    for value in data['input_additions']:
        s.require(inputs.count(value) == 1, 'JSON-GLib physical source input missing/duplicated: ' + value)
        inputs.remove(value)
    for rule in data['rules']:
        s.require(previous['path_rules'].count(rule) == 1, 'JSON-GLib semantic ownership changed')
        previous['path_rules'].remove(rule)
    return previous
