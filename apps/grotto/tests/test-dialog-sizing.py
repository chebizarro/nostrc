#!/usr/bin/env python3
"""Guard production dialog sizing, including committed generated UI (no GTK needed).

BLP/C are tokenized, not searched line-by-line: comments, strings, nested blocks,
casts and multiline calls cannot accidentally satisfy a neighbouring object's rule.
XML is parsed with source locations. Each diagnostic is file:line.

Exceptions are deliberately narrow and live here, not inline suppression comments:
* GtkShortcutsWindow uses GTK's built-in paginated, content-sized layout.
* AdwAlertDialog/AdwAboutDialog/AdwPreferencesDialog own their default root
  dimensions; application-owned child subtrees do NOT inherit this exception.
* C under tests/ is fixture code, not shipped UI. All other C in both components
  is inspected, including new source directories.
Ordinary application-window pages may fill their viewport. Scroller propagation
is required for dialogs, not every scroller in the main window or assistant.
AdwPreferencesPage's private scroller already propagates natural height. Its
host dialog must follow content size (asynchronous inventory/recovery content)
or supply an explicit height. Subclasses backed by templates are checked in markup.
"""
import re
import json
import tempfile
import sys
import unittest
from dataclasses import dataclass, field
from pathlib import Path
from xml.parsers import expat

# Genuine toolkit-owned exceptions, keyed by class (not whole source files).
AUTO_SIZED = {
    "GtkShortcutsWindow": "GTK owns the paginated shortcuts window's size",
    "AdwAlertDialog": "libadwaita measures the message and response area",
    "AdwAboutDialog": "libadwaita owns the About dialog layout",
    "AdwPreferencesDialog": "libadwaita owns its page viewport and width",
}
TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_$](?:[\w$.]|-(?!>))*|\d+|->|[^\s]', re.S)


def tokens(text):
    return [(m.group(), text.count("\n", 0, m.start()) + 1) for m in TOKEN.finditer(text)
            if not m.group().startswith(("//", "/*"))]


@dataclass
class Node:
    kind: str
    name: str
    line: int
    props: dict = field(default_factory=dict)
    children: list = field(default_factory=list)


def blueprint(text):
    ts = tokens(text)
    root = Node("root", "", 1)
    stack = [root]
    for i, (t, line) in enumerate(ts):
        if t == "{":
            # Object declarations end with 'Type [id] {'; property blocks like
            # accessibility do not introduce an independent sized widget.
            prev = ts[i - 1][0]
            kind = ts[i - 2][0] if i > 1 and re.fullmatch(r'[\w$]+', prev) and re.fullmatch(r'[A-Z][\w.]*', ts[i - 2][0]) else prev
            name = prev if kind != prev else ""
            if i > 2 and ts[i - 2][0] == ":" and ts[i - 3][0].startswith("$"):
                name = ts[i - 3][0]
            kind = kind.replace("Adw.", "Adw").replace("Gtk.", "Gtk")
            if kind in ("ScrolledWindow", "ShortcutsWindow", "Window", "ApplicationWindow"):
                kind = "Gtk" + kind
            node = Node(kind, name, line)
            stack[-1].children.append(node)
            stack.append(node)
        elif t == "}":
            stack.pop()
        elif t == ":" and i and i + 1 < len(ts):
            stack[-1].props[ts[i - 1][0]] = ts[i + 1][0]
    return root


def xml_ui(text):
    parser = expat.ParserCreate()
    root = Node("root", "", 1)
    stack = [root]
    prop = None
    def start(tag, attrs):
        nonlocal prop
        if tag in ("object", "template"):
            prop = None
            node = Node(attrs.get("parent", attrs.get("class", "")), attrs.get("class", "") if tag == "template" else attrs.get("id", ""), parser.CurrentLineNumber)
            stack[-1].children.append(node)
            stack.append(node)
        elif tag == "property":
            prop = attrs["name"]
            stack[-1].props[prop] = ""
    def end(tag):
        nonlocal prop
        if tag in ("object", "template"):
            stack.pop()
        elif tag == "property":
            prop = None
    def data(value):
        if prop:
            stack[-1].props[prop] += value.strip()
    parser.StartElementHandler, parser.EndElementHandler, parser.CharacterDataHandler = start, end, data
    parser.Parse(text, True)
    return root


