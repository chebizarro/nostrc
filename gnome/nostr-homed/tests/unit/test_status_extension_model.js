// nostrc-h10m.6 — headless test of the nostr-home-status@nostrc view model.
//
// Run: gjs -m tests/unit/test_status_extension_model.js
//
// model.js has no GNOME imports; this exercises everything the panel
// shows except the St/Clutter rendering in extension.js, which needs a
// running gnome-shell.

import System from 'system';
import {buildModel, resolveArgv, resolveOutcome, usage, humanSize, choiceLabel} from
    '../../greeter-extension/nostr-home-status@nostrc/model.js';

let failures = 0;
function check(cond, what) {
    if (!cond) {
        failures++;
        printerr(`FAIL: ${what}`);
    }
}
function eq(a, b, what) {
    check(a === b, `${what}: expected ${JSON.stringify(b)}, got ${JSON.stringify(a)}`);
}

const HOME_ID = '0123456789abcdef0123456789abcdef';
const VAULT_ID = 'aa'.repeat(32);
const DEL_ID = 'bb'.repeat(32);
const WRITER = '3f2a1b0c' + '9'.repeat(56);

// 1. Missing / malformed status: unavailable, never confident defaults.
for (const doc of [null, [], 'x', {}, {schema: 1}]) {
    const m = buildModel(doc);
    eq(m.syncText, 'Status unavailable', `syncText for ${JSON.stringify(doc)}`);
    eq(m.conflicts.length, 0, 'no conflicts invented');
    eq(m.cache, null, 'no cache figure invented');
    eq(m.glyph, '?', 'unknown glyph');
}
eq(buildModel({}).available, false, 'empty doc is not available');

// 2. Every state syncd actually emits gets its own text (the old
// indicator expected ready/partial, which syncd never writes).
const states = {
    idle: 'Up to date', pulling: 'Downloading changes…', pushing: 'Uploading changes…',
    reconciling: 'Applying changes…', offline: 'Offline: changes will sync when the connection returns',
};
for (const [s, text] of Object.entries(states))
    eq(buildModel({syncd: {state: s}}).syncText, text, `state ${s}`);
eq(buildModel({syncd: {state: 'idle'}}).glyph, '●', 'idle glyph');
eq(buildModel({syncd: {state: 'idle', last_error_class: 'upload-failed'}}).syncText,
    'Needs attention: uploading to the storage servers failed', 'idle + error');
eq(buildModel({syncd: {state: 'error', last_error_class: 'vault-auth'}}).syncText,
    'Sync problem: access to a shared folder was refused', 'error text');
eq(buildModel({syncd: {state: 'ready'}}).syncText, 'Status unavailable', 'unknown state');

// 3. Quota: cache_quota_bytes (the field syncd writes), clamped bar,
// numeric percentage may exceed 100.
let m = buildModel({syncd: {state: 'idle', cache_bytes: 3 * 2 ** 30, cache_quota_bytes: 10 * 2 ** 30}});
eq(m.cache.text, 'Cache: 3.0 GiB of 10.0 GiB (30%)', 'cache text');
eq(m.cache.fraction, 0.3, 'cache fraction');
m = buildModel({syncd: {state: 'idle', cache_bytes: 12 * 2 ** 30, cache_quota_bytes: 10 * 2 ** 30}});
eq(m.cache.fraction, 1, 'over-quota bar clamps');
eq(m.cache.percent, 120, 'over-quota percent kept');
check(m.cache.over && m.cache.text.endsWith('over the limit'), 'over-quota is said in words');
eq(buildModel({syncd: {state: 'idle', cache_bytes: 5, quota_bytes: 9}}).cache.fraction, null,
    'legacy quota_bytes is not read');
eq(usage('Home size', 512, 0).text, 'Home size: 512 bytes', 'no limit -> plain size');
eq(humanSize(-1), '—', 'negative size');
m = buildModel({provisioner: {state: 'done', home_bytes: 2 ** 30, home_quota_bytes: 20 * 2 ** 30}});
eq(m.home.text, 'Home size: 1.0 GiB of 20.0 GiB (5%)', 'home size vs limit');

