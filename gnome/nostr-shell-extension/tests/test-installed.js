// SPDX-License-Identifier: MIT
// Package completeness, not live Shell acceptance: UI modules are parsed, while
// Gio-only monitors are actually imported without constructing/activating them.
import Gio from 'gi://Gio';

function assert(condition, message) {
    if (!condition)
        throw new Error(message);
}

assert(ARGV.length === 2, 'usage: test-installed.js EXTENSION_DIR SCHEMA_DIR');
const root = Gio.File.new_for_path(ARGV[0]);
function read(file) {
    const [, bytes] = file.load_contents(null);
    return new TextDecoder().decode(bytes);
}

const metadata = JSON.parse(read(root.get_child('metadata.json')));
assert(metadata.uuid === 'nostr@nostr.org', 'wrong installed extension UUID');
// Never inherit the host schema source: it could hide an incomplete package.
const schemas = Gio.SettingsSchemaSource.new_from_directory(ARGV[1], null, false);
const schema = schemas.lookup(metadata['settings-schema'], false);
assert(schema !== null, 'installed compiled schema is missing');
assert(schema.has_key('refresh-interval'), 'installed schema lacks refresh-interval');

const visited = new Set();
function checkModule(file) {
    const uri = file.get_uri();
    if (visited.has(uri))
        return;
    visited.add(uri);
    const ast = Reflect.parse(read(file), {target: 'module'});
    for (const node of ast.body) {
        const specifier = node.moduleRequest?.source.value;
        if (typeof specifier !== 'string' || !specifier.startsWith('.'))
            continue;
        const dependency = file.get_parent().resolve_relative_path(specifier);
        assert(root.get_relative_path(dependency) !== null,
            `import escapes installed extension: ${specifier}`);
        checkModule(dependency);
    }
}
checkModule(root.get_child('extension.js'));
checkModule(root.get_child('prefs.js'));
for (const name of ['nostr-relay', 'nostr-signer-ready', 'nostr-signer-waiting',
    'nostr-signer-paused', 'nostr-signer-unavailable']) {
    assert(root.get_child(`icons/${name}-symbolic.svg`).query_exists(null),
        `missing installed icon: ${name}`);
}

// These real modules include the signer entrypoint omitted by the old state-only
// test. Missing imports, named-export mismatches and evaluation errors fail here.
for (const name of ['relay', 'dm', 'wallet', 'signer']) {
    const module = await import(root.get_child(`lib/${name}.js`).get_uri());
    assert(Object.keys(module).length > 0, `empty monitor module: ${name}`);
}
print(`installed extension: ${visited.size} modules parsed, four monitors imported, compiled schema found`);