def positive(value):
    return bool(re.fullmatch(r'[1-9]\d*', str(value)))


def markup_errors(root):
    errors = []
    def visit(node, dialog=False, height=False, follows=False, toolkit_page=False):
        # Only the toolkit root's default width/height is exempt. Application
        # children (especially AlertDialog.extra-child) are always inspected.
        if node.kind in ("AdwDialog", "AdwAlertDialog", "AdwAboutDialog", "AdwPreferencesDialog"):
            dialog, height = True, False
            follows = node.props.get("follows-content-size", "").lower() in ("true", "1")
            toolkit_page = node.kind == "AdwPreferencesDialog"
            if node.kind not in AUTO_SIZED and not positive(node.props.get("content-width")):
                errors.append((node.line, "Adw.Dialog lacks positive content-width"))
        if node.kind.endswith("Window") and node.kind not in AUTO_SIZED and node.kind not in ("ScrolledWindow", "GtkScrolledWindow"):
            for dim in ("default-width", "default-height"):
                if not positive(node.props.get(dim)):
                    errors.append((node.line, f"{node.kind} lacks positive {dim}"))
        height = height or positive(node.props.get("content-height")) or positive(node.props.get("height-request"))
        if dialog and node.kind in ("ScrolledWindow", "GtkScrolledWindow") and not height:
            if node.props.get("propagate-natural-height", "").lower() not in ("true", "1"):
                errors.append((node.line, "dialog ScrolledWindow lacks propagate-natural-height or explicit height"))
        if dialog and node.kind == "AdwPreferencesPage" and not (height or follows or toolkit_page):
            errors.append((node.line, "dialog PreferencesPage needs follows-content-size or explicit height"))
        for child in node.children:
            visit(child, dialog, height, follows, toolkit_page)
    visit(root)
    return errors


def arguments(ts, start):
    depth, result, current = 1, [], []
    for i in range(start + 1, len(ts)):
        t = ts[i][0]
        if t == "(": depth += 1
        if t == ")": depth -= 1
        if not depth:
            return result + [current], i
        if t == "," and depth == 1:
            result.append(current); current = []
        else:
            current.append(t)
    raise ValueError("unbalanced C call")


def ref(arg):
    # Strip GTK cast macros, retaining self->field rather than just 'self'.
    names = [t for t in arg if re.fullmatch(r'[a-z_]\w*', t)]
    return "->".join(names)


