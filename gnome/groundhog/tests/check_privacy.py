#!/usr/bin/env python3
"""Static privacy guards for Groundhog (privacy charter item G01).

usage: check_privacy.py [--self-test] [GROUNDHOG_DIR]

Checks the Groundhog tree (default: this script's parent directory) against
the rules that docs/designs/groundhog-privacy-ux-charter-2026-09-28.md makes
static (§8.2 G01, §7.2, §7.11, PT-1, PT-4c, PT-9, PT-11):

  url-literal           No ws://, wss://, http:// or https:// URL literal in
                        src/** or data/ui/**. A URL is a scheme followed by
                        anything other than whitespace, a quote, a
                        backslash or an angle bracket, so "wss://%s" counts. Comments are ignored,
                        and a bare scheme ("wss://" in a prefix check or an
                        error message) names no host and is allowed. (P1, PT-4c)
  gsettings-allowlist   data/org.nostr.Groundhog.gschema.xml holds exactly one
                        schema whose keys are exactly GSETTINGS below, each
                        pinned to its type, default and choices. (PT-9, PT-11)
  blueprint-denylist    data/ui/** (.blp and committed .ui) uses no class newer
                        than the libadwaita 1.5 / GTK 4.14 floor. The C build
                        already enforces the floor with *_VERSION_MAX_ALLOWED;
                        this gives the same early failure for UI files on
                        hosts with a newer libadwaita. (§7.2)
  libsoup-boundary      libsoup is included or used only under src/net/ and
                        src/media/.
  no-gdk-pixbuf         src/** neither includes nor uses gdk-pixbuf; images
                        decode through GdkTexture's built-in loaders. (§2.1)
  no-tmp-cache          src/** never creates temporary files and never locates
                        the tmp, cache or runtime directories. (§6, P3)
  nip17-publish-relays  NIP-17 publication sources never reference
                        gh_account_relays_get_read_relays or
                        gh_account_relays_get_write_relays. (PD-4)
  nip17-no-10002        The same sources never name kind 10002 or NIP-65.
                        (PD-4)
  lookup-sources        Recipient lookup sources (gh-inbox-lookup*, later
                        gh-contact-directory*) never reference the account's
                        own relays: gh_account_relays_get_read_relays,
                        _write_relays or _inbox_relays. (§4.3, PD-12)
  account-auth-purpose  NIP-42 AUTH as the account (GH_RELAY_AUTH_ACCOUNT,
                        gh_relay_{scope,publish}_set_account_signer,
                        GhAccountAuth) appears only in ACCOUNT_AUTH_FILES,
                        each listed with its §4.3 purpose. (§4.4 R1)
  message-status        GhMessageStatus has no DELIVERED, READ or SEEN value.
                        (PD-1, PT-1)
  log-ids               No log call in src/** (g_debug, g_info, g_message,
                        g_warning, g_critical, g_error, g_print, g_printerr,
                        g_log) is passed a wrap, rumor or event id (an
                        identifier ending in wrap_id, rumor_id or event_id):
                        a rumor id commits to the plaintext and a wrap id
                        names a relay event. (PD-10; W13 review, item 5)
  app-id                GROUNDHOG_APP_ID in src/, the schema id and path, and
                        GhWindow's icon-name in gh-window.blp and gh-window.ui
                        all agree. (W11 review, non-blocking item 4)
  exceptions            Every EXCEPTIONS entry is justified and still matches.

NIP-17 publication sources are the files that build, hold or publish DM wraps
and pick their targets: gh-dm-send*, gh-nip17-*, gh-inbox-resolver* (the
recipient-target seam) and gh-outbox* (G06). Charter PD-4 says a wrap goes only
to the recipient's kind-10050 relays and the self-copy only to the account's
own 10050 relays, with no kind-10002 fallback. The recipient inbox lookup
(gh-inbox-lookup*, later G10's gh-contact-directory*) is deliberately exempt
from those two rules: it is a contact-directory connection (§4.3) that may
later ask the contact's own kind-10002 write relays for their kind 10050, and
only the 10050 it finds can become a target. Its sources are discovery-relays
and those contact relays, never the account's own relays, which would learn
whom the account is about to message (lookup-sources).

Exceptions: a finding from any rule except gsettings-allowlist, app-id and
message-status (which have their own pinned specifications) can be allowed by
adding (rule, path, match) -> justification to EXCEPTIONS, where `match` is the
exact text the rule reports. An entry that no longer matches, or has no
justification, fails the check.

--self-test first checks GROUNDHOG_DIR, then writes a synthetic clean tree
(including near misses no rule may flag) and one synthetic tree per mutation
fixture. The clean tree must pass, every fixture must fail with exactly its
expected rules, and every rule must have at least one failing fixture.
"""

import argparse
from collections import namedtuple
from pathlib import Path
import re
import sys
import tempfile
import xml.etree.ElementTree as ET

APP_ID = "org.nostr.Groundhog"
SCHEMA_PATH = "/org/nostr/Groundhog/"
SCHEMA_FILE = f"data/{APP_ID}.gschema.xml"

RULES = (
    "url-literal", "gsettings-allowlist", "blueprint-denylist", "libsoup-boundary",
    "no-gdk-pixbuf", "no-tmp-cache", "nip17-publish-relays", "nip17-no-10002",
    "lookup-sources", "account-auth-purpose", "message-status", "log-ids", "app-id",
    "exceptions",
)
# Rules whose findings EXCEPTIONS can never waive.
UNWAIVABLE = {"gsettings-allowlist", "app-id", "message-status", "exceptions"}

# (rule, path relative to GROUNDHOG_DIR, exact reported match) -> justification.
# Empty: nothing in Groundhog needs an exception today.
EXCEPTIONS = {}

Key = namedtuple("Key", "type default choices", defaults=(None,))

