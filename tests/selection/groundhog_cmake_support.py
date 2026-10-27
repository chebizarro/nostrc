"""Exact inverse of later Groundhog CMake edits for the closeout producer witness, never admission.

test_closeout_producers pins the audited bytes of gnome/groundhog/CMakeLists.txt.
The GnBlossomClient/bud02 linkage and symbol-visibility edits that followed
changed those bytes without changing the producer repair it audits. This helper
reverses exactly those recorded edits (pinned witness) and refuses any other
bytes, so the pin keeps meaning "the audited file plus these known edits".
"""
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/groundhog_cmake_source_edits_20261027.json'
SHA256 = '4a57d829e26d404cc0ddc9d27df981a2d11aeb8ae6cf40e6a0e9e3115e96047e'
PATH = 'gnome/groundhog/CMakeLists.txt'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'groundhog cmake edit witness changed')
    return json.loads(raw)


def before_groundhog_cmake(raw):
    record = witness()
    s.require(record['path'] == PATH, 'groundhog cmake witness path changed')
    digest = hashlib.sha256(raw).hexdigest()
    if digest == record['before_sha256']:
        return raw
    s.require(digest == record['after_sha256'], 'groundhog cmake source changed: ' + PATH)
    for edit in record['edits']:
        after, before = edit['after'].encode(), edit['before'].encode()
        s.require(raw.count(after) == 1, 'ambiguous groundhog cmake inverse: ' + PATH)
        raw = raw.replace(after, before, 1)
    s.require(hashlib.sha256(raw).hexdigest() == record['before_sha256'], 'historical groundhog cmake changed: ' + PATH)
    return raw