def c_errors(text, templates=(), sites=None):
    ts = tokens(text)
    declarations = TOKEN.sub(lambda m: "\n" * m.group().count("\n") if m.group().startswith(("//", "/*", '"', "'")) else m.group(), text)
    subclasses = list(re.finditer(r'G_DEFINE_(?:FINAL_)?TYPE(?:_WITH_(?:PRIVATE|CODE))?\s*\(\s*(\w+)\s*,\s*(\w+)\s*,\s*((?:ADW|GTK)_TYPE_(?:DIALOG|WINDOW|APPLICATION_WINDOW))\s*[,)]', declarations))
    errors, functions, body, depth = [], [], [], 0
    # File-level function bodies are independent sizing scopes. Retain nested
    # blocks within them (if/loop branches often build content).
    names, owners = {}, {}
    for index, token in enumerate(ts):
        if token[0] == "{":
            depth += 1
            if depth == 1:
                body = []
                name = ""
                if index and ts[index - 1][0] == ")":
                    nesting, j = 1, index - 2
                    while j >= 0 and nesting:
                        if ts[j][0] == ")": nesting += 1
                        elif ts[j][0] == "(": nesting -= 1
                        j -= 1
                    if j >= 0: name = ts[j][0]
        if depth: body.append(token)
        if token[0] == "}":
            depth -= 1
            if not depth:
                functions.append(body)
                names[name] = body
                owners[id(body)] = name
    all_calls = {}
    for body in functions:
        calls = []
        for i, (t, line) in enumerate(body[:-1]):
            if re.fullmatch(r'[a-z_]\w*', t) and body[i + 1][0] == "(":
                args, _ = arguments(body, i + 1)
                target = ""
                # Constructors may be wrapped in a type cast.
                j = i - 1
                while j >= 0 and body[j][0] not in ("=", ";", "{", "}"): j -= 1
                if j >= 0 and body[j][0] == "=":
                    k = j - 1
                    while k >= 0 and (re.fullmatch(r'[a-z_]\w*', body[k][0]) or body[k][0] == "->"): k -= 1
                    target = ref([x[0] for x in body[k + 1:j]])
                calls.append((t, args, target, line))
        all_calls[id(body)] = calls
        def constructs(fn, args, factory, type_name):
            return fn == factory or (fn == "g_object_new" and args and args[0] == [type_name])
        toolkit_factories = {"adw_alert_dialog_new": "ADW_TYPE_ALERT_DIALOG",
                             "adw_about_dialog_new": "ADW_TYPE_ABOUT_DIALOG",
                             "adw_preferences_dialog_new": "ADW_TYPE_PREFERENCES_DIALOG"}
        dialogs = [(target, line, False) for fn, args, target, line in calls if constructs(fn, args, "adw_dialog_new", "ADW_TYPE_DIALOG")]
        dialogs += [(target, line, True) for fn, args, target, line in calls
                    if any(constructs(fn, args, factory, kind) for factory, kind in toolkit_factories.items())]
        windows = [(target, line) for fn, args, target, line in calls
                   if constructs(fn, args, "adw_window_new", "ADW_TYPE_WINDOW") or
                   constructs(fn, args, "adw_application_window_new", "ADW_TYPE_APPLICATION_WINDOW") or
                   constructs(fn, args, "gtk_window_new", "GTK_TYPE_WINDOW") or
                   constructs(fn, args, "gtk_application_window_new", "GTK_TYPE_APPLICATION_WINDOW")]
        if sites is not None:
            custom = any(fn.endswith(("_set_extra_child", "_set_child", "_set_content")) or
                         (fn == "g_object_new" and any(args[i] in ([ '"child"' ], [ '"content"' ], [ '"extra-child"' ])
                                                      for i in range(1, len(args) - 1, 2)))
                         for fn, args, _, _ in calls)
            for _, line, _ in dialogs:
                sites[owners[id(body)]] = {"line": line, "custom": custom}
            for _, line in windows:
                sites[owners[id(body)]] = {"line": line, "custom": custom}
        window_widths, window_heights = set(), set()
        subclass = next((m for m in subclasses if m[3] == "ADW_TYPE_DIALOG" and names.get(m[2] + "_init") is body), None)
        if subclass:
            dialogs.append(("self", declarations.count("\n", 0, subclass.start()) + 1, False))
        if not dialogs and not windows: continue
        edges, sized, widths, propagates, follows, scrollers = {}, set(), set(), set(), set(), []
        if subclass and subclass[1] in templates:
            widths.add("self")
            props = templates.get(subclass[1], {}) if isinstance(templates, dict) else {}
            if positive(props.get("content-height")): sized.add("self")
        for fn, args, target, line in calls:
            first = ref(args[0]) if args else ""
            if fn == "g_object_new":
                props = {"".join(args[i]).strip('"'): args[i + 1] for i in range(1, len(args) - 1, 2)}
                for prop, collection in (("content-width", widths), ("content-height", sized),
                                         ("default-width", window_widths), ("default-height", window_heights),
                                         ("height-request", sized)):
                    if positive("".join(props.get(prop, []))): collection.add(target)
                if props.get("propagate-natural-height") == ["TRUE"]: propagates.add(target)
                if props.get("follows-content-size") == ["TRUE"]: follows.add(target)
                for prop in ("child", "content", "extra-child"):
                    if prop in props: edges.setdefault(target, set()).add(ref(props[prop]))
            if fn == "gtk_window_set_default_size" and len(args) == 3:
                if positive("".join(args[1])): window_widths.add(first)
                if positive("".join(args[2])): window_heights.add(first)
            if fn == "adw_dialog_set_content_width" and len(args) > 1 and positive("".join(args[1])): widths.add(first)
            if fn == "adw_dialog_set_content_height" and len(args) > 1 and positive("".join(args[1])): sized.add(first)
            if fn == "gtk_widget_set_size_request" and len(args) > 2 and positive("".join(args[2])): sized.add(first)
            if fn == "adw_dialog_set_follows_content_size" and len(args) > 1 and args[1] == ["TRUE"]: follows.add(first)
            if fn == "gtk_scrolled_window_set_propagate_natural_height" and len(args) > 1 and args[1] == ["TRUE"]: propagates.add(first)
            if constructs(fn, args, "gtk_scrolled_window_new", "GTK_TYPE_SCROLLED_WINDOW"):
                scrollers.append((target, line, "gtk_scrolled_window_new"))
            if constructs(fn, args, "adw_preferences_page_new", "ADW_TYPE_PREFERENCES_PAGE"):
                scrollers.append((target, line, "adw_preferences_page_new"))
            if fn.endswith(("_set_child", "_set_extra_child", "_set_content", "_append", "_prepend", "_add", "_add_top_bar", "_add_row", "_add_suffix")) and len(args) > 1:
                edges.setdefault(first, set()).add(ref(args[1]))
                if subclass and first.startswith("self->"): edges.setdefault("self", set()).add(first)
        for window, line in windows:
            for dim, collection in (("default-width", window_widths), ("default-height", window_heights)):
                if window not in collection: errors.append((line, f"{window}: window lacks positive {dim}"))
        for dialog, line, automatic_width in dialogs:
            if not automatic_width and dialog not in widths: errors.append((line, f"{dialog}: AdwDialog lacks positive content-width"))
            descendants = {dialog}
            pending = [dialog]
            while pending:
                for child in edges.get(pending.pop(), ()):
                    if child not in descendants:
                        descendants.add(child); pending.append(child)
            for scroll, at, fn in scrollers:
                if scroll not in descendants: continue
                # Explicit height on the dialog or scroller is an intentional
                # viewport. A size on an unrelated sibling cannot waive it.
                if dialog not in sized and scroll not in sized and scroll not in propagates:
                    if fn == "adw_preferences_page_new" and dialog in follows: continue
                    errors.append((at, f"{scroll}: dialog {fn} lacks natural-height propagation/content-following or explicit height"))
    # Template-backed subclasses inherit checked sizing from the markup. Other
    # subclasses must size 'self' in their initializer (fail closed on new ones).
    for m in subclasses:
        name, prefix, parent = m.groups()
        if sites is not None:
            sites[name] = {"line": declarations.count("\n", 0, m.start()) + 1, "custom": True}
        if name in templates: continue
        initializer = names.get(prefix + "_init", [])
        setter = "adw_dialog_set_content_width" if parent == "ADW_TYPE_DIALOG" else "gtk_window_set_default_size"
        valid = any(fn == setter and len(args) == (2 if parent == "ADW_TYPE_DIALOG" else 3)
                    and ref(args[0]) == "self" and all(positive("".join(a)) for a in args[1:])
                    for fn, args, _, _ in all_calls.get(id(initializer), []))
        if not valid:
            errors.append((declarations.count("\n", 0, m.start()) + 1, f"{name}: lacks positive {setter} or template sizing"))
    return errors


