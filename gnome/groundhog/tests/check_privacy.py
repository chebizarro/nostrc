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
  tls-resumption        Every source under src/net/ and src/media/ passes each
                        SoupMessage it makes (soup_message_new*) to
                        gh_net_tls_no_resumption(): at least as many calls as
                        messages made. No src/ file makes a GIO TLS client
                        connection itself (g_tls_client_connection_new,
                        g_socket_client_set_tls). glib-networking's TLS
                        session cache is process-wide and keyed by host name,
                        so one connection that stores a ticket links all later
                        ones to that host, Tor or not. (PD-6; W16 review B1)
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
  account-auth-purpose  The NIP-42 account-AUTH mechanism (GH_RELAY_AUTH_ACCOUNT,
                        gh_relay_{scope,publish}_set_account_signer,
                        GhAccountAuth) appears only in ACCOUNT_AUTH_FILES:
                        the relay layer, GhAccountAuth and GhAuthPolicy.
                        (§4.4 R1)
  auth-policy           GhAuthPolicy is the one NIP-42 identity decision (G08):
                        gh_relay_{scope,publish}_set_url_auth appears only in
                        URL_AUTH_FILES (src/relay/, gh-auth-policy*), and the
                        purposes that sign in as the account
                        (GH_AUTH_PURPOSE_OWN_INBOX_READ, _OWN_LIST_PUBLISH,
                        _SELF_WRAP, _GROUP) only in ACCOUNT_PURPOSE_FILES,
                        each listed with its §4.3 purpose. (§4.3, §4.4 R1)
  account-auth-setter   Inside those files too, the calls that set a URL's
                        NIP-42 identity or hand out the account signer
                        (gh_relay_{scope,publish}_set_url_auth,
                        gh_relay_{scope,publish}_set_account_signer,
                        gh_account_auth_new, gh_account_auth_get_signer)
                        appear only where each is defined (AUTH_SETTERS) and
                        in src/app/gh-auth-policy.c: no other file, not even
                        in the relay layer or GhAccountAuth, sets account AUTH
                        directly, so every account sign-in shares the
                        policy's one signer per generation. (§4.4 R1, R6)
  message-status        GhMessageStatus has no DELIVERED, READ or SEEN value.
                        (PD-1, PT-1)
  log-ids               No log call in src/** (g_debug, g_info, g_message,
                        g_warning, g_critical, g_error, g_print, g_printerr,
                        g_log) is passed a wrap, rumor or event id (an
                        identifier ending in wrap_id, rumor_id or event_id):
                        a rumor id commits to the plaintext and a wrap id
                        names a relay event. (PD-10; W13 review, item 5)
  relay-suggestions     data/relay-suggestions.json (the reviewed onboarding
                        suggestions, D4) is short (1..6 relays), every URL is
                        a normalized wss:// relay address, every entry says
                        honestly whether the relay keeps kind-1059 reads
                        private ("yes"/"no" with its review evidence, or
                        "unknown"), and no host is one AGENTS.md bans. The
                        banned hosts are read from AGENTS.md's "Banned Relays"
                        section (its "never" sentences), so no banned name is
                        ever written into this tree. Not waivable.
  app-id                GROUNDHOG_APP_ID in src/, the schema id and path, and
                        GhWindow's icon-name in gh-window.blp and gh-window.ui
                        all agree. (W11 review, non-blocking item 4)
  preference-consumers  Every preference key the Preferences dialog binds
                        (src/ui/gh-preferences-dialog.c) is read somewhere in
                        src/ outside the dialog, unless its row is gated: its
                        key_features entry names a feature that
                        src/app/gh-features.h, the one list of what the build
                        performs, defines as a literal 0. Every feature a
                        gate names must be in that list. Landing a consumer
                        and flipping its GH_FEATURE_* is then one change.
                        (charter §7.11: no fake support; W13b review B2)
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
import json
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
    "tls-resumption",
    "no-gdk-pixbuf", "no-tmp-cache", "nip17-publish-relays", "nip17-no-10002",
    "lookup-sources", "account-auth-purpose", "auth-policy", "account-auth-setter",
    "message-status", "log-ids",
    "relay-suggestions",
    "app-id", "preference-consumers", "exceptions",
)
# Rules whose findings EXCEPTIONS can never waive.
UNWAIVABLE = {"gsettings-allowlist", "app-id", "message-status", "relay-suggestions",
              "exceptions"}

# (rule, path relative to GROUNDHOG_DIR, exact reported match) -> justification.
EXCEPTIONS = {
    # The About dialog's project links: build-time constants shown as text in
    # AdwAboutDialog, opened in the browser only when the user activates one
    # (a user action, P1); Groundhog itself never fetches them.
    ("url-literal", "src/ui/gh-about-dialog.c", "https://github.com/chebizarro/nostrc"):
        "the About dialog's website link, opened only when the user clicks it",
    ("url-literal", "src/ui/gh-about-dialog.c", "https://github.com/chebizarro/nostrc/issues"):
        "the About dialog's issue-tracker link, opened only when the user clicks it",
    # nostrc-u7cb review L6: the runtime directory is only located to find the
    # GVfs FUSE mount ($XDG_RUNTIME_DIR/gvfs) and refuse to read files under
    # it (gvfsd would fetch them outside GhNetHttp and Tor); nothing is ever
    # written there or read from it.
    ("no-tmp-cache", "src/media/gh-attachment.c", "g_get_user_runtime_dir"):
        "path_under_gvfs() compares a chosen file's path with $XDG_RUNTIME_DIR/gvfs "
        "to refuse GVfs FUSE files; it creates, writes and reads nothing there",
    # nostrc-lrac: setup_instance() reads XDG_CACHE_HOME to redirect it to an
    # instance-specific subdirectory for data isolation; it does not write
    # plaintext to the cache directory.
    ("no-tmp-cache", "src/main.c", '"XDG_CACHE_HOME"'):
        "setup_instance() reads XDG_CACHE_HOME to redirect it to the instance's "
        "subdirectory (nostrc-lrac); no plaintext is written to the cache",
}

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
    "only-join-verified-mls-groups": Key("b", "false"),
    # Charter amendment 2026-10-01 (nostrc-lf62 review L1): on until the
    # sunset criteria there are met.
    "mls-legacy-key-packages": Key("b", "true"),
    "show-message-previews": Key("b", "true"),
    # §7.9: Marmot by default; the user can change this in Preferences.
    "default-dm-protocol": Key("s", "'marmot'", ("marmot", "nip17")),
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

# Schema keys that are state, not preferences: the dialog has no row for them.
STATE_KEYS = {"current-npub", "window-width", "window-height", "window-maximized"}
PREFS_DIALOG = "src/ui/gh-preferences-dialog.c"
PREFS_DIALOG_PREFIX = "src/ui/gh-preferences-dialog."
FEATURES_FILE = "src/app/gh-features.h"
KEY_FEATURES_RE = re.compile(r"\bkey_features\s*\[\s*\]\s*=\s*\{(.*?)\n\s*\};", re.S)
KEY_FEATURE_ENTRY_RE = re.compile(r'\{\s*"([a-z0-9-]+)"\s*,\s*([A-Z0-9_|\s]+?)\s*\}')
FEATURE_NAME_RE = re.compile(r"\bGH_PREFERENCES_FEATURE_([A-Z0-9_]+)\b")
FEATURE_DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+GH_FEATURE_([A-Z0-9_]+)[ \t]+(\S+)", re.M)

LIBSOUP_DIRS = ("src/net/", "src/media/")
NIP17_PUBLICATION = re.compile(r"^(?:gh-dm-send|gh-nip17-|gh-inbox-resolver|gh-outbox)")
LOOKUP_SOURCES = re.compile(r"^(?:gh-inbox-lookup|gh-contact-directory)")
# The only files that hold the account-AUTH mechanism (charter §4.4 R1), by
# path prefix. Everything else asks GhAuthPolicy (G08).
ACCOUNT_AUTH_FILES = {
    "src/relay/": "the NIP-42 mechanism; the policy chooses the identity per URL",
    "src/app/gh-account-auth.": "GhAccountAuth, the account's generation-bound AUTH signer",
    "src/app/gh-auth-policy.": "GhAuthPolicy, the single identity decision per purpose",
}
# Where a URL's NIP-42 identity may be set: the mechanism and the policy.
URL_AUTH_FILES = ("src/relay/", "src/app/gh-auth-policy.")
# Each call that sets a URL's identity or hands out the account signer, by
# the path prefix of the file that defines it. Only that file and
# AUTH_POLICY_SOURCE may name it (account-auth-setter).
AUTH_SETTERS = {
    "gh_relay_scope_set_url_auth": "src/relay/gh-relay-scope.",
    "gh_relay_scope_set_account_signer": "src/relay/gh-relay-scope.",
    "gh_relay_publish_set_url_auth": "src/relay/gh-relay-publish.",
    "gh_relay_publish_set_account_signer": "src/relay/gh-relay-publish.",
    "gh_account_auth_new": "src/app/gh-account-auth.",
    "gh_account_auth_get_signer": "src/app/gh-account-auth.",
}
AUTH_POLICY_SOURCE = "src/app/gh-auth-policy.c"
# The only files that may name a purpose that signs in as the account, each
# with its §4.3 purpose. A new caller needs a purpose the charter allows (own
# inbox, own list publish, own self-copy publish, NIP-29 group relays) and an
# entry here; lookups and the contact directory never get one.
ACCOUNT_PURPOSE_FILES = {
    "src/app/gh-auth-policy.": "the purposes' definition",
    "src/app/gh-dm-inbox.": "own inbox read, on the account's own 10050 relays only",
    "src/app/gh-outbox.": "the self-copy (and a note to self) on the own 10050 relays",
    "src/app/gh-dm-send.": "the self-copy (and a note to self) on the own 10050 relays",
    "src/app/gh-relay-list-setup.": "own list publish (the account's kind 10002, nostrc-0bdg) "
                                    "on the relays the user chose and its discovery relays, "
                                    "after own-list discovery (ephemeral AUTH only) on the same",
    "src/app/gh-inbox-setup.": "own list publish (the account's kind 10050) on its chosen inbox, "
                               "own 10002 write and discovery relays only; its private-reads "
                               "probe never authenticates",
    "src/nip29/gh-nip29-service.": "NIP-29 group relays: the one group relay's live REQ",
    "src/nip29/gh-nip29-outbox.": "NIP-29 group relays: publish to exactly the group's relay",
    "src/mls/gh-mls-service.": "own list publish (the account's KeyPackage, kind 30443) on its "
                               "own 10002 write and 10050 inbox relays only; group relays, "
                               "lookups and Welcome wraps are ephemeral",
}
FORBIDDEN_STATUS_WORDS = {"DELIVERED", "READ", "SEEN"}
SUGGESTIONS_FILE = "data/relay-suggestions.json"
MAX_SUGGESTIONS = 6  # GH_INBOX_SETUP_MAX_SUGGESTIONS in src/app/gh-inbox-setup.h
SUGGESTION_URL_RE = re.compile(
    r"^wss://(?P<host>[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?)(?::(?P<port>\d{1,5}))?(?P<path>/[^\s?#]*)?$")

URL_RE = re.compile(r"\b(?:wss?|https?)://[^\s\"'\\<>]+", re.I)
INCLUDE_RE = re.compile(r"^[ \t]*#[ \t]*include[ \t]*[<\"]([^>\"\n]+)[>\"]", re.M)
SOUP_API_RE = re.compile(r"\b(?:soup_[a-z]\w*|Soup[A-Z]\w*|SOUP_[A-Z]\w*)\b")
SOUP_MESSAGE_NEW_RE = re.compile(r"\bsoup_message_new\w*(?=\s*\()")
NO_RESUMPTION_RE = re.compile(r"\bgh_net_tls_no_resumption(?=\s*\()")
GIO_TLS_CLIENT_RE = re.compile(r"\b(?:g_tls_client_connection_new|g_socket_client_set_tls)\b")
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
URL_AUTH_RE = re.compile(r"\bgh_relay_(?:scope|publish)_set_url_auth\b")
AUTH_SETTER_RE = re.compile(r"\b(?:" + "|".join(sorted(AUTH_SETTERS)) + r")\b")
ACCOUNT_PURPOSE_RE = re.compile(
    r"\bGH_AUTH_PURPOSE_(?:OWN_INBOX_READ|OWN_LIST_PUBLISH|SELF_WRAP|GROUP)\b")
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
        elif len(SOUP_MESSAGE_NEW_RE.findall(code)) > len(NO_RESUMPTION_RE.findall(code)):
            found += find_all("tls-resumption", rel, code, SOUP_MESSAGE_NEW_RE,
                              lambda s: f"{s}: this file makes more SoupMessages than it passes "
                                        "to gh_net_tls_no_resumption(); a message that may "
                                        "resume a TLS session links connections (PD-6)")
        found += find_all("tls-resumption", rel, code, GIO_TLS_CLIENT_RE,
                          lambda s: f"{s}: a GIO TLS client outside libsoup may store TLS "
                                    "session tickets in glib-networking's process-wide "
                                    "cache; use a libsoup message passed to "
                                    "gh_net_tls_no_resumption() (PD-6)")
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
                              lambda s: f"{s}: the account-AUTH mechanism belongs to the "
                                        "relay layer, GhAccountAuth and GhAuthPolicy; ask "
                                        "GhAuthPolicy for a purpose instead (§4.4 R1)")
        if not rel.startswith(URL_AUTH_FILES):
            found += find_all("auth-policy", rel, code, URL_AUTH_RE,
                              lambda s: f"{s}: a URL's NIP-42 identity is GhAuthPolicy's "
                                        "decision; call gh_auth_policy_apply_{scope,publish} "
                                        "with the connection's purpose (§4.3)")
        if rel.startswith(tuple(ACCOUNT_AUTH_FILES)) and rel != AUTH_POLICY_SOURCE:
            # The two rules above allow the mechanism here (and report it
            # anywhere else); each setter still belongs to its definer alone.
            found += [v for v in find_all(
                          "account-auth-setter", rel, code, AUTH_SETTER_RE,
                          lambda s: f"{s} outside {AUTH_SETTERS[s]}* and {AUTH_POLICY_SOURCE}: "
                                    "only GhAuthPolicy sets a URL's NIP-42 identity or hands "
                                    "out the account signer, so every account sign-in shares "
                                    "its one signer per generation (§4.4 R1, R6)")
                      if not rel.startswith(AUTH_SETTERS[v.match])]
        if not rel.startswith(tuple(ACCOUNT_PURPOSE_FILES)):
            found += find_all("auth-policy", rel, code, ACCOUNT_PURPOSE_RE,
                              lambda s: f"{s} signs in as the account: only for a purpose "
                                        "§4.4 R1 allows; add the file to ACCOUNT_PURPOSE_FILES "
                                        "with its §4.3 purpose")
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


def banned_relay_hosts(root):
    """Hosts AGENTS.md bans, from the "never" sentences of its Banned Relays
    section (the nearest AGENTS.md at or above root, up to two levels)."""
    root = Path(root).resolve()
    for directory in (root, root.parent, root.parent.parent):
        agents = directory / "AGENTS.md"
        if not agents.is_file():
            continue
        text = agents.read_text(encoding="utf-8", errors="replace")
        m = re.search(r"^(#+)\s*Banned Relays\s*$", text, re.M | re.I)
        if not m:
            return agents, set()
        level = len(m.group(1))
        rest = text[m.end():]
        end = re.search(r"^#{1,%d}\s" % level, rest, re.M)
        section = rest[:end.start()] if end else rest
        hosts = set()
        for sentence in re.split(r"(?<=[.!?])\s+", section):
            if re.search(r"\bnever\b", sentence, re.I):
                for token in re.findall(r"`([^`\s]+)`", sentence):
                    host = re.sub(r"^wss?://", "", token.lower()).split("/")[0]
                    if "." in host:
                        hosts.add(host)
        return agents, hosts
    return None, set()


def check_relay_suggestions(tree):
    rule, rel = "relay-suggestions", SUGGESTIONS_FILE
    if not tree.exists(rel):
        return [Violation(rule, rel, 0, "the reviewed relay suggestions file is missing", "")]
    agents, banned = banned_relay_hosts(tree.root)
    if not banned:
        return [Violation(rule, rel, 0, "cannot read the banned relays from "
                          f"{agents or 'AGENTS.md'} (a \"Banned Relays\" section naming them in a "
                          "\"never\" sentence), so the suggestions cannot be checked", "")]
    text, _, _ = tree.views(rel)
    try:
        data = json.loads(text)
    except ValueError as error:
        return [Violation(rule, rel, 0, f"invalid JSON: {error}", "")]

    def at(value):
        return line_of(text, text.find(value)) if value and value in text else 0

    found = []
    relays = data.get("relays") if isinstance(data, dict) else None
    if not isinstance(data, dict) or not str(data.get("reviewed", "")).strip():
        found.append(Violation(rule, rel, 0, "no \"reviewed\" date: say when the list was "
                               "last reviewed", "reviewed"))
    if not isinstance(relays, list) or not 1 <= len(relays) <= MAX_SUGGESTIONS:
        count = len(relays) if isinstance(relays, list) else "no"
        return found + [Violation(rule, rel, 0, f"{count} relays: keep the reviewed list short "
                                  f"(1..{MAX_SUGGESTIONS})", "relays")]
    seen = set()
    for index, entry in enumerate(relays):
        entry = entry if isinstance(entry, dict) else {}
        url = entry.get("url") if isinstance(entry.get("url"), str) else ""
        where = at(f'"{url}"') if url else 0
        for field in ("url", "name", "description", "private_reads"):
            if not isinstance(entry.get(field), str) or not entry[field].strip():
                found.append(Violation(rule, rel, where, f"entry {index} has no {field}",
                                       f"{index}:{field}"))
        m = SUGGESTION_URL_RE.match(url)
        port = int(m.group("port")) if m and m.group("port") else None
        if url and (not m or "." not in m.group("host") or ".." in m.group("host")
                    or (port is not None and not 0 < port < 65536)
                    or (m.group("path") or "").endswith("/")):
            found.append(Violation(rule, rel, where, f"{url!r} is not a normalized secure relay "
                                   "address (wss://, lower-case host, no trailing slash, "
                                   "user name, query or fragment)", url))
        if m:
            host = m.group("host")
            for ban in sorted(banned):
                if host == ban or host.endswith("." + ban):
                    found.append(Violation(rule, rel, where, f"entry {index} names a relay "
                                           "AGENTS.md bans; it must never be suggested",
                                           f"{index}:banned"))
        if url in seen:
            found.append(Violation(rule, rel, where, f"{url!r} is listed twice", url))
        seen.add(url)
        reads = entry.get("private_reads")
        if isinstance(reads, str) and reads.strip():
            if reads not in ("yes", "no", "unknown"):
                found.append(Violation(rule, rel, where, f"private_reads {reads!r} must be "
                                       "\"yes\", \"no\" or \"unknown\"", f"{index}:private_reads"))
            elif reads != "unknown" and not str(entry.get("evidence", "")).strip():
                found.append(Violation(rule, rel, where, f"entry {index} claims private_reads "
                                       f"{reads!r} without review evidence; say \"unknown\" "
                                       "unless it was checked", f"{index}:evidence"))
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


def check_preference_consumers(tree):
    """W13b review B2: no Preferences row without an effect in this build."""
    rule = "preference-consumers"
    if not tree.exists(PREFS_DIALOG):
        return []
    _, keep, _ = tree.views(PREFS_DIALOG)
    bound = [key for key in GSETTINGS if key not in STATE_KEYS and f'"{key}"' in keep]
    features = {}
    if tree.exists(FEATURES_FILE):
        _, features_keep, _ = tree.views(FEATURES_FILE)
        features = {m.group(1): m.group(2) for m in FEATURE_DEFINE_RE.finditer(features_keep)}
    found, needs = [], {}
    table = KEY_FEATURES_RE.search(keep)
    for entry in KEY_FEATURE_ENTRY_RE.finditer(table.group(1)) if table else ():
        key, names = entry.group(1), FEATURE_NAME_RE.findall(entry.group(2))
        line = line_of(keep, table.start(1) + entry.start())
        needs[key] = names
        if key not in GSETTINGS or key in STATE_KEYS:
            found.append(Violation(rule, PREFS_DIALOG, line, f"key_features gates {key!r}, which "
                                   "is not a preference key", key))
        for name in names:
            if name not in features:
                found.append(Violation(rule, PREFS_DIALOG, line,
                                       f"key_features gates {key!r} on GH_PREFERENCES_FEATURE_"
                                       f"{name}, but {FEATURES_FILE} has no GH_FEATURE_{name}: "
                                       "the build's features are listed there, once",
                                       f"{key}:{name}"))
    consumers = set()
    for rel in tree.files("src", suffixes={".c", ".h"}):
        if rel.startswith(PREFS_DIALOG_PREFIX) or rel == FEATURES_FILE:
            continue
        _, other, _ = tree.views(rel)
        consumers |= {key for key in bound if f'"{key}"' in other}
    for key in bound:
        if key in consumers or any(features.get(name) == "0" for name in needs.get(key, ())):
            continue
        found.append(Violation(rule, PREFS_DIALOG, line_of(keep, keep.find(f'"{key}"')),
                               f"preference {key!r} has a row in the Preferences dialog but "
                               "nothing outside it reads it: gate the row (a key_features "
                               f"entry whose GH_FEATURE_* is 0 in {FEATURES_FILE}) or land its "
                               "consumer (charter §7.11: no fake support)", key))
    return found


def check(root, exceptions=None):
    """Return the violations in the Groundhog tree at `root`."""
    exceptions = EXCEPTIONS if exceptions is None else exceptions
    tree = Tree(root)
    raw = (check_url_literals(tree) + check_gsettings(tree) + check_blueprint(tree)
           + check_sources(tree) + check_message_status(tree) + check_relay_suggestions(tree)
           + check_app_id(tree) + check_preference_consumers(tree))
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
        # Account AUTH where §4.3 allows it: the mechanism, its adapter and the
        # policy hold it; the own inbox and the self-copy ask the policy.
        "src/relay/gh-relay-auth.h": (
            "typedef enum { GH_RELAY_AUTH_NONE, GH_RELAY_AUTH_EPHEMERAL,\n"
            "               GH_RELAY_AUTH_ACCOUNT } GhRelayAuthMode;\n"),
        "src/app/gh-account-auth.c": (
            "#include \"gh-account-auth.h\"\n"
            "G_DEFINE_FINAL_TYPE(GhAccountAuth, gh_account_auth, G_TYPE_OBJECT)\n"
            "GhRelayAuthSigner *gh_account_auth_get_signer(GhAccountAuth *self) {\n"
            "  return self->signer;\n"
            "}\n"),
        # The setters' own definitions (and a comment naming another) are
        # not settings of anyone's identity.
        "src/relay/gh-relay-publish.c": (
            "/* Like gh_relay_scope_set_url_auth(), per URL. */\n"
            "gboolean\n"
            "gh_relay_publish_set_url_auth(GhRelayPublish *p, const char *u, GhRelayAuthMode m,\n"
            "                              GError **e)\n"
            "{\n"
            "  return m != GH_RELAY_AUTH_ACCOUNT || (p->signer && u && e);\n"
            "}\n"),
        "src/app/gh-auth-policy.c": (
            "#include \"gh-account-auth.h\"\n"
            "static const int modes[] = { [GH_AUTH_PURPOSE_SELF_WRAP] = GH_RELAY_AUTH_ACCOUNT };\n"
            "static int own(GhRelayScope *s, GhAccountAuth *a, const char *u) {\n"
            "  return gh_relay_scope_set_account_signer(s, gh_account_auth_get_signer(a), NULL) &&\n"
            "         gh_relay_scope_set_url_auth(s, u, GH_RELAY_AUTH_ACCOUNT, NULL);\n"
            "}\n"),
        "src/app/gh-outbox.c": (
            "/* Never gh_relay_publish_set_url_auth() here: GhAuthPolicy decides. */\n"
            "static int p(GhRelayPublish *pub, int self, const char *u) {\n"
            "  return gh_auth_policy_apply_publish(NULL, pub, self ? GH_AUTH_PURPOSE_SELF_WRAP\n"
            "                                      : GH_AUTH_PURPOSE_RECIPIENT_WRAP, u, NULL);\n"
            "}\n"),
        # Own list publish signs in as the account through the policy; own
        # list discovery asks it for a throwaway key.
        "src/app/gh-inbox-setup.c": (
            "#include \"gh-auth-policy.h\"\n"
            "static int own(GhAuthPolicy *p, GhRelayPublish *pub, const char *u) {\n"
            "  return gh_auth_policy_apply_publish(p, pub, GH_AUTH_PURPOSE_OWN_LIST_PUBLISH, u, NULL);\n"
            "}\n"),
        "src/app/gh-account-relays.c": (
            "static int d(GhAuthPolicy *p, GhRelayScope *s, const char *u) {\n"
            "  return gh_auth_policy_apply_scope(p, s, GH_AUTH_PURPOSE_OWN_LIST_DISCOVERY, u, NULL);\n"
            "}\n"),
        "src/app/gh-contact-directory.c": (
            "/* Discovery only: GH_AUTH_PURPOSE_CONTACT_DIRECTORY, never GH_AUTH_PURPOSE_SELF_WRAP. */\n"
            "static int d(GhRelayScope *s, const char *u) {\n"
            "  return gh_auth_policy_apply_scope(NULL, s, GH_AUTH_PURPOSE_CONTACT_DIRECTORY, u, NULL);\n"
            "}\n"),
        "src/app/gh-dm-inbox.c": (
            "#include \"gh-auth-policy.h\"\n"
            "static int own(GhAuthPolicy *p, GhRelayScope *s, const char *u) {\n"
            "  return gh_auth_policy_apply_scope(p, s, GH_AUTH_PURPOSE_OWN_INBOX_READ, u, NULL);\n"
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
        # Each message made is passed on; the comment's soup_message_new( and
        # the string's g_tls_client_connection_new are not code.
        "src/net/gh-net-http.c": (
            "/* soup_message_new() without resumption: */\n"
            "static SoupMessage *get(GUri *u) {\n"
            "  SoupMessage *m = soup_message_new_from_uri(\"GET\", u);\n"
            "  gh_net_tls_no_resumption(m);\n"
            "  return m;\n"
            "}\n"
            "static const char *why = \"not g_tls_client_connection_new\";\n"),
        "src/media/gh-blossom-client.c": "#include <libsoup/soup.h>\n",
        # Preferences: one live row with a consumer, one gated row without one
        # (its only mention elsewhere is a comment), and one row gated on a
        # feature that is on and a build-dependent one, which needs a consumer.
        "src/ui/gh-preferences-dialog.c": (
            "static const struct { const gchar *key; GhPreferencesFeatures needs; } "
            "key_features[] = {\n"
            '  { "enter-sends", GH_PREFERENCES_FEATURE_COMPOSER },\n'
            '  { "link-previews", GH_PREFERENCES_FEATURE_LINK_PREVIEWS | '
            "GH_PREFERENCES_FEATURE_TOR },\n"
            "};\n"
            "static void bind(GSettings *s, GObject *row) {\n"
            '  g_settings_bind(s, "show-message-previews", row, "active", 0);\n'
            '  g_settings_bind(s, "enter-sends", row, "active", 0);\n'
            '  g_settings_bind(s, "link-previews", row, "active", 0);\n'
            "}\n"),
        "src/app/gh-features.h": (
            "#define GH_FEATURE_COMPOSER 0 /* G13 */\n"
            "#define GH_FEATURE_LINK_PREVIEWS 1\n"
            "#define GH_FEATURE_TOR GROUNDHOG_HAVE_TOR\n"),
        "src/ui/gh-conversation-list.h": (
            '#define GH_CONVERSATION_LIST_PREVIEWS_KEY "show-message-previews"\n'),
        "src/ui/gh-conversation-view.c": (
            '/* "enter-sends" is G13\'s composer\'s to read. */\n'
            'static const char *const previews = "link-previews";\n'),
        SCHEMA_FILE: render_schema(),
        # The banned host is named only in the synthetic AGENTS.md, and the
        # host it recommends instead is not banned.
        "AGENTS.md": (
            "# Agents\n\n## Banned Relays\n\n"
            "**NEVER add `banned.example.org` (or `wss://banned.example.org`) anywhere.** "
            "It is unreliable. Use `wss://good.example.org` instead.\n\n## Next\n\n"
            "Never mind `other.example.org`.\n"),
        SUGGESTIONS_FILE: json.dumps({
            "reviewed": "2026-09-28",
            "relays": [
                {"url": "wss://good.example.org", "name": "Good", "description": "Reviewed.",
                 "private_reads": "yes", "evidence": "synthetic: refused an unauthenticated REQ"},
                {"url": "wss://other.example.org:4443/inbox", "name": "Other",
                 "description": "Reviewed.", "private_reads": "unknown"},
            ],
        }, indent=2) + "\n",
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
    M("tls-message-resumable", {"tls-resumption"},
      [append("src/media/gh-blossom-client.c",
              "static SoupMessage *m(GUri *u) { return soup_message_new_from_uri(\"GET\", u); }\n")]),
    M("tls-second-message", {"tls-resumption"},
      [append("src/net/gh-net-http.c",
              "static SoupMessage *head(const char *u) { return soup_message_new(\"HEAD\", u); }\n")]),
    M("tls-gio-client", {"tls-resumption"},
      [append("src/app/gh-fetch.c",
              "static GIOStream *t(GIOStream *b) { return g_tls_client_connection_new(b, NULL, NULL); }\n")]),
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
    M("account-auth-inbox-adapter", {"account-auth-purpose"},
      [append("src/app/gh-dm-inbox.c",
              "static void *s(GhAccountAuth *a) { return gh_account_auth_get_signer(a); }\n")]),
    M("auth-policy-outbox-bypass", {"auth-policy"},
      [append("src/app/gh-outbox.c",
              "static int e(GhRelayPublish *p, const char *u) {\n"
              "  return gh_relay_publish_set_url_auth(p, u, GH_RELAY_AUTH_EPHEMERAL, NULL);\n}\n")]),
    M("auth-policy-scope-bypass", {"auth-policy"},
      [append("src/app/gh-inbox-lookup.c", "static void *f = (void *)gh_relay_scope_set_url_auth;\n")]),
    M("auth-policy-directory-self-wrap", {"auth-policy"},
      [append("src/app/gh-contact-directory.c", "static const int purpose = GH_AUTH_PURPOSE_SELF_WRAP;\n")]),
    M("auth-policy-lookup-own-inbox", {"auth-policy"},
      [append("src/app/gh-inbox-lookup.c",
              "static const int purpose = GH_AUTH_PURPOSE_OWN_INBOX_READ;\n")]),
    M("auth-policy-ui-group", {"auth-policy"},
      [append("src/ui/gh-group-view.c", "static const int purpose = GH_AUTH_PURPOSE_GROUP;\n")]),
    M("auth-policy-own-discovery-as-account", {"auth-policy"},
      [replace("src/app/gh-account-relays.c", "GH_AUTH_PURPOSE_OWN_LIST_DISCOVERY",
               "GH_AUTH_PURPOSE_OWN_LIST_PUBLISH")]),
    M("setter-relay-layer", {"account-auth-setter"},
      [append("src/relay/gh-relay-guard.c",
              "static int g(GhRelayPublish *p, const char *u) {\n"
              "  return gh_relay_publish_set_url_auth(p, u, GH_RELAY_AUTH_ACCOUNT, NULL);\n}\n")]),
    M("setter-account-auth-adapter", {"account-auth-setter"},
      [append("src/app/gh-account-auth.c",
              "static int s(GhRelayScope *sc, GhAccountAuth *a) {\n"
              "  return gh_relay_scope_set_account_signer(sc, gh_account_auth_get_signer(a), NULL);\n"
              "}\n")]),
    M("setter-policy-header", {"account-auth-setter"},
      [append("src/app/gh-auth-policy.h",
              "static inline void *a(GhAccountController *c) { return gh_account_auth_new(c); }\n")]),
    M("setter-other-definer", {"account-auth-setter"},
      [append("src/relay/gh-relay-publish.c",
              "static void *f = (void *)gh_relay_scope_set_account_signer;\n")]),
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
    M("suggestions-banned-host", {"relay-suggestions"},
      [replace(SUGGESTIONS_FILE, "wss://other.example.org:4443/inbox", "wss://banned.example.org")]),
    M("suggestions-banned-subdomain", {"relay-suggestions"},
      [replace(SUGGESTIONS_FILE, "wss://other.example.org:4443/inbox",
               "wss://inbox.banned.example.org")]),
    M("suggestions-insecure", {"relay-suggestions"},
      [replace(SUGGESTIONS_FILE, "wss://good.example.org", "ws://good.example.org")]),
    M("suggestions-not-normalized", {"relay-suggestions"},
      [replace(SUGGESTIONS_FILE, "wss://good.example.org", "wss://Good.example.org/")]),
    M("suggestions-query", {"relay-suggestions"},
      [replace(SUGGESTIONS_FILE, "wss://good.example.org", "wss://good.example.org/?x=1")]),
    M("suggestions-unproven-claim", {"relay-suggestions"},
      [replace(SUGGESTIONS_FILE, '"private_reads": "unknown"', '"private_reads": "no"')]),
    M("suggestions-bad-reads", {"relay-suggestions"},
      [replace(SUGGESTIONS_FILE, '"private_reads": "unknown"', '"private_reads": "maybe"')]),
    M("suggestions-duplicate", {"relay-suggestions"},
      [replace(SUGGESTIONS_FILE, "wss://other.example.org:4443/inbox", "wss://good.example.org")]),
    M("suggestions-too-many", {"relay-suggestions"},
      [lambda tree: tree.__setitem__(SUGGESTIONS_FILE, json.dumps({"reviewed": "2026-09-28", "relays": [
          {"url": f"wss://r{i}.example.org", "name": "R", "description": "D",
           "private_reads": "unknown"} for i in range(MAX_SUGGESTIONS + 1)]}))]),
    M("suggestions-no-banned-list", {"relay-suggestions"},
      [replace("AGENTS.md", "## Banned Relays", "## Relays")]),
    M("suggestions-missing", {"relay-suggestions"},
      [lambda tree: tree.pop(SUGGESTIONS_FILE)]),
    # G14 once set account AUTH itself; since W19 (nostrc-qp24.65) it asks
    # the policy like everyone else.
    M("account-auth-inbox-setup-direct", {"account-auth-purpose", "auth-policy"},
      [append("src/app/gh-inbox-setup.c",
              "static int direct(GhRelayPublish *p, const char *u) {\n"
              "  return gh_relay_publish_set_url_auth(p, u, GH_RELAY_AUTH_ACCOUNT, NULL);\n}\n")]),
    M("app-id-blueprint", {"app-id"},
      [replace("data/ui/gh-window.blp", f'icon-name: "{APP_ID}"', f'icon-name: "{APP_ID}.Devel"')]),
    M("app-id-ui", {"app-id"},
      [replace("data/ui/gh-window.ui", f">{APP_ID}</property>", ">org.example.Other</property>")]),
    M("app-id-define", {"app-id"},
      [replace("src/main.c", f'"{APP_ID}"', '"org.example.Groundhog"')]),
    M("app-id-schema", {"app-id"},
      [replace(SCHEMA_FILE, f'path="{SCHEMA_PATH}"', 'path="/org/example/Groundhog/"')]),
    M("prefs-gate-flipped-without-consumer", {"preference-consumers"},
      [replace("src/app/gh-features.h", "GH_FEATURE_COMPOSER 0", "GH_FEATURE_COMPOSER 1")]),
    M("prefs-gate-build-macro-without-consumer", {"preference-consumers"},
      [replace("src/app/gh-features.h", "GH_FEATURE_COMPOSER 0",
               "GH_FEATURE_COMPOSER GROUNDHOG_HAVE_COMPOSER")]),
    M("prefs-consumer-only-in-comment", {"preference-consumers"},
      [replace("src/ui/gh-conversation-list.h",
               '#define GH_CONVERSATION_LIST_PREVIEWS_KEY "show-message-previews"',
               '/* "show-message-previews" is read here one day. */')]),
    M("prefs-new-row-without-consumer", {"preference-consumers"},
      [replace("src/ui/gh-preferences-dialog.c", "static void bind(GSettings *s, GObject *row) {\n",
               "static void bind(GSettings *s, GObject *row) {\n"
               '  g_settings_bind(s, "load-remote-images", row, "active", 0);\n')]),
    M("prefs-gate-unknown-feature", {"preference-consumers"},
      [replace("src/ui/gh-preferences-dialog.c", "GH_PREFERENCES_FEATURE_COMPOSER }",
               "GH_PREFERENCES_FEATURE_SENDING }")]),
    M("prefs-new-row-gated", set(),
      [replace("src/ui/gh-preferences-dialog.c", "static void bind(GSettings *s, GObject *row) {\n",
               "static void bind(GSettings *s, GObject *row) {\n"
               '  g_settings_bind(s, "load-remote-images", row, "active", 0);\n'),
       replace("src/ui/gh-preferences-dialog.c", "};\nstatic void bind",
               '  { "load-remote-images", GH_PREFERENCES_FEATURE_REMOTE_IMAGES },\n};\n'
               "static void bind"),
       append("src/app/gh-features.h", "#define GH_FEATURE_REMOTE_IMAGES 0\n")]),
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
