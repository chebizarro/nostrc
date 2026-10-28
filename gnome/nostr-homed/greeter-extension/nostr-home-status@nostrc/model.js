// nostr-home-status@nostrc — view model.
//
// Pure functions from porthome-status.json to what the panel shows. No
// GNOME imports, so the logic runs (and is tested) headless under gjs:
// tests/unit/test_status_extension_model.js. extension.js only renders.
//
// Only well-known keys are read; anything malformed degrades to
// "unavailable" rather than a confident-looking default.
//
// Bead: nostrc-h10m.6.

const SYNC_TEXT = {
    idle: 'Up to date',
    pulling: 'Downloading changes…',
    pushing: 'Uploading changes…',
    reconciling: 'Applying changes…',
    limited: 'Limited mode: changes stay on this device for now',
    offline: 'Offline: changes will sync when the connection returns',
    error: 'Sync problem',
};

const SYNC_SHORT = {
    idle: 'synced',
    pulling: 'syncing',
    pushing: 'syncing',
    reconciling: 'syncing',
    limited: 'limited',
    offline: 'offline',
    error: 'problem',
};

const ERROR_TEXT = {
    'cache-thrashing': 'the local cache is too small for this home',
    'insufficient-replication': 'not enough storage servers accepted the upload',
    'upload-failed': 'uploading to the storage servers failed',
    'push-failed': 'sharing changes failed',
    'vault-auth': 'access to a shared folder was refused',
    'vault-epoch': 'a shared folder changed its keys; waiting to catch up',
    'vault-incomplete': 'a shared folder is still catching up',
    'vault-budget': 'a shared folder hit its storage budget',
    'vault-sync': 'a shared folder could not sync',
};

const PROVISIONING = new Set(['preparing', 'fetching', 'verifying', 'publishing']);

function isCount(n) {
    return typeof n === 'number' && Number.isFinite(n) && n >= 0;
}

export function humanSize(n) {
    if (!isCount(n)) return '—';
    if (n < 1024) return `${n} bytes`;
    const units = ['KiB', 'MiB', 'GiB', 'TiB'];
    let v = n / 1024;
    let u = 0;
    while (v >= 1024 && u < units.length - 1) {
        v /= 1024;
        u++;
    }
    return `${v.toFixed(1)} ${units[u]}`;
}

// used/limit -> {text, fraction (0..1 for the bar), percent (may exceed 100)}
export function usage(label, used, limit) {
    if (!isCount(used)) return null;
    if (!isCount(limit) || limit === 0)
        return {text: `${label}: ${humanSize(used)}`, fraction: null, percent: null, over: false};
    const percent = Math.floor((used * 100) / limit);
    return {
        text: `${label}: ${humanSize(used)} of ${humanSize(limit)} (${percent}%)` +
              (used > limit ? ' — over the limit' : ''),
        fraction: Math.min(1, used / limit),
        percent,
        over: used > limit,
    };
}

function errorText(slug) {
    if (typeof slug !== 'string' || slug.length === 0) return '';
    return ERROR_TEXT[slug] ?? slug.replace(/-/g, ' ');
}

function str(v) {
    return typeof v === 'string' && v.length > 0 ? v : null;
}

const CHOICE_ORDER = ['keep-current', 'use-saved', 'keep-both'];

export function choiceLabel(c, choice) {
    const deletion = !c.savedPresent && c.currentPresent;
    const removed = c.savedPresent && !c.currentPresent;
    switch (choice) {
    case 'keep-current':
        if (c.wholeFolder) return 'Keep the folder as it is now';
        return deletion ? 'Keep the file' : removed ? 'Leave it removed' : 'Keep the current version';
    case 'use-saved':
        return deletion ? 'Delete the file' : removed ? 'Restore the saved version'
            : 'Use the saved version';
    case 'keep-both':
        return 'Keep both (add the saved version as a copy)';
    }
    return choice;
}

export function describe(c) {
    const who = c.source === 'vault'
        ? (c.writer ? `another member (${c.writer.slice(0, 8)})` : 'another member')
        : 'another device';
    const saved = c.savedPath ? ` at ${c.savedPath}` : '';
    if (c.wholeFolder) {
        return 'Changes made here could not be shared because the folder\'s sharing settings ' +
               `changed first. They are kept in ${c.savedPath ?? 'a saved copy'} so you can copy ` +
               'back anything you still need.';
    }
    if (c.kind === 'deleted-locally')
        return `You deleted this file here, but it was changed on ${who}, so it was put back.`;
    if (c.kind === 'deleted-remotely')
        return `This file was deleted on ${who}, but it was changed here.`;
    if (!c.savedPresent && c.currentPresent) {
        const by = c.mine === 'saved' ? 'here'
            : c.source === 'vault' ? 'by another member' : 'on another device';
        const where = c.mine === 'current' ? 'changed here' : 'changed in the current version';
        return `This file was deleted ${by}, but it was ${where}.`;
    }
    if (c.savedPresent && !c.currentPresent) {
        return `This file was added or changed on ${who} but is not in the current version. ` +
               `That version is saved${saved}.` +
               (c.mine === 'saved' ? ' The saved version is yours.' : '');
    }
    const yours = c.mine === 'current' ? ' The current version is yours.'
        : c.mine === 'saved' ? ' The saved version is yours.' : '';
    return 'This file was changed in two places at the same time. The current version is in ' +
           `place; the other version is saved${saved}.${yours}`;
}