class ParserTests(unittest.TestCase):
    def test_markup_scopes(self):
        text = 'Adw.Dialog { content-width: 400; Box { ScrolledWindow { Label { label: "propagate-natural-height: true"; } } ScrolledWindow { propagate-natural-height: true; } } }'
        self.assertEqual(len(markup_errors(blueprint(text))), 1)
        self.assertEqual(len(markup_errors(blueprint('Adw.Dialog { content-width: 400; content-height: 500; ScrolledWindow {} }'))), 0)
        self.assertEqual(len(markup_errors(blueprint('Adw.Window { default-width: 400; }'))), 1)
        self.assertEqual(len(markup_errors(xml_ui('<interface><object class="AdwDialog"><property name="content-width">400</property><child><object class="GtkScrolledWindow"/></child></object></interface>'))), 1)
    def test_alert_custom_subtrees(self):
        blp = 'Adw.AlertDialog { extra-child: ScrolledWindow {} }'
        ui = '<interface><object class="AdwAlertDialog"><child><object class="GtkScrolledWindow"/></child></object></interface>'
        self.assertEqual(len(markup_errors(blueprint(blp))), 1)
        self.assertEqual(len(markup_errors(xml_ui(ui))), 1)
        self.assertEqual(markup_errors(blueprint(blp.replace('ScrolledWindow {}', 'ScrolledWindow { propagate-natural-height: true; }'))), [])
        c = 'void f() { AdwDialog *d = adw_alert_dialog_new("Title", "Body"); GtkWidget *s = gtk_scrolled_window_new(); adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(d), s); }'
        self.assertEqual(len(c_errors(c)), 1)
        self.assertEqual(c_errors(c.replace('adw_alert_dialog_set_extra_child', 'gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(s), TRUE); adw_alert_dialog_set_extra_child')), [])
        sites = {}; c_errors(c, sites=sites)
        self.assertTrue(sites['f']['custom'])
        generic = 'void f() { GtkWidget *s = gtk_scrolled_window_new(); AdwDialog *d = g_object_new(ADW_TYPE_ALERT_DIALOG, "extra-child", s, NULL); }'
        sites = {}; self.assertEqual(len(c_errors(generic, sites=sites)), 1)
        self.assertTrue(sites['f']['custom'])
    def test_executed_inventory_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); directory = root / 'apps/grotto/tests'; directory.mkdir(parents=True)
            (directory / 'test-dialog-sizing.c').write_text('{"sample", open_sample},\n')
            path = directory / 'dialog-sizing-coverage.json'
            site = 'apps/grotto/src/probe.c:open_sample'
            sites = {site: {'line': 4, 'custom': True}}
            path.write_text(json.dumps({site: {'runtime': ['sample']}}))
            self.assertEqual(coverage_errors(root, sites), [])
            path.write_text(json.dumps({site: {'exception': 'toolkit'}}))
            self.assertTrue(any('custom children' in e[2] for e in coverage_errors(root, sites)))
            path.write_text(json.dumps({site: {'runtime': ['missing']}}))
            self.assertTrue(any('unknown runtime row' in e[2] for e in coverage_errors(root, sites)))
            path.write_text('{}')
            self.assertTrue(any('missing runtime coverage' in e[2] for e in coverage_errors(root, sites)))
            path.write_text(json.dumps({site: {'runtime': ['sample']}, 'stale.c:old': {'exception': 'old'}}))
            self.assertTrue(any('stale executed' in e[2] for e in coverage_errors(root, sites)))
    def test_gtk_window_families(self):
        for parent, factory in (("GTK_TYPE_WINDOW", "gtk_window_new"), ("GTK_TYPE_APPLICATION_WINDOW", "gtk_application_window_new")):
            bad_class = f'G_DEFINE_TYPE(Probe, probe, {parent}) void probe_init(Probe *self) {{}}'
            self.assertEqual(len(c_errors(bad_class)), 1)
            self.assertEqual(c_errors(bad_class.replace('{}', '{gtk_window_set_default_size(GTK_WINDOW(self), 400, 500);}')), [])
            bad_factory = f'void f() {{ GtkWindow *w = {factory}(); }}'
            self.assertEqual(len(c_errors(bad_factory)), 2)
            self.assertEqual(c_errors(bad_factory.replace('; }', '; gtk_window_set_default_size(w, 400, 500); }')), [])
            self.assertEqual(len(c_errors(f'void f() {{ GtkWindow *w = g_object_new({parent}, NULL); }}')), 2)
            self.assertEqual(c_errors(f'void f() {{ GtkWindow *w = g_object_new({parent}, "default-width", 400, "default-height", 500, NULL); }}'), [])
    def test_private_scroller_markup(self):
        for parser, text in ((blueprint, 'Adw.Dialog { content-width: 400; Adw.PreferencesPage {} }'),
                             (xml_ui, '<interface><object class="AdwDialog"><property name="content-width">400</property><child><object class="AdwPreferencesPage"/></child></object></interface>')):
            self.assertEqual(len(markup_errors(parser(text))), 1)
        self.assertEqual(markup_errors(blueprint('Adw.Dialog { content-width: 400; follows-content-size: true; Adw.PreferencesPage {} }')), [])
    def test_c_scopes(self):
        text = 'void f() { AdwDialog *d = adw_dialog_new(); adw_dialog_set_content_width(d, 400); GtkWidget *s = gtk_scrolled_window_new(); adw_dialog_set_child(d, s); /* gtk_scrolled_window_set_propagate_natural_height(s, TRUE); */ }'
        self.assertEqual(len(c_errors(text)), 1)
        self.assertEqual(len(c_errors(text.replace('/* ', '').replace(' */', ''))), 0)
        self.assertEqual(len(c_errors(text.replace('adw_dialog_set_content_width(d, 400);', 'adw_dialog_set_content_width(other, 400);'))), 2)
        self.assertEqual(len(c_errors(text.replace('gtk_scrolled_window_new()', 'adw_preferences_page_new()'))), 1)
        self.assertEqual(len(c_errors(text.replace('adw_dialog_set_child(d, s);', 'adw_dialog_set_child(d, s); adw_dialog_set_content_height(d, 500);'))), 0)
        self.assertEqual(len(c_errors(text.replace('AdwDialog *d =', 'p->dialog =').replace('(d,', '(p->dialog,'))), 1)
    def test_subclass_and_decoys(self):
        text = 'G_DEFINE_TYPE(TestWindow, test_window, ADW_TYPE_WINDOW) void test_window_init(TestWindow *self) { gtk_window_set_default_size(GTK_WINDOW(self), 400, 500); }'
        self.assertEqual(c_errors(text), [])
        self.assertEqual(len(c_errors(text.replace('400, 500', '400, -1'))), 1)
        self.assertEqual(len(c_errors(text.replace('gtk_window_set_default_size(GTK_WINDOW(self), 400, 500);', '') + 'void unrelated() { gtk_window_set_default_size(GTK_WINDOW(self), 400, 500); }')), 1)
        self.assertEqual(c_errors('/* ' + text + ' */'), [])
        self.assertEqual(len(c_errors('void f() { AdwWindow *w = adw_window_new(); }')), 2)
        self.assertEqual(c_errors('void f() { AdwWindow *w = adw_window_new(); gtk_window_set_default_size(GTK_WINDOW(w), 400, 500); }'), [])
        self.assertEqual(len(c_errors('void f() { AdwDialog *d = g_object_new(ADW_TYPE_DIALOG, NULL); }')), 1)
        self.assertEqual(c_errors('void f() { AdwDialog *d = g_object_new(ADW_TYPE_DIALOG, "content-width", 400, NULL); }'), [])
        self.assertEqual(c_errors('void f() { AdwWindow *w = g_object_new(ADW_TYPE_WINDOW, "default-width", 400, "default-height", 500, NULL); }'), [])
        self.assertEqual(c_errors(text.replace('gtk_window_set_default_size(GTK_WINDOW(self), 400, 500);', ''), {'TestWindow'}), [])


