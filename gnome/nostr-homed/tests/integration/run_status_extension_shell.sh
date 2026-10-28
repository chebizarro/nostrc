#!/bin/sh
# nostrc-h10m.6 — run nostr-home-status@nostrc inside a real, headless
# GNOME Shell (mutter --headless, virtual monitor) and check what it shows.
#
# A probe extension loaded next to it reads the indicator, opens the menu
# and the first conflict submenu, screenshots the stage, then activates a
# resolution choice; a stand-in `nostr-home-status` on PATH records the
# argv the extension runs. Results: $OUT/probe.json, $OUT/menu.png.
#
# Opt-in: exits 77 (skip) when gnome-shell/dbus-run-session/gsettings are
# missing. Not a substitute for a logged-in session: no real syncd runs.
#
# usage: run_status_extension_shell.sh EXTENSION_DIR [OUT_DIR]
set -eu
EXT_SRC=$1
OUT=${2:-$(mktemp -d /tmp/nh-shell-ext-XXXXXX)}
for tool in gnome-shell dbus-run-session gsettings; do
    command -v "$tool" >/dev/null 2>&1 || { echo "SKIP: $tool not installed"; exit 77; }
done
mkdir -p "$OUT"
SANDBOX=$(mktemp -d /tmp/nh-shell-home-XXXXXX)
export HOME="$SANDBOX/home" XDG_RUNTIME_DIR="$SANDBOX/run"
export XDG_CONFIG_HOME="$HOME/.config" XDG_DATA_HOME="$HOME/.local/share"
export XDG_STATE_HOME="$HOME/.local/state" XDG_CACHE_HOME="$HOME/.cache"
mkdir -p "$HOME" "$XDG_RUNTIME_DIR" "$XDG_STATE_HOME/nostr-homed" "$SANDBOX/bin"
chmod 700 "$XDG_RUNTIME_DIR"
EXTS="$XDG_DATA_HOME/gnome-shell/extensions"
mkdir -p "$EXTS/nostr-home-status@nostrc" "$EXTS/nh-probe@nostrc"
cp "$EXT_SRC"/metadata.json "$EXT_SRC"/extension.js "$EXT_SRC"/model.js \
   "$EXT_SRC"/stylesheet.css "$EXTS/nostr-home-status@nostrc/"

# Stand-in CLI: record argv, answer like a successful resolution.
cat > "$SANDBOX/bin/nostr-home-status" <<EOF
#!/bin/sh
printf '%s\n' "\$@" > "$OUT/argv.txt"
echo "Done. The conflict is resolved and the change has been shared."
EOF
chmod +x "$SANDBOX/bin/nostr-home-status"

H=0123456789abcdef0123456789abcdef
V=$(printf 'a%.0s' $(seq 64))
W=3f2a1b0c$(printf '9%.0s' $(seq 56))
cat > "$XDG_STATE_HOME/nostr-homed/porthome-status.json" <<EOF
{"schema":1,
 "provisioner":{"state":"done","home_bytes":1073741824,"home_quota_bytes":21474836480},
 "syncd":{"state":"idle","last_error_class":"","cache_bytes":3221225472,
  "cache_quota_bytes":10737418240,"conflicts_active_count":2,
  "home_conflicts":[{"source":"home","id":"$H","path":"Documents/report.txt","kind":"edit",
   "present":true,"current_present":true,"state":"active","mine":"saved",
   "choices":["keep-current","use-saved","keep-both"],"expect":"t",
   "destination":"Documents/report.txt.conflict-laptop-20261028T101500Z"}],
  "vault_conflicts":[{"source":"vault","id":"$V","path":"Portable/Team/gone.txt","writer":"$W",
   "present":false,"current_present":true,"state":"active","mine":null,
   "choices":["keep-current","use-saved"],"expect":"t"}]}}
EOF

