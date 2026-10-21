"""Strict PR9 helper projection for historical registration witnesses only."""
import ast
import hashlib
import json

from selector_support import ROOT, s


FIXTURE = ROOT / 'tests/selection/fixtures/resource_diagnostics_projection_20261022.json'


def project_resource_helper(case, content, historical_hash, policy):
    """Check the reviewed current bytes, then reconstruct the exact old bytes."""
    witness = json.loads(FIXTURE.read_text())
    case.assertEqual(witness['schema_version'], 1)
    case.assertEqual(witness['helper_path'], 'tests/common/test_blueprint_resources.py')
    case.assertEqual(historical_hash, witness['historical_helper_sha256'])
    case.assertNotEqual(historical_hash, witness['current_helper_sha256'])
    case.assertEqual(hashlib.sha256(content).hexdigest(), witness['current_helper_sha256'])
    for name, digest in witness['historical_fixtures_sha256'].items():
        case.assertEqual(hashlib.sha256((FIXTURE.parent / name).read_bytes()).hexdigest(), digest)
    for key, digest in witness['authority_records_sha256'].items():
        case.assertEqual(s.digest(policy[key]), digest)
    previous = content
    for edit in witness['reverse_edits']:
        current, old = edit['current'].encode(), edit['previous'].encode()
        case.assertTrue(current)
        case.assertEqual(previous.count(current), 1)
        previous = previous.replace(current, old, 1)
    case.assertEqual(hashlib.sha256(previous).hexdigest(), historical_hash)

    old_tree, new_tree = ast.parse(previous), ast.parse(content)
    def named(tree, name):
        return next(node for node in tree.body if getattr(node, 'name', None) == name)
    def shape(node):
        return ast.dump(node, include_attributes=False)
    old_class = named(old_tree, 'ResourceBuildTests')
    new_class = named(new_tree, 'ResourceBuildTests')
    case.assertEqual([node.name for node in old_class.body], [node.name for node in new_class.body])
    old_cases = [node.name for node in old_class.body if node.name.startswith('test_')]
    case.assertEqual(len(old_cases), 6)
    for old, new in zip(old_class.body, new_class.body):
        if old.name == 'setUp':
            # Only elapsed/stage diagnostics precede the existing setup body.
            case.assertEqual([shape(n) for n in old.body], [shape(n) for n in new.body[3:]])
        else:
            case.assertEqual(shape(old), shape(new))
    old_run, new_run = named(old_tree, 'run'), named(new_tree, 'run')
    child = next(node for node in new_run.body if isinstance(node, ast.Try))
    case.assertEqual(shape(old_run.body[0]), shape(child.body[0]))
    case.assertEqual([shape(n) for n in old_run.body[1:]], [shape(n) for n in new_run.body[-2:]])
    diagnostics = named(new_tree, 'ProgressDiagnosticsTests')
    case.assertEqual([node.name for node in diagnostics.body], [
        'test_start_is_flushed_before_blocked_child_and_failure_is_preserved',
        'test_diagnostics_are_bounded_without_changing_child_arguments',
        'test_launch_exception_is_logged_and_propagated_unchanged',
    ])

    resource = json.loads((FIXTURE.parent / 'resource_profile_exports.json').read_text())
    grotto = json.loads((FIXTURE.parent / 'grotto_profile_exports.json').read_text())
    fresh = witness['authored_exports']
    case.assertEqual(fresh['grotto.linux']['all_registration_names'],
                     sorted(grotto['variants']['native']['all_registration_names']))
    case.assertEqual(len(fresh['grotto.linux']['all_registration_names']), 461)
    for profile in ('groundhog.linux', 'grotto.linux'):
        record = fresh[profile]
        case.assertEqual(record['resource_descriptor'], resource['descriptors']['nostr-gtk-resource-build'])
        case.assertEqual(record['helper_input_sha256'], witness['current_helper_sha256'])
        case.assertFalse(record['complete'])
        case.assertFalse(record['complete_gate_eligible'])
    native = witness['native_resource_evidence']
    case.assertEqual(native['source_sha'], witness['change_commit'])
    case.assertEqual(native['helper_sha256'], witness['current_helper_sha256'])
    case.assertTrue(native['not_fresh_execution_in_this_task'])
    case.assertTrue(native['diagnostic_only'])
    case.assertEqual(native['original_timeout_cause'], 'UNRESOLVED')
    case.assertEqual(set(native['resources']), set(resource['primary_names']))
    for name, record in native['resources'].items():
        case.assertEqual(record['cases'], len(old_cases) + len(diagnostics.body))
        case.assertTrue(record['passed'])
        case.assertEqual(record['timeout_seconds'], 120)
        expected = resource['descriptors'][name]
        case.assertEqual(record['descriptor_contract'], {k: expected[k] for k in record['descriptor_contract']})
    return previous