def coverage_errors(root, sites):
    """A source-to-execution inventory, not an assumption that file coverage is UI coverage.

    Scratch parser fixtures need no runtime registry. In the real component the
    CMake-required registry exists, so its inventory is mandatory. Stale entries,
    unregistered runtime rows and toolkit waivers over custom children all fail.
    """
    runtime = root / "apps/grotto/tests/test-dialog-sizing.c"
    if not runtime.exists(): return []
    path = runtime.with_name("dialog-sizing-coverage.json")
    if not path.exists(): return [(path, 1, "missing executed coverage inventory")]
    entries = json.loads(path.read_text())
    cases = set(re.findall(r'^\s*\{"([\w-]+)",', runtime.read_text(), re.M)) | {"@host"}
    errors = []
    for site, info in sites.items():
        entry = entries.get(site)
        source = root / site.rsplit(":", 1)[0]
        if not entry:
            errors.append((source, info["line"], f"{site}: missing runtime coverage or justified exception")); continue
        if bool(entry.get("runtime")) == bool(entry.get("exception")):
            errors.append((path, 1, f"{site}: supply runtime rows OR a justified exception"))
        for case in entry.get("runtime", []):
            if case not in cases: errors.append((path, 1, f"{site}: unknown runtime row {case}"))
        if entry.get("exception") and info.get("custom"):
            errors.append((source, info["line"], f"{site}: custom children cannot use a toolkit exception"))
    for site in entries.keys() - sites.keys():
        errors.append((path, 1, f"stale executed coverage site {site}"))
    print(f"Runtime inventory: {len(sites)} source sites, {len(cases) - 1} surface rows; "
          f"{sum(bool(e.get('exception')) for e in entries.values())} explicit exceptions")
    return errors