cat > "$EXTS/nh-probe@nostrc/metadata.json" <<'EOF'
{"uuid":"nh-probe@nostrc","name":"probe","description":"test probe","shell-version":["46","47"]}
EOF
cat > "$EXTS/nh-probe@nostrc/extension.js" <<EOF
import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import Shell from 'gi://Shell';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
const OUT = '$OUT';
function save(name, text) { GLib.file_set_contents(\`\${OUT}/\${name}\`, text); }
export default class Probe extends Extension {
    enable() {
        GLib.timeout_add(GLib.PRIORITY_DEFAULT, 3000, () => { this._run(); return GLib.SOURCE_REMOVE; });
    }
    disable() {}
    _run() {
        const r = {loaded: false};
        try {
            const i = Main.panel.statusArea['nostr-home-status'];
            r.loaded = !!i;
            if (i) {
                r.glyph = i._icon.text; r.label = i._label.text; r.accessible = i.accessible_name;
                i.menu.open(false);
                r.items = i.menu._getMenuItems().filter(it => it.label || it._label || it.label_actor)
                    .map(it => (it.label?.text ?? it.label_actor?.text ?? ''));
                const subs = i._conflictSection._getMenuItems();
                r.conflicts = subs.map(s => s.label.text);
                subs[0].setSubmenuShown(true);
                r.firstConflict = subs[0].menu._getMenuItems().map(it => it.label?.text ?? '');
                r.sensitive = subs[0].menu._getMenuItems().map(it => it.getSensitive?.() ?? null);
            }
        } catch (e) { r.error = \`\${e}\n\${e.stack}\`; }
        save('probe.json', JSON.stringify(r, null, 1));
        GLib.timeout_add(GLib.PRIORITY_DEFAULT, 800, () => {
            const shot = new Shell.Screenshot();
            const file = Gio.File.new_for_path(\`\${OUT}/menu.png\`);
            const stream = file.replace(null, false, Gio.FileCreateFlags.NONE, null);
            shot.screenshot(false, stream, (s, res) => {
                try { s.screenshot_finish(res); } catch (e) { save('shot-error.txt', \`\${e}\`); }
                stream.close(null);
                // Now act: choose the first offered action of the first conflict.
                try {
                    const i = Main.panel.statusArea['nostr-home-status'];
                    const c = i._model.conflicts[0];
                    i._resolve(c, c.actions[0].choice);
                } catch (e) { save('act-error.txt', \`\${e}\`); }
            });
            return GLib.SOURCE_REMOVE;
        });
    }
}
EOF

export PATH="$SANDBOX/bin:$PATH"
dbus-run-session -- sh -c '
  gsettings set org.gnome.shell disable-user-extensions false
  gsettings set org.gnome.shell enabled-extensions "[\"nostr-home-status@nostrc\", \"nh-probe@nostrc\"]"
  timeout 40 gnome-shell --headless --wayland --no-x11 --virtual-monitor 1280x800 \
      > "'"$OUT"'/shell.log" 2>&1 || true' || true

rm -rf "$SANDBOX"
[ -f "$OUT/probe.json" ] || { echo "FAIL: probe did not run"; tail -40 "$OUT/shell.log"; exit 1; }
if grep -q "nostr-home-status@nostrc" "$OUT/shell.log" && grep -qi "error" "$OUT/shell.log"; then
    grep -i "nostr-home-status" "$OUT/shell.log" | head -20
fi
python3 - "$OUT" <<'PY'
import json, sys
out = sys.argv[1]
r = json.load(open(f"{out}/probe.json"))
assert r.get("loaded") and "error" not in r, r
assert r["glyph"] == "●" and r["label"] == "synced · 2 conflicts", r
assert "2 conflicts need your attention" in r["accessible"], r
items = " | ".join(r["items"])
for want in ("Up to date", "Cache: 3.0 GiB of 10.0 GiB (30%)",
             "Home size: 1.0 GiB of 20.0 GiB (5%)", "2 conflicts need your attention"):
    assert want in items, (want, items)
assert r["conflicts"] == ["report.txt — Documents", "gone.txt — Portable/Team"], r
fc = r["firstConflict"]
assert fc[0].startswith("This file was changed in two places"), fc
assert fc[1:4] == ["Keep the current version", "Use the saved version",
                   "Keep both (add the saved version as a copy)"], fc
argv = open(f"{out}/argv.txt").read().split()
assert argv == ["conflicts", "resolve", "0123456789abcdef0123456789abcdef",
                "--keep-current", "--timeout", "30"], argv
print("PASS headless gnome-shell: indicator, usage, conflict menu and resolve argv")
PY
echo "artifacts: $OUT"