# The complete GSettings surface, pinned to type, default text and choices.
# GSettings is plaintext dconf that any same-user process can read, so it
# holds only app-global preferences, the selected identity and window state;
# per-conversation or per-contact state lives in the encrypted store (PD-11).
# The preference rows mirror charter §7.11; a new key needs a charter change
# and an entry here (§8.4).
GSETTINGS = {
    "current-npub": Key("s", "''"),
    # §7.11 Privacy / Notifications (PD-9). notifications-enabled becomes
    # true only when onboarding chooses background delivery.
    "notifications-enabled": Key("b", "false"),
    "notification-privacy": Key("s", "'hidden'"),
    "sound-enabled": Key("b", "false"),
    # §7.11 Privacy / Web Content (PD-2: nothing is fetched by default).
    "load-remote-images": Key("b", "false"),
    "link-previews": Key("b", "false"),
    "load-profile-pictures": Key("b", "false"),
    # §7.11 Privacy / Conversations (PD-8, D9).
    "filter-unknown-senders": Key("b", "true"),
    "show-message-previews": Key("b", "true"),
    # §7.11 Network / Connection (§4.2).
    "network-mode": Key("s", "'system'", ("system", "none", "tor")),
    "tor-socks-address": Key("s", "'127.0.0.1:9050'"),
    # §7.11 Network / Relays (PD-13, PT-9: no relay before the user confirms one).
    "discovery-relays": Key("as", "[]"),
    # §7.11 Account & Storage.
    "signer-method": Key("s", "'auto'"),
    "run-in-background": Key("b", "true"),  # D11: on, chosen in onboarding
    "retention-days": Key("i", "0"),  # D10: keep forever
    "default-disappearing-seconds": Key("i", "0"),
    "enter-sends": Key("b", "true"),
    "blossom-servers": Key("as", "[]"),  # D6: attachments off until chosen
    # Window state: no content, no identifiers.
    "window-width": Key("i", "900"),
    "window-height": Key("i", "600"),
    "window-maximized": Key("b", "false"),
}

# Classes introduced after the libadwaita 1.5 / GTK 4.14 floor, taken from the
# ADW_AVAILABLE_IN_1_x / GDK_AVAILABLE_IN_4_x annotations of libadwaita 1.9 and
# GTK 4.22 headers (AdwButtonRow is documented "Since 1.6" but its type is
# annotated ADW_AVAILABLE_IN_ALL). §7.2 lists the substitutes to use instead.
BLUEPRINT_DENYLIST = {
    **dict.fromkeys(("AdwBottomSheet", "AdwButtonRow", "AdwLayout", "AdwLayoutSlot",
                     "AdwMultiLayoutView", "AdwSpinner", "AdwSpinnerPaintable"),
                    "libadwaita 1.6"),
    **dict.fromkeys(("AdwInlineViewSwitcher", "AdwToggle", "AdwToggleGroup",
                     "AdwWrapBox", "AdwWrapLayout"), "libadwaita 1.7"),
    **dict.fromkeys(("AdwShortcutLabel", "AdwShortcutsDialog", "AdwShortcutsItem",
                     "AdwShortcutsSection"), "libadwaita 1.8"),
    **dict.fromkeys(("AdwSidebar", "AdwSidebarItem", "AdwSidebarSection",
                     "AdwViewSwitcherSidebar", "AdwNoneAnimationTarget"), "libadwaita 1.9"),
    **dict.fromkeys(("GtkAccessibleHyperlink", "GtkPopoverBin", "GtkSvg"), "GTK 4.22"),
}

LIBSOUP_DIRS = ("src/net/", "src/media/")
NIP17_PUBLICATION = re.compile(r"^(?:gh-dm-send|gh-nip17-|gh-inbox-resolver|gh-outbox)")
LOOKUP_SOURCES = re.compile(r"^(?:gh-inbox-lookup|gh-contact-directory)")
# The only files that may authenticate as the account (charter §4.4 R1), by
# path prefix, each with its §4.3 purpose. A new caller needs a purpose the
# charter allows (own inbox, own list publish, own self-copy publish, NIP-29
# group relays) and an entry here.
ACCOUNT_AUTH_FILES = {
    "src/relay/": "the NIP-42 mechanism; callers choose the identity per URL",
    "src/app/gh-account-auth.": "GhAccountAuth, the account's generation-bound AUTH signer",
    "src/app/gh-dm-inbox.": "own inbox read, on the account's own 10050 relays only",
}
FORBIDDEN_STATUS_WORDS = {"DELIVERED", "READ", "SEEN"}

URL_RE = re.compile(r"\b(?:wss?|https?)://[^\s\"'\\<>]+", re.I)
INCLUDE_RE = re.compile(r"^[ \t]*#[ \t]*include[ \t]*[<\"]([^>\"\n]+)[>\"]", re.M)
SOUP_API_RE = re.compile(r"\b(?:soup_[a-z]\w*|Soup[A-Z]\w*|SOUP_[A-Z]\w*)\b")
PIXBUF_API_RE = re.compile(
    r"\b(?:gdk_pixbuf_\w+|GdkPixbuf\w*|GDK_(?:TYPE_|IS_)?PIXBUF\w*|\w+_(?:from|for)_pixbuf)\b")
TMP_API_RE = re.compile(
    r"\b(?:g_file_open_tmp|g_get_tmp_dir|g_get_user_cache_dir|g_get_user_runtime_dir"
    r"|g_file_new_tmp\w*|g_dir_make_tmp|g_mkstemp\w*|g_mkdtemp\w*"
    r"|mkstemps?|mkostemps?|mkdtemp|tmpfile|tmpnam|tempnam)\b")
TMP_ENV_RE = re.compile(r"\"(?:TMPDIR|TMP|TEMP|TEMPDIR|XDG_CACHE_HOME|XDG_RUNTIME_DIR)\"")
ACCOUNT_RELAY_GETTER_RE = re.compile(r"\bgh_account_relays_get_(?:read|write)_relays\b")
OWN_RELAY_GETTER_RE = re.compile(r"\bgh_account_relays_get_(?:read|write|inbox)_relays\b")
ACCOUNT_AUTH_RE = re.compile(
    r"\bGH_RELAY_AUTH_ACCOUNT\b|\bgh_relay_(?:scope|publish)_set_account_signer\b"
    r"|\bgh_account_auth_\w+|\bGhAccountAuth\w*")