function conflictFrom(row, source) {
    if (row === null || typeof row !== 'object') return null;
    const id = str(row.id);
    const path = str(row.path);
    const idOk = id && /^[0-9a-f]+$/.test(id) && id.length === (source === 'home' ? 32 : 64);
    if (!idOk || !path) return null;
    const c = {
        id,
        source,
        path,
        savedPath: str(row.destination),
        marker: str(row.marker),
        kind: str(row.kind),
        writer: str(row.writer),
        savedPresent: row.present === true,
        currentPresent: row.current_present !== false,
        overflow: row.overflow === true,
        wholeFolder: row.root_snapshot === true,
        pending: row.state === 'pending',
        mine: row.mine === 'current' || row.mine === 'saved' ? row.mine : null,
    };
    const offered = Array.isArray(row.choices) ? row.choices : [];
    const choices = CHOICE_ORDER.filter(ch => offered.includes(ch));
    if (choices.length === 0) choices.push('keep-current');
    c.actions = c.pending ? [] : choices.map(choice => ({choice, label: choiceLabel(c, choice)}));
    const slash = path.lastIndexOf('/');
    c.title = slash >= 0 ? path.slice(slash + 1) : path;
    c.folder = slash >= 0 ? path.slice(0, slash) : '';
    c.description = describe(c) +
        (c.pending ? ' Your choice is applied here and waiting to be shared.' : '');
    // Where "Show in Files" should land: the saved copy's folder, else the original's.
    const target = c.savedPath ?? path;
    const cut = target.lastIndexOf('/');
    c.showFolder = cut >= 0 ? target.slice(0, cut) : '';
    return c;
}

// argv for the CLI that asks nostr-home-syncd to apply the choice. The
// extension never edits HOME itself; syncd publishes the result.
export function resolveArgv(conflict, choice, timeoutSeconds = 30) {
    if (!conflict || !conflict.actions.some(a => a.choice === choice))
        throw new Error(`choice ${choice} is not offered for this conflict`);
    return ['nostr-home-status', 'conflicts', 'resolve', conflict.id, `--${choice}`,
        '--timeout', String(timeoutSeconds)];
}

// Exit status of `nostr-home-status conflicts resolve` -> notification.
export function resolveOutcome(exitStatus, output) {
    const lines = (output ?? '').split('\n').map(l => l.trim()).filter(Boolean);
    const last = lines.length ? lines[lines.length - 1] : '';
    switch (exitStatus) {
    case 0: return {ok: true, title: 'Conflict resolved', body: last || 'The change has been shared.'};
    case 3: return {ok: true, title: 'Choice applied on this device', body: last ||
            'It will be shared when sync completes.'};
    default: return {ok: false, title: 'Could not resolve the conflict', body: last ||
            'The sync service could not apply the change.'};
    }
}

export function buildModel(doc) {
    const valid = doc !== null && typeof doc === 'object' && !Array.isArray(doc);
    const syncd = valid && doc.syncd && typeof doc.syncd === 'object' ? doc.syncd : null;
    const prov = valid && doc.provisioner && typeof doc.provisioner === 'object'
        ? doc.provisioner : null;

    const state = syncd && typeof syncd.state === 'string' && SYNC_TEXT[syncd.state]
        ? syncd.state : null;
    const err = errorText(syncd?.last_error_class);
    let syncText = state ? SYNC_TEXT[state] : 'Status unavailable';
    if (state === 'idle' && err) syncText = `Needs attention: ${err}`;
    else if (state === 'error' && err) syncText = `Sync problem: ${err}`;

    const provState = typeof prov?.state === 'string' ? prov.state : null;
    let provision = {active: false, text: null, fraction: null};
    if (provState && PROVISIONING.has(provState)) {
        const done = isCount(prov.chunks_done) ? prov.chunks_done : 0;
        const pending = isCount(prov.chunks_pending) ? prov.chunks_pending : 0;
        const total = done + pending;
        provision = {
            active: true,
            text: total > 0
                ? `Loading your home: ${done} of ${total} files`
                : done > 0 ? `Loading your home: ${done} files so far` : 'Loading your home…',
            fraction: total > 0 ? Math.min(1, done / total) : null,
        };
    } else if (provState === 'error') {
        const why = errorText(prov.last_error_class);
        provision.text = `The last home download did not finish${why ? ` (${why})` : ''}.`;
    }

    const conflicts = [];
    for (const [key, source] of [['home_conflicts', 'home'], ['vault_conflicts', 'vault']]) {
        const rows = Array.isArray(syncd?.[key]) ? syncd[key] : [];
        for (const row of rows) {
            const c = conflictFrom(row, source);
            if (c) conflicts.push(c);
        }
    }

    const cache = usage('Cache', syncd?.cache_bytes, syncd?.cache_quota_bytes);
    const home = usage('Home size', prov?.home_bytes, prov?.home_quota_bytes);

    const n = conflicts.length;
    const conflictText = n === 0 ? 'No conflicts'
        : `${n} conflict${n === 1 ? '' : 's'} need${n === 1 ? 's' : ''} your attention`;
    let glyph = '?';
    let label = 'home';
    if (provision.active) {
        glyph = '◐';
        label = provision.fraction !== null
            ? `loading ${Math.floor(provision.fraction * 100)}%` : 'loading';
    } else if (state) {
        glyph = state === 'idle' && !err ? '●'
            : state === 'error' || err ? '!'
                : state === 'limited' || state === 'offline' ? '○' : '◐';
        label = SYNC_SHORT[state];
    }
    if (n > 0) label = `${label} · ${n} conflict${n === 1 ? '' : 's'}`;

    const accessibleName = ['Portable home', provision.active ? provision.text : syncText,
        n > 0 ? conflictText : null].filter(Boolean).join(', ');

    return {
        available: !!syncd || !!prov,
        glyph,
        label,
        accessibleName,
        syncText,
        provision,
        cache,
        home,
        conflicts,
        conflictText,
    };
}