// 4. Provisioning progress.
m = buildModel({syncd: {state: 'idle'}, provisioner: {state: 'fetching', chunks_done: 30, chunks_pending: 90}});
check(m.provision.active, 'fetching is active');
eq(m.provision.text, 'Loading your home: 30 of 120 files', 'progress text');
eq(m.provision.fraction, 0.25, 'progress fraction');
eq(m.label, 'loading 25%', 'panel label while loading');
check(m.accessibleName.includes('Loading your home: 30 of 120 files'), 'accessible name has progress');
m = buildModel({provisioner: {state: 'preparing'}});
eq(m.provision.text, 'Loading your home…', 'progress without counts');
eq(m.provision.fraction, null, 'no bar without a total');
m = buildModel({provisioner: {state: 'error', last_error_class: 'network'}});
check(!m.provision.active && m.provision.text.startsWith('The last home download did not finish'),
    'provisioner error text');

// 5. Conflicts from both sources, with the choices syncd offers.
const doc = {syncd: {
    state: 'idle',
    home_conflicts: [
        {id: HOME_ID, path: 'Documents/report.txt', kind: 'edit', present: true,
            current_present: true, state: 'active', mine: 'saved',
            choices: ['keep-current', 'use-saved', 'keep-both'],
            destination: 'Documents/report.txt.conflict-laptop-20261028T101500Z'},
        {id: 'nothex', path: 'x', choices: []},           // dropped
    ],
    vault_conflicts: [
        {id: VAULT_ID, path: 'Portable/Team/plan.md', writer: WRITER, present: true,
            current_present: true, state: 'active', mine: 'current', overflow: true,
            choices: ['keep-current', 'use-saved', 'keep-both'],
            destination: 'Portable/.conflicts/.overflow/abcd'},
        {id: DEL_ID, path: 'Portable/Team/gone.txt', writer: WRITER, present: false,
            current_present: true, state: 'pending', choices: ['keep-current', 'use-saved']},
    ],
}};
m = buildModel(doc);
eq(m.conflicts.length, 3, 'malformed conflict dropped');
eq(m.conflictText, '3 conflicts need your attention', 'conflict header');
eq(m.label, 'synced · 3 conflicts', 'panel label shows conflicts');
const [home, vault, gone] = m.conflicts;
eq(home.title, 'report.txt', 'title');
eq(home.folder, 'Documents', 'folder');
check(home.description.includes('saved at Documents/report.txt.conflict-laptop') &&
      home.description.endsWith('The saved version is yours.'), 'home description');
eq(home.actions.map(a => a.label).join('|'),
    'Keep the current version|Use the saved version|Keep both (add the saved version as a copy)',
    'edit labels');
eq(home.showFolder, 'Documents', 'show folder of saved copy');
check(vault.description.includes('.overflow/abcd'), 'overflow destination shown verbatim');
eq(vault.showFolder, 'Portable/.conflicts/.overflow', 'overflow folder');
check(gone.description.startsWith('This file was deleted by another member'), 'deletion text');
eq(gone.actions.length, 0, 'pending conflict offers no actions');
check(gone.description.endsWith('waiting to be shared.'), 'pending is explained');
eq(gone.showFolder, 'Portable/Team', 'deletion-only shows the original folder');
eq(choiceLabel({savedPresent: false, currentPresent: true}, 'use-saved'), 'Delete the file',
    'deletion use-saved label');
eq(choiceLabel({savedPresent: true, currentPresent: false}, 'keep-current'), 'Leave it removed',
    'removed keep-current label');

// 6. The command run for a choice, and only for offered choices.
eq(resolveArgv(home, 'keep-both').join(' '),
    `nostr-home-status conflicts resolve ${HOME_ID} --keep-both --timeout 30`, 'argv');
let threw = false;
try { resolveArgv(gone, 'keep-current'); } catch (_e) { threw = true; }
check(threw, 'pending conflict cannot be resolved again');
threw = false;
try { resolveArgv(home, 'rm -rf'); } catch (_e) { threw = true; }
check(threw, 'unknown choice refused');

// 7. CLI outcome -> notification.
check(resolveOutcome(0, 'Sent...\nDone. The conflict is resolved and the change has been shared.\n').ok,
    'exit 0 ok');
eq(resolveOutcome(3, '').title, 'Choice applied on this device', 'exit 3 pending');
const bad = resolveOutcome(1, 'Not resolved: The file changed after the conflict was listed.');
check(!bad.ok && bad.body.startsWith('Not resolved'), 'exit 1 error with CLI text');

if (failures) {
    printerr(`${failures} check(s) failed`);
    System.exit(1);
}
print('PASS test_status_extension_model');