def main():
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ParserTests)
    if not unittest.TextTestRunner().run(suite).wasSuccessful(): return 1
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[3]
    components = [root / "apps/grotto", root / "gnome/grotto-panels"]
    templates, errors, count, sites = {}, [], 0, {}
    for component in components:
        for path in sorted((component / "data").rglob("*")):
            if path.suffix not in (".blp", ".ui"): continue
            count += 1
            tree = blueprint(path.read_text()) if path.suffix == ".blp" else xml_ui(path.read_text())
            def collect(node):
                if node.kind in ("AdwDialog", "AdwWindow", "AdwApplicationWindow", "GtkWindow", "GtkApplicationWindow"):
                    templates[node.name.lstrip("$")] = node.props
                if (node.kind in ("AdwDialog", "AdwAlertDialog", "AdwAboutDialog", "AdwPreferencesDialog") or
                        (node.kind.endswith("Window") and node.kind != "GtkScrolledWindow")):
                    site = f"{path.relative_to(root).with_suffix('')}:{node.name.lstrip('$') or node.kind}"
                    sites[site] = {"line": node.line, "custom": node.kind not in AUTO_SIZED or
                                   (node.kind != "GtkShortcutsWindow" and bool(node.children))}
                for child in node.children: collect(child)
            collect(tree)
            errors += [(path, line, message) for line, message in markup_errors(tree)]
    for component in components:
        for path in sorted(component.rglob("*.c")):
            if "tests" in path.relative_to(component).parts: continue
            count += 1
            local_sites = {}
            errors += [(path, line, message) for line, message in c_errors(path.read_text(), templates, local_sites)]
            sites.update({f"{path.relative_to(root)}:{owner}": info for owner, info in local_sites.items()})
    errors += coverage_errors(root, sites)
    for path, line, message in errors: print(f"{path.relative_to(root)}:{line}: {message}")
    print(f"Dialog sizing: {count} production markup/C files, {len(errors)} violations")
    return bool(errors)


if __name__ == "__main__":
    sys.exit(main())
