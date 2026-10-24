#!/usr/bin/env python3
"""Actions runner projection of the shared planner; conclusions, not attestations."""
import argparse
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import test_selection as selection
from dependency_contract import BROAD_FAMILIES

# These are runner adapters, not another path classifier or dependency graph.
# The shared planner owns path classification, history union and impact closure.
FAMILY_NODES = {
    'core': {'libnostr', 'libjson', 'libmarmot', 'marmot.gobject', 'shared.tests',
             'testing.framework', 'testkit', 'schema', 'execution.core'},
    'libgo': {'libgo', 'execution.libgo'},
    'hanami': {'libhanami'},
    'gi': {'libnostr', 'nostr.gobject'},
    'groundhog': {'execution.groundhog', 'packaging.groundhog'},
    'grotto': {'execution.grotto', 'packaging.grotto', 'gnome.seahorse'},
    'nip55l': {'nip55l', 'nip55l.core', 'nip55l.glib', 'nip55l.dbus_contract'},
    'homed': {'execution.homed', 'gnome.nostr_homed', 'gnome.nostr_homed.test_inputs'},
    'signet': {'signet'},
    'static': {'static.policy', 'selection.contract'},
    'appimage': {'gnostr', 'packaging.gnostr', 'gnostr.schema_inputs'},
    'windows': {'grotto.gui', 'grotto.daemon', 'grotto.native_host', 'packaging.grotto'},
    'bp': {'benchmarks'},
    'bp_extended': {'benchmarks'},
    'fuzz': {'nip49', 'nip06', 'libjson', 'libnostr'},
    'mdk08': {'libmarmot', 'marmot.gobject', 'marmot.vector_inputs', 'mdk08.peer_inputs',
              'mdk09.peer_inputs'},
    'mdk011': {'libmarmot', 'marmot.gobject', 'marmot.vector_inputs', 'mdk011.peer_inputs'},
}
REFERENCE_NODES = {'groundhog.mls_store', 'groundhog.mls_service', 'groundhog.mls_media',
                   'groundhog.mls_files', 'groundhog.mls_ui', 'groundhog.voice'}


def project(plan):
    """Project affected ownership, never prerequisite/diagnostic profile membership."""
    nodes = set(plan['witnesses'])
    families = {family for family, owners in FAMILY_NODES.items() if nodes & owners}
    accounted = set().union(*FAMILY_NODES.values())
    for node in nodes:
        if node.startswith('groundhog.'):
            families.add('groundhog')
            accounted.add(node)
        elif node.startswith('grotto.'):
            families.add('grotto')
            accounted.add(node)
        elif node.startswith('nip'):
            families.add('core')
            accounted.add(node)
    reference = bool(nodes & REFERENCE_NODES)
    if reference:
        families.update(('mdk08', 'mdk011'))
    reasons = list(plan['broadening_reasons'])
    unhandled = nodes - accounted
    if unhandled:
        reasons.append('No bounded legacy runner projection for: ' + ', '.join(sorted(unhandled)))
    broad = plan['effective_mode'] == 'broad' or bool(unhandled)
    if broad:
        families = set(BROAD_FAMILIES)
    return {'schema_version': 1, 'kind': 'ci_legacy_dispatch_v1',
            'scope': 'legacy_family_conclusions_not_profile_or_release_attestation',
            'sha': plan['tested_sha'], 'shared_plan_id': plan['plan_id'],
            'broad': broad, 'families': sorted(families),
            'run_reference': broad or reference or bool(families & {'mdk08', 'mdk011'}),
            'reasons': sorted(set(reasons))}


def event_contract(name, event, sha, full=False):
    """PR identities come only from event SHAs, not mutable refs or fork names."""
    mode = 'broad' if full else 'affected'
    if name == 'pull_request':
        pr = event['pull_request']
        if any(label['name'] == 'ci:full' for label in pr.get('labels', [])):
            mode = 'broad'
        return {'kind': name, 'base_sha': pr['base']['sha'],
                'pr_head_sha': pr['head']['sha'], 'tested_sha': sha}, mode, 'ci'
    if name == 'merge_group':
        group = event['merge_group']
        selection.require(group['head_sha'] == sha, 'merge-group tested SHA mismatch')
        return {'kind': name, 'base_sha': group['base_sha'], 'tested_sha': sha}, mode, 'ci'
    selection.require(name in {'push', 'release', 'workflow_dispatch', 'workflow_call'},
                      'unsupported Actions event')
    selection.require(not event.get('deleted'), 'deleted refs have no CI execution')
    # Master/main, all release/component tags, releases and manual/reusable runs
    # are broad regardless of an input asking for affected selection.
    return {'kind': 'manual', 'tested_sha': sha}, 'broad', 'ci'