KIND_10002_RE = re.compile(r"\b10002\b|\b\w*KIND_RELAY_LIST\w*|(?i:\b\w*nip_?65\w*)")
LOG_CALL_RE = re.compile(
    r"\bg_(?:debug|info|message|warning|critical|error|print|printerr|log)\s*\(")
LOG_ID_RE = re.compile(r"\b\w*(?:wrap_id|rumor_id|event_id)\b")
APP_ID_DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+GROUNDHOG_APP_ID[ \t]+\"([^\"]*)\"", re.M)
XML_COMMENT_RE = re.compile(r"<!--.*?-->", re.S)
LEX_SPECIAL_RE = re.compile(r"//|/\*|[\"']")

Violation = namedtuple("Violation", "rule path line detail match")


def blank(text):
    """Replace everything but newlines with spaces, keeping offsets and lines."""
    return re.sub(r"[^\n]", " ", text)


def lex(text):
    """Return (no_comments, code_only) views of C or Blueprint source.

    Both views have the same length and line breaks as `text`. Comments are
    blanked in both; string and character literal contents are also blanked
    in code_only (the quotes stay).
    """
    keep, code = [], []
    i, n = 0, len(text)
    while i < n:
        m = LEX_SPECIAL_RE.search(text, i)
        if not m:
            keep.append(text[i:])
            code.append(text[i:])
            break
        keep.append(text[i:m.start()])
        code.append(text[i:m.start()])
        i = m.start()
        token = m.group()
        if token in ("//", "/*"):
            if token == "//":
                j = text.find("\n", i)
                j = n if j < 0 else j
            else:
                j = text.find("*/", i + 2)
                j = n if j < 0 else j + 2
            keep.append(blank(text[i:j]))
            code.append(blank(text[i:j]))
        else:
            j = i + 1
            while j < n and text[j] != token and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            literal = text[i:j]
            keep.append(literal)
            code.append(literal[0] + blank(literal[1:-1]) + literal[-1] if len(literal) > 1
                        else literal)
        i = j
    return "".join(keep), "".join(code)


def line_of(text, offset):
    return text.count("\n", 0, max(offset, 0)) + 1


class Tree:
    """A Groundhog source tree with cached comment-free views of its files."""

    def __init__(self, root):
        self.root = Path(root)
        self._views = {}

    def files(self, *prefixes, suffixes=None):
        for prefix in prefixes:
            base = self.root / prefix
            if not base.is_dir():
                continue
            for path in sorted(base.rglob("*")):
                if path.is_file() and (suffixes is None or path.suffix in suffixes):
                    yield path.relative_to(self.root).as_posix()

    def exists(self, rel):
        return (self.root / rel).is_file()

    def views(self, rel):
        """Return (text, no_comments, code_only) for a file."""
        if rel not in self._views:
            text = (self.root / rel).read_bytes().decode("utf-8", errors="replace")
            suffix = Path(rel).suffix
            if suffix in (".c", ".h", ".blp"):
                keep, code = lex(text)
            elif suffix in (".ui", ".xml"):
                keep = code = XML_COMMENT_RE.sub(lambda m: blank(m.group()), text)
            else:
                keep = code = text
            self._views[rel] = (text, keep, code)
        return self._views[rel]


def find_all(rule, rel, view, pattern, detail):
    return [Violation(rule, rel, line_of(view, m.start()), detail(m.group()), m.group())
            for m in pattern.finditer(view)]


def includes(view):
    return [(m.group(1), m.start()) for m in INCLUDE_RE.finditer(view)]


def check_url_literals(tree):
    found = []
    for rel in tree.files("src", "data/ui"):
        _, keep, _ = tree.views(rel)
        found += find_all("url-literal", rel, keep, URL_RE,
                          lambda url: f"URL literal {url!r}: every destination must come from a "
                                      "signed relay list, a user setting or a user action (P1)")
    return found


def check_gsettings(tree):
    rule = "gsettings-allowlist"
    if not tree.exists(SCHEMA_FILE):
        return [Violation(rule, SCHEMA_FILE, 0, "schema file is missing", "")]
    text, _, _ = tree.views(SCHEMA_FILE)
    try:
        schemalist = ET.fromstring(text)
    except ET.ParseError as error:
        return [Violation(rule, SCHEMA_FILE, error.position[0], f"invalid XML: {error}", "")]

    def at(name):
        return line_of(text, text.find(f'name="{name}"'))

    found = []
    schemas = schemalist.findall("schema")
    for extra in schemas[1:]:
        found.append(Violation(rule, SCHEMA_FILE, line_of(text, text.find(extra.get("id", "<schema"))),
                               f"unexpected extra schema {extra.get('id')!r}: Groundhog has one "
                               "app-global schema", extra.get("id", "")))
    for node in list(schemalist):
        if node.tag not in ("schema", "enum", "flags"):
            found.append(Violation(rule, SCHEMA_FILE, 0, f"unexpected <{node.tag}>", node.tag))
    if not schemas:
        return found + [Violation(rule, SCHEMA_FILE, 0, "no <schema> element", "")]
    schema = schemas[0]
    for node in schema:
        if node.tag != "key":
            found.append(Violation(rule, SCHEMA_FILE, 0,
                                   f"unexpected <{node.tag}> in the schema (child schemas and "
                                   "overrides are not allowed)", node.tag))
    keys = {}
    for key in schema.findall("key"):
        name = key.get("name", "")
        if name in keys:
            found.append(Violation(rule, SCHEMA_FILE, at(name), f"duplicate key {name!r}", name))
        keys[name] = key
    for name in sorted(set(keys) - set(GSETTINGS)):
        found.append(Violation(rule, SCHEMA_FILE, at(name),
                               f"key {name!r} is not in the allowlist: GSettings holds only "
                               "app-global preferences; per-conversation or per-contact state "
                               "belongs in the encrypted store (PD-11). A new preference needs "
                               "a charter §7.11 row and a GSETTINGS entry", name))
    for name in sorted(set(GSETTINGS) - set(keys)):
        found.append(Violation(rule, SCHEMA_FILE, 0, f"allowlisted key {name!r} is missing", name))
    for name in sorted(set(GSETTINGS) & set(keys)):
        spec, key = GSETTINGS[name], keys[name]
        default = " ".join((key.findtext("default") or "").split())
        choices = key.find("choices")
        have_choices = (tuple(c.get("value") for c in choices.findall("choice"))
                        if choices is not None else None)
        problems = []
        if key.get("type") != spec.type:
            problems.append(f"type {key.get('type')!r} (want {spec.type!r})")
        if default != spec.default:
            problems.append(f"default {default!r} (want {spec.default!r})")
        if have_choices != spec.choices:
            problems.append(f"choices {have_choices} (want {spec.choices})")
        for attribute in ("enum", "flags"):
            if key.get(attribute) is not None:
                problems.append(f"{attribute}= is not allowed")
        if key.find("range") is not None:
            problems.append("<range> is not pinned")
        if problems:
            found.append(Violation(rule, SCHEMA_FILE, at(name),
                                   f"key {name!r} has " + ", ".join(problems), name))
    return found