def aggregate(dispatch, needs):
    expected = {'select': 'success', 'cheap': 'success',
                'full_static': 'success' if dispatch['broad'] else 'skipped'}
    expected.update({family: 'success' if family in dispatch['families'] else 'skipped'
                     for family in BROAD_FAMILIES})
    errors = [f'{job}: expected {wanted}, observed {needs.get(job, {}).get("result", "missing")}'
              for job, wanted in expected.items() if needs.get(job, {}).get('result') != wanted]
    if set(needs) != set(expected):
        errors.append('aggregate job set differs from the runner contract')
    return {'schema_version': 1, 'kind': 'ci_legacy_conclusions_v1', 'sha': dispatch['sha'],
            'scope': dispatch['scope'], 'families': {
                family: {'selected': family in dispatch['families'],
                         'result': needs.get(family, {}).get('result', 'missing'),
                         'coverage': 'legacy_suite_conclusion' if family in dispatch['families']
                                     else 'not_executed'} for family in BROAD_FAMILIES},
            'passed': not errors, 'errors': errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['select', 'aggregate'])
    parser.add_argument('--sha', required=True)
    parser.add_argument('--plan', type=Path, required=True)
    parser.add_argument('--needs', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--full', action='store_true')
    args = parser.parse_args()
    selection.require(selection.commit_sha(ROOT, args.sha) == args.sha,
                      'requested identity is not a full commit SHA')
    selection.require(selection.commit_sha(ROOT, 'HEAD') == args.sha, 'checkout is not requested SHA')
    policy = selection.load_inventory(ROOT / 'tests/selection/inventory.json')
    if args.command == 'select':
        event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text())
        metadata, mode, context = event_contract(os.environ['GITHUB_EVENT_NAME'], event, args.sha, args.full)
        shared = selection.git_plan(ROOT, policy, head=args.sha, context=context, mode=mode, event=metadata)
        dispatch = project(shared)
        args.plan.parent.mkdir(parents=True, exist_ok=True)
        args.plan.write_text(json.dumps({'shared_plan': shared, 'dispatch': dispatch}, indent=2) + '\n')
        with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
            for key in ('sha', 'broad', 'families', 'run_reference'):
                output.write(f'{key}={json.dumps(dispatch[key]) if not isinstance(dispatch[key], str) else dispatch[key]}\n')
        summary = ['## CI selection', f'Commit: `{args.sha}`',
                   'Mode: **broad**' if dispatch['broad'] else 'Mode: **affected**',
                   'Selected families: ' + (', '.join(dispatch['families']) or '(none)'),
                   'Reference compatibility: ' + ('selected' if dispatch['run_reference'] else 'not executed'),
                   *['- ' + reason for reason in dispatch['reasons']],
                   'These are legacy suite conclusions, not physical profile/release attestations.']
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as output:
            output.write('\n\n'.join(summary) + '\n')
    else:
        saved = json.loads(args.plan.read_text())
        selection.validate_plan(saved['shared_plan'], policy)
        dispatch = project(saved['shared_plan'])
        selection.require(saved['dispatch'] == dispatch and dispatch['sha'] == args.sha,
                          'same-run dispatch does not match the bound shared plan')
        result = aggregate(dispatch, json.loads(args.needs.read_text()))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n')
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as output:
            output.write('## CI conclusions\n\n' + '\n'.join(
                f'- {family}: {item["result"]} ({item["coverage"]})'
                for family, item in result['families'].items()) + '\n')
        selection.require(result['passed'], '; '.join(result['errors']))


if __name__ == '__main__':
    try:
        main()
    except (selection.ContractError, KeyError, ValueError, OSError) as exc:
        print(f'CI selection refused: {exc}', file=sys.stderr)
        sys.exit(1)