def check_blueprint(tree):
    found = []
    for rel in tree.files("data/ui", suffixes={".blp", ".ui"}):
        _, _, code = tree.views(rel)
        if rel.endswith(".blp"):
            pattern = re.compile(r"\b(Adw|Gtk|Gdk)\.([A-Z]\w*)")
            refs = [(m.group(1) + m.group(2), m.group(), m.start()) for m in pattern.finditer(code)]
        else:
            pattern = re.compile(r"\b(?:Adw|Gtk|Gdk)[A-Z]\w*")
            refs = [(m.group(), m.group(), m.start()) for m in pattern.finditer(code)]
        for cls, text, offset in refs:
            if cls in BLUEPRINT_DENYLIST:
                found.append(Violation("blueprint-denylist", rel, line_of(code, offset),
                                       f"{text} needs {BLUEPRINT_DENYLIST[cls]}, above the "
                                       "libadwaita 1.5 / GTK 4.14 floor (charter §7.2 lists the "
                                       "substitute)", text))
    return found


def check_sources(tree):
    """The C-source rules: libsoup, gdk-pixbuf, tmp/cache and the NIP-17 publication rules."""
    found = []
    for rel in tree.files("src", suffixes={".c", ".h"}):
        _, keep, code = tree.views(rel)
        soup_allowed = rel.startswith(LIBSOUP_DIRS)
        for path, offset in includes(keep):
            if re.match(r"libsoup(?:-[\d.]+)?/", path) and not soup_allowed:
                found.append(Violation("libsoup-boundary", rel, line_of(keep, offset),
                                       f"includes <{path}>: libsoup belongs in src/net/ or "
                                       "src/media/ only", path))
            if path.startswith("gdk-pixbuf/"):
                found.append(Violation("no-gdk-pixbuf", rel, line_of(keep, offset),
                                       f"includes <{path}>: decode PNG/JPEG with "
                                       "gdk_texture_new_from_bytes, not gdk-pixbuf", path))
        if not soup_allowed:
            found += find_all("libsoup-boundary", rel, code, SOUP_API_RE,
                              lambda s: f"uses libsoup ({s}) outside src/net/ and src/media/")
        found += find_all("no-gdk-pixbuf", rel, code, PIXBUF_API_RE,
                          lambda s: f"uses gdk-pixbuf ({s}); decode with gdk_texture_new_from_bytes")
        found += find_all("no-tmp-cache", rel, keep, TMP_API_RE,
                          lambda s: f"{s}: plaintext never goes to tmp, cache or runtime "
                                    "directories; use the encrypted store (§6, P3)")
        found += find_all("no-tmp-cache", rel, keep, TMP_ENV_RE,
                          lambda s: f"reads {s}: plaintext never goes to tmp, cache or runtime "
                                    "directories (§6, P3)")
        if NIP17_PUBLICATION.match(Path(rel).name):
            found += find_all("nip17-publish-relays", rel, keep, ACCOUNT_RELAY_GETTER_RE,
                              lambda s: f"{s} in a NIP-17 publication source: wraps go only to "
                                        "the recipient's 10050 and the self-copy only to "
                                        "gh_account_relays_get_inbox_relays (PD-4)")
            found += find_all("nip17-no-10002", rel, keep, KIND_10002_RE,
                              lambda s: f"{s!r} in a NIP-17 publication source: kind 10002 is "
                                        "never a DM target and there is no fallback (PD-4)")
        if LOOKUP_SOURCES.match(Path(rel).name):
            found += find_all("lookup-sources", rel, keep, OWN_RELAY_GETTER_RE,
                              lambda s: f"{s} in a recipient lookup: its sources are "
                                        "discovery-relays and the contact's own 10002 write "
                                        "relays, never the account's own relays (§4.3, PD-12)")
        if not rel.startswith(tuple(ACCOUNT_AUTH_FILES)):
            found += find_all("account-auth-purpose", rel, code, ACCOUNT_AUTH_RE,
                              lambda s: f"{s}: AUTH as the account only for a purpose §4.4 R1 "
                                        "allows; add the file to ACCOUNT_AUTH_FILES with its "
                                        "§4.3 purpose")
        found += check_log_ids(rel, code)
    return found


def check_log_ids(rel, code):
    """log-ids: the arguments of every log call (balanced parentheses)."""
    found = []
    for call in LOG_CALL_RE.finditer(code):
        depth, end = 1, call.end()
        while end < len(code) and depth:
            depth += {"(": 1, ")": -1}.get(code[end], 0)
            end += 1
        for m in LOG_ID_RE.finditer(code, call.end(), end):
            found.append(Violation("log-ids", rel, line_of(code, m.start()),
                                   f"{m.group(0)} in a log call: logs never name a wrap, rumor "
                                   "or event (PD-10)", m.group(0)))
    return found


def check_message_status(tree):
    found = []
    enum_re = re.compile(r"typedef\s+enum\s*\w*\s*\{([^{}]*)\}\s*GhMessageStatus\s*;"
                         r"|\benum\s+_?GhMessageStatus\s*\{([^{}]*)\}")
    for rel in tree.files("src", suffixes={".c", ".h"}):
        _, _, code = tree.views(rel)
        for m in enum_re.finditer(code):
            group = 1 if m.group(1) is not None else 2
            body, start = m.group(group), m.start(group)
            for item in re.finditer(r"\b([A-Za-z_]\w*)\s*(?:=[^,]*)?(?:,|$)", body):
                name = item.group(1)
                words = set(name.upper().split("_")) & FORBIDDEN_STATUS_WORDS
                if words:
                    found.append(Violation("message-status", rel,
                                           line_of(code, start + item.start()),
                                           f"GhMessageStatus value {name} claims "
                                           f"{'/'.join(sorted(words))}: status never claims more "
                                           "than a relay OK (PD-1)", name))
    return found


def blueprint_template_icon_name(keep, code, template="GhWindow"):
    """Return (value, offset) of the icon-name set directly on the template."""
    m = re.search(r"\btemplate\s+\$" + template + r"\s*:\s*[\w.]+\s*\{", code)
    if not m:
        return None, 0
    prop = re.compile(r"icon-name\s*:\s*(?:_\(\s*)?([\"'])(.*?)\1")
    depth, i = 1, m.end()
    while i < len(code) and depth:
        ch = code[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        elif depth == 1 and code.startswith("icon-name", i) and not re.match(r"[\w-]", code[i - 1]):
            pm = prop.match(keep, i)
            if pm:
                return pm.group(2), i
        i += 1
    return None, m.start()


def check_app_id(tree):
    rule = "app-id"
    found = []
    defines = []
    for rel in tree.files("src", suffixes={".c", ".h"}):
        _, keep, _ = tree.views(rel)
        defines += [(rel, m.group(1), line_of(keep, m.start()))
                    for m in APP_ID_DEFINE_RE.finditer(keep)]
    if not defines:
        found.append(Violation(rule, "src", 0, "GROUNDHOG_APP_ID is not defined in src/", ""))
    for rel, value, line in defines:
        if value != APP_ID:
            found.append(Violation(rule, rel, line, f"GROUNDHOG_APP_ID is {value!r}, not {APP_ID!r}",
                                   value))

    if tree.exists(SCHEMA_FILE):
        text, _, _ = tree.views(SCHEMA_FILE)
        try:
            schema = ET.fromstring(text).find("schema")
        except ET.ParseError:
            schema = None  # gsettings-allowlist reports the parse error
        if schema is not None and (schema.get("id"), schema.get("path")) != (APP_ID, SCHEMA_PATH):
            found.append(Violation(rule, SCHEMA_FILE, line_of(text, text.find("<schema ")),
                                   f"schema id/path {schema.get('id')!r}/{schema.get('path')!r}, "
                                   f"not {APP_ID!r}/{SCHEMA_PATH!r}", str(schema.get("id"))))

    blp = "data/ui/gh-window.blp"
    if not tree.exists(blp):
        found.append(Violation(rule, blp, 0, "gh-window.blp is missing", ""))
    else:
        _, keep, code = tree.views(blp)
        value, offset = blueprint_template_icon_name(keep, code)
        if value != APP_ID:
            found.append(Violation(rule, blp, line_of(code, offset),
                                   f"GhWindow icon-name is {value!r}, not the app ID {APP_ID!r}",
                                   str(value)))
    ui = "data/ui/gh-window.ui"
    if not tree.exists(ui):
        found.append(Violation(rule, ui, 0, "gh-window.ui is missing", ""))
    else:
        text, _, _ = tree.views(ui)
        try:
            template = ET.fromstring(text).find("template[@class='GhWindow']")
        except ET.ParseError as error:
            return found + [Violation(rule, ui, error.position[0], f"invalid XML: {error}", "")]
        prop = template.find("property[@name='icon-name']") if template is not None else None
        value = prop.text if prop is not None else None
        if value != APP_ID:
            found.append(Violation(rule, ui, line_of(text, text.find('name="icon-name"')),
                                   f"GhWindow icon-name is {value!r}, not the app ID {APP_ID!r}",
                                   str(value)))
    return found


def check(root, exceptions=None):
    """Return the violations in the Groundhog tree at `root`."""
    exceptions = EXCEPTIONS if exceptions is None else exceptions
    tree = Tree(root)
    raw = (check_url_literals(tree) + check_gsettings(tree) + check_blueprint(tree)
           + check_sources(tree) + check_message_status(tree) + check_app_id(tree))
    used, found = set(), []
    for violation in raw:
        key = (violation.rule, violation.path, violation.match)
        if violation.rule not in UNWAIVABLE and (exceptions.get(key) or "").strip():
            used.add(key)
        else:
            found.append(violation)
    for key, justification in exceptions.items():
        rule, path, match = key
        if rule not in RULES or rule in UNWAIVABLE:
            found.append(Violation("exceptions", path, 0,
                                   f"exception for {rule!r} is not allowed", match))
        elif not (justification or "").strip():
            found.append(Violation("exceptions", path, 0,
                                   f"exception {key} has no justification", match))
        elif key not in used:
            found.append(Violation("exceptions", path, 0,
                                   f"stale exception {key}: nothing matches it any more", match))
    return found


def report(violations):
    for v in violations:
        print(f"[{v.rule}] {v.path}:{v.line}: {v.detail}")


# --- self-test fixtures -------------------------------------------------------

def render_schema(keys=GSETTINGS):
    lines = ['<?xml version="1.0" encoding="UTF-8"?>',
             "<!-- Synthetic schema; documented at https://docs.gtk.org/gio/class.Settings.html -->",
             "<schemalist>", f'  <schema id="{APP_ID}" path="{SCHEMA_PATH}">']
    for name, key in keys.items():
        lines.append(f'    <key name="{name}" type="{key.type}">')
        if key.choices:
            lines.append("      <choices>")
            lines += [f'        <choice value="{choice}"/>' for choice in key.choices]
            lines.append("      </choices>")
        lines += [f"      <default>{key.default}</default>", f"      <summary>{name}</summary>",
                  "    </key>"]
    lines += ["  </schema>", "</schemalist>", ""]
    return "\n".join(lines)


def clean_tree():
    """A minimal tree that passes every rule, with near misses each rule must ignore."""
    return {
        "src/main.c": (
            "#include <adwaita.h>\n"
            f'#define GROUNDHOG_APP_ID "{APP_ID}"\n'
            "int main(void) { return 0; }\n"),
        "src/app/gh-dm-send.h": (
            "/* Kind 10002 is never consulted: wraps go to the recipient's kind-10050\n"
            " * relays (https://github.com/nostr-protocol/nips/blob/master/17.md) and\n"
            " * never via gh_account_relays_get_write_relays(). */\n"
            "#pragma once\n"
            "#include \"gh-account-relays.h\"\n"),
        "src/app/gh-dm-send.c": (
            "#include \"gh-dm-send.h\"\n"
            "// Scheme-only strings name no host and are not URL literals.\n"
            "static const char *const reason = \"relay URL must be ws:// or wss:// with a host\";\n"
            "static int secure(const char *url) { return g_str_has_prefix(url, \"wss://\"); }\n"
            "static const char *const note = \"The recipient has no kind-10050 inbox relays\";\n"
            "static const gchar *const *own(GhAccountRelays *relays) {\n"
            "  return gh_account_relays_get_inbox_relays(relays);\n"
            "}\n"),
        "src/app/gh-nip17-envelope.c": (
            "/* Seal and wrap; gift wrap timestamps per NIP-59. No g_file_open_tmp. */\n"
            "#include \"gh-nip17-envelope.h\"\n"
            "static const int kinds[] = { 13, 1059, 10050 };\n"),
        "src/app/gh-inbox-resolver.c": "#include \"gh-inbox-resolver.h\"\n",
        # The lookup is a contact-directory connection: discovery-relays only.
        # Comments and strings naming what it must not use are near misses.
        "src/app/gh-inbox-lookup.c": (
            "#include \"gh-inbox-lookup.h\"\n"
            "/* Never gh_account_relays_get_read_relays(), and no GH_RELAY_AUTH_ACCOUNT. */\n"
            "static gchar **sources(GSettings *s) {\n"
            "  return g_settings_get_strv(s, \"discovery-relays\");\n"
            "}\n"
            "static const int mode = GH_RELAY_AUTH_NONE;\n"
            "static const char *const why = \"no GH_RELAY_AUTH_ACCOUNT on others' relays\";\n"),
        # Account AUTH where §4.3 allows it: the mechanism, its adapter, own inbox.
        "src/relay/gh-relay-auth.h": (
            "typedef enum { GH_RELAY_AUTH_NONE, GH_RELAY_AUTH_EPHEMERAL,\n"
            "               GH_RELAY_AUTH_ACCOUNT } GhRelayAuthMode;\n"),
        "src/app/gh-account-auth.c": (
            "#include \"gh-account-auth.h\"\n"
            "G_DEFINE_FINAL_TYPE(GhAccountAuth, gh_account_auth, G_TYPE_OBJECT)\n"),
        "src/app/gh-dm-inbox.c": (
            "#include \"gh-account-auth.h\"\n"
            "static int own(GhRelayScope *s, GhAccountAuth *a, const char *u) {\n"
            "  return gh_relay_scope_set_account_signer(s, gh_account_auth_get_signer(a), NULL) &&\n"
            "         gh_relay_scope_set_url_auth(s, u, GH_RELAY_AUTH_ACCOUNT, NULL);\n"
            "}\n"
            "/* Near misses: ids used outside log calls, and logs about a wrap. */\n"
            "static void deferred(GHashTable *ids, const char *wrap_id, GError *error) {\n"
            "  g_hash_table_add(ids, g_strdup(wrap_id));\n"
            "  g_debug(\"Groundhog deferred a NIP-17 wrap (event_id elided): %s\", error->message);\n"
            "  g_log_set_default_handler(NULL, NULL);\n"
            "}\n"),
        "src/app/gh-message-status.h": (
            "/* Honest status: there is deliberately no DELIVERED or READ value. */\n"
            "typedef enum {\n"
            "  GH_MESSAGE_STATUS_SENDING,\n"
            "  GH_MESSAGE_STATUS_SENT = 2,\n"
            "  GH_MESSAGE_STATUS_ALREADY_SENT,\n"
            "  GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX\n"
            "} GhMessageStatus;\n"),
        "src/app/gh-store-path.c": (
            "/* Not the cache dir (g_get_user_cache_dir): the data dir, and textures, not pixbufs. */\n"
            "static char *path(void) { return g_build_filename(g_get_user_data_dir(), \"groundhog\", NULL); }\n"
            "static const char *const home = \"XDG_DATA_HOME\";\n"
            "static GdkTexture *decode(GBytes *b, GError **e) { return gdk_texture_new_from_bytes(b, e); }\n"),
        "src/net/gh-net-session.c": (
            "#include <libsoup/soup.h>\n"
            "static SoupSession *session_new(void) { return soup_session_new(); }\n"),
        "src/media/gh-blossom-client.c": "#include <libsoup/soup.h>\n",
        SCHEMA_FILE: render_schema(),
        "data/ui/gh-window.blp": (
            "using Gtk 4.0;\n"
            "using Adw 1;\n\n"
            "// Reference: https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1.5/\n"
            "/* Adw.Spinner needs 1.6, so this uses Gtk.Spinner. */\n"
            "template $GhWindow: Adw.ApplicationWindow {\n"
            "  title: \"Groundhog\";\n"
            f"  icon-name: \"{APP_ID}\";\n"
            "  content: Adw.ToastOverlay {\n"
            "    child: Gtk.Box {\n"
            "      Gtk.Button { icon-name: \"open-menu-symbolic\"; }\n"
            "      Gtk.Spinner {}\n"
            "      Gtk.Label { label: _(\"Relays use wss:// addresses\"); }\n"
            "    };\n"
            "  };\n"
            "}\n"),
        "data/ui/gh-window.ui": (
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            "<!--\nDO NOT EDIT! Generated by https://gitlab.gnome.org/jwestman/blueprint-compiler\n-->\n"
            "<interface>\n"
            '  <requires lib="gtk" version="4.0"/>\n'
            '  <template class="GhWindow" parent="AdwApplicationWindow">\n'
            '    <property name="title">Groundhog</property>\n'
            f'    <property name="icon-name">{APP_ID}</property>\n'
            '    <property name="content">\n'
            '      <object class="AdwToastOverlay">\n'
            '        <property name="child"><object class="GtkSpinner"/></property>\n'
            "      </object>\n"
            "    </property>\n"
            "  </template>\n"
            "</interface>\n"),
    }


def append(rel, extra):
    def edit(tree):
        tree[rel] = tree.get(rel, "") + extra
    return edit


def replace(rel, old, new):
    def edit(tree):
        if old not in tree[rel]:
            raise AssertionError(f"fixture {rel} lacks {old!r}")
        tree[rel] = tree[rel].replace(old, new, 1)
    return edit


Mutation = namedtuple("Mutation", "name expect edits exceptions", defaults=(None,))
URL = "wss://relay.example.org"
M = Mutation
MUTATIONS = [
    M("url-c-literal", {"url-literal"},
      [append("src/app/gh-dm-send.c", f'static const char *fallback = "{URL}";\n')]),
    M("url-c-format", {"url-literal"},
      [append("src/app/gh-inbox-lookup.c", 'static char *u(const char *h) { return g_strdup_printf("wss://%s", h); }\n')]),
    M("url-header-define", {"url-literal"},
      [append("src/app/gh-link.h", '#define GH_HELP_URL "https://example.org/help"\n')]),
    M("url-blueprint", {"url-literal"},
      [replace("data/ui/gh-window.blp", 'title: "Groundhog";', 'title: "Groundhog";\n  tooltip-text: "https://example.org";')]),
    M("url-ui", {"url-literal"},
      [replace("data/ui/gh-window.ui", '<property name="title">',
               '<property name="tooltip-text">ws://198.51.100.7:7777</property>\n    <property name="title">')]),
    M("gsettings-default-flip", {"gsettings-allowlist"},
      [replace(SCHEMA_FILE, '<key name="link-previews" type="b">\n      <default>false',
               '<key name="link-previews" type="b">\n      <default>true')]),
    M("gsettings-discovery-default", {"gsettings-allowlist"},
      [replace(SCHEMA_FILE, '<key name="discovery-relays" type="as">\n      <default>[]',
               f"<key name=\"discovery-relays\" type=\"as\">\n      <default>['{URL}']")]),
    M("gsettings-type", {"gsettings-allowlist"},
      [replace(SCHEMA_FILE, '<key name="retention-days" type="i">', '<key name="retention-days" type="x">')]),
    M("gsettings-extra-key", {"gsettings-allowlist"},
      [replace(SCHEMA_FILE, "  </schema>",
               '    <key name="muted-conversations" type="as"><default>[]</default></key>\n  </schema>')]),
    M("gsettings-missing-key", {"gsettings-allowlist"},
      [replace(SCHEMA_FILE, '<key name="tor-socks-address"', '<key name="tor-socks-host"')]),
    M("gsettings-choices", {"gsettings-allowlist"},
      [replace(SCHEMA_FILE, '<choice value="tor"/>', '<choice value="tor"/>\n        <choice value="direct"/>')]),
    M("gsettings-child-schema", {"gsettings-allowlist"},
      [replace(SCHEMA_FILE, "</schemalist>",
               f'  <schema id="{APP_ID}.Conversation">\n    <key name="draft" type="s">'
               "<default>''</default></key>\n  </schema>\n</schemalist>")]),
    M("blueprint-adw-1-6", {"blueprint-denylist"},
      [replace("data/ui/gh-window.blp", "      Gtk.Spinner {}", "      Adw.Spinner {}")]),
    M("blueprint-adw-1-9", {"blueprint-denylist"},
      [replace("data/ui/gh-window.blp", "      Gtk.Spinner {}", "      Adw.Sidebar {}")]),
    M("blueprint-ui-class", {"blueprint-denylist"},
      [replace("data/ui/gh-window.ui", 'class="GtkSpinner"', 'class="AdwButtonRow"')]),
    M("libsoup-include", {"libsoup-boundary"},
      [append("src/app/gh-fetch.c", "#include <libsoup/soup.h>\n")]),
    M("libsoup-api", {"libsoup-boundary"},
      [append("src/relay/gh-relay-soup.c", "static SoupSession *s(void) { return soup_session_new(); }\n")]),
    M("gdk-pixbuf-include", {"no-gdk-pixbuf"},
      [append("src/ui/gh-avatar.c", "#include <gdk-pixbuf/gdk-pixbuf.h>\n")]),
    M("gdk-pixbuf-api", {"no-gdk-pixbuf"},
      [append("src/ui/gh-avatar.c", "static void *t(void *p) { return gdk_texture_new_for_pixbuf(p); }\n")]),
    M("tmp-open", {"no-tmp-cache"},
      [append("src/media/gh-attachment.c", "static int f(GError **e) { return g_file_open_tmp(NULL, NULL, e); }\n")]),
    M("tmp-dir", {"no-tmp-cache"},
      [append("src/app/gh-store-path.c", "static const char *t(void) { return g_get_tmp_dir(); }\n")]),
    M("tmp-cache-dir", {"no-tmp-cache"},
      [append("src/app/gh-store-path.c", "static const char *c(void) { return g_get_user_cache_dir(); }\n")]),
    M("tmp-env", {"no-tmp-cache"},
      [append("src/app/gh-store-path.c", 'static const char *e(void) { return g_getenv("XDG_CACHE_HOME"); }\n')]),
    M("nip17-send-write-relays", {"nip17-publish-relays"},
      [append("src/app/gh-dm-send.c",
              "static const gchar *const *w(GhAccountRelays *r) { return gh_account_relays_get_write_relays(r); }\n")]),
    M("nip17-envelope-read-relays", {"nip17-publish-relays"},
      [append("src/app/gh-nip17-envelope.c",
              "static const gchar *const *r(GhAccountRelays *a) { return gh_account_relays_get_read_relays(a); }\n")]),
    M("nip17-outbox-getter-pointer", {"nip17-publish-relays"},
      [append("src/app/gh-outbox.c", "static void *getter = (void *)gh_account_relays_get_read_relays;\n")]),
    M("nip17-kind-literal", {"nip17-no-10002"},
      [append("src/app/gh-dm-send.c", "static const int fallback_kinds[] = { 10002 };\n")]),
    M("nip17-kind-in-filter-string", {"nip17-no-10002"},
      [append("src/app/gh-dm-send.c", 'static const char *filter = "{\\"kinds\\":[10002]}";\n')]),
    M("nip17-nip65-include", {"nip17-no-10002"},
      [append("src/app/gh-dm-send.c", "#include <nostr/nip65/nip65.h>\n")]),
    M("nip17-resolver-kind-constant", {"nip17-no-10002"},
      [append("src/app/gh-inbox-resolver.c", "static const int k = NOSTR_KIND_RELAY_LIST_METADATA;\n")]),
    M("lookup-own-read-relays", {"lookup-sources"},
      [append("src/app/gh-inbox-lookup.c",
              "static const gchar *const *r(GhAccountRelays *a) { return gh_account_relays_get_read_relays(a); }\n")]),
    M("directory-own-inbox-relays", {"lookup-sources"},
      [append("src/app/gh-contact-directory.c",
              "static void *getter = (void *)gh_account_relays_get_inbox_relays;\n")]),
    M("account-auth-lookup", {"account-auth-purpose"},
      [append("src/app/gh-inbox-lookup.c", "static const int account = GH_RELAY_AUTH_ACCOUNT;\n")]),
    M("account-auth-send-signer", {"account-auth-purpose"},
      [append("src/app/gh-dm-send.c",
              "static void *s(GhAccountAuth *a) { return gh_account_auth_get_signer(a); }\n")]),
    M("account-auth-publish-signer", {"account-auth-purpose"},
      [append("src/app/gh-outbox.c", "static void *f = (void *)gh_relay_publish_set_account_signer;\n")]),
    M("status-delivered", {"message-status"},
      [replace("src/app/gh-message-status.h", "  GH_MESSAGE_STATUS_SENDING,\n",
               "  GH_MESSAGE_STATUS_SENDING,\n  GH_MESSAGE_STATUS_DELIVERED,\n")]),
    M("status-read", {"message-status"},
      [replace("src/app/gh-message-status.h", "NO_INBOX\n", "NO_INBOX,\n  GH_MESSAGE_STATUS_READ = 9\n")]),
    M("log-rumor-id", {"log-ids"},
      [append("src/app/gh-dm-inbox.c",
              'static void f(GhNip17Message *m) { g_message("cannot store %s", m->rumor_id); }\n')]),
    M("log-wrap-id-nested", {"log-ids"},
      [append("src/app/gh-outbox.c",
              'static void w(Job *j, GError *e) {\n'
              '  g_warning("wrap %s: %s", g_strdup(j->wrap_id), e ? e->message : "?");\n}\n')]),
    M("app-id-blueprint", {"app-id"},
      [replace("data/ui/gh-window.blp", f'icon-name: "{APP_ID}"', f'icon-name: "{APP_ID}.Devel"')]),
    M("app-id-ui", {"app-id"},
      [replace("data/ui/gh-window.ui", f">{APP_ID}</property>", ">org.example.Other</property>")]),
    M("app-id-define", {"app-id"},
      [replace("src/main.c", f'"{APP_ID}"', '"org.example.Groundhog"')]),
    M("app-id-schema", {"app-id"},
      [replace(SCHEMA_FILE, f'path="{SCHEMA_PATH}"', 'path="/org/example/Groundhog/"')]),
    M("exception-allows-justified", set(),
      [append("src/app/gh-dm-send.c", f'static const char *fallback = "{URL}";\n')],
      {("url-literal", "src/app/gh-dm-send.c", URL): "synthetic: documented exception"}),
    M("exception-stale", {"exceptions"}, [],
      {("url-literal", "src/app/gh-dm-send.c", URL): "synthetic: nothing matches"}),
    M("exception-unjustified", {"exceptions", "url-literal"},
      [append("src/app/gh-dm-send.c", f'static const char *fallback = "{URL}";\n')],
      {("url-literal", "src/app/gh-dm-send.c", URL): " "}),
    M("exception-unwaivable", {"exceptions", "gsettings-allowlist"},
      [replace(SCHEMA_FILE, '<key name="retention-days" type="i">', '<key name="retention-days" type="x">')],
      {("gsettings-allowlist", SCHEMA_FILE, "retention-days"): "synthetic: pinned rules cannot be waived"}),
]


def self_test():
    failures, covered = [], set()
    with tempfile.TemporaryDirectory(prefix="groundhog-privacy-") as tmp:
        def run(name, tree, exceptions):
            root = Path(tmp) / name
            for rel, content in tree.items():
                path = root / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")
            return check(root, exceptions)

        clean = run("clean", clean_tree(), {})
        if clean:
            report(clean)
            failures.append("the clean fixture must pass")
        for mutation in MUTATIONS:
            tree = clean_tree()
            for edit in mutation.edits:
                edit(tree)
            got = {v.rule for v in run(mutation.name, tree, mutation.exceptions or {})}
            covered |= mutation.expect
            if got != mutation.expect:
                failures.append(f"{mutation.name}: expected {sorted(mutation.expect) or 'pass'}, "
                                f"got {sorted(got) or 'pass'}")
    uncovered = set(RULES) - covered
    if uncovered:
        failures.append("rules without a failing mutation fixture: " + ", ".join(sorted(uncovered)))
    if failures:
        print("\n".join(failures))
        return 1
    print(f"self-test: the clean fixture passes and all {len(MUTATIONS)} mutation fixtures give "
          f"exactly their expected result, covering all {len(RULES)} rules")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--self-test", action="store_true",
                        help="also prove every rule fails on a synthetic violating tree")
    parser.add_argument("root", nargs="?", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="the gnome/groundhog directory")
    args = parser.parse_args()
    for required in ("src", "data/ui"):
        if not (args.root / required).is_dir():
            sys.exit(f"{args.root / required} is not a directory; pass the gnome/groundhog directory")
    violations = check(args.root)
    report(violations)
    if violations:
        print(f"{len(violations)} Groundhog privacy violation(s)")
        return 1
    print(f"Groundhog privacy static checks pass ({len(RULES)} rules)")
    return self_test() if args.self_test else 0


if __name__ == "__main__":
    sys.exit(main())
