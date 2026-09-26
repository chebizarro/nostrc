# nostr-nautilus — Nostr in the GNOME Files context menu

A `libnautilus-extension-4` module (Nautilus 43+, GTK 4 API) that adds
right-click items to GNOME Files. It is the modern analog of the retired
`seahorse-nautilus`: the menu contains **no Nostr code**. Each item launches
the matching CLI, and that CLI does all the UI, signing and networking.

- `nostr-share` (bead nostrc-1xak) publishes files.
- `nostr-seal` (bead nostrc-da9c) seals and opens `.nsealed` files.

Bead: nostrc-xlf3.

## Menu items

| Item | Shown when (every selected file must qualify) | Runs |
|---|---|---|
| **Share to Nostr…** | `nostr-share` is installed and the file's MIME type is covered by `org.nostr.Share.desktop`'s `MimeType=` (read at runtime; wildcards and shared-mime-info subclasses count, the same as *Open With*). For a directory, only a git repository (`.git`, or a bare layout), because nostr-share refuses other directories | `nostr-share FILE…` |
| **Upload to Blossom…** | a regular file that `nostr-share --kind 1063` accepts: media, PDF and any other binary (including `.nsealed`). Text, Markdown, `.ics` and `.vcf` are excluded because nostr-share rejects kind 1063 for them. The decision uses nostr-share's own `ns_kind_classify()`/`ns_kind_resolve()` (compiled in), so the menu can't drift from the CLI | `nostr-share --kind 1063 FILE…` |
| **Encrypt for Nostr Contact…** | a regular **local** file that is not already sealed, with `nostr-seal` and the helper installed | `nostr-nautilus-seal FILE…` → per file `nostr-seal encrypt --to NPUB… [--to-self] FILE` |
| **Decrypt…** | a local `application/vnd.nostr.sealed` / `*.nsealed` file | `nostr-seal-gtk FILE…` (nostr-seal's own open dialog); if only the CLI is installed, `nostr-seal decrypt FILE` once per file |
| **Share This Repository to Nostr…** (folder background) | the current folder is a local git repository | `nostr-share DIR` (NIP-34 kind 30617) |

Local files are passed as absolute paths. Remote (GVfs) files are passed as
URIs, which nostr-share accepts (`Exec=nostr-share %U`). nostr-seal needs
local paths, so Encrypt and Decrypt are hidden for remote files.

nostr-share opens its preview dialog whenever a display is present. Nothing
is uploaded or signed until you press **Publish** there.

### Encrypt for Nostr Contact…

`nostr-seal-gtk` only opens sealed files and has no recipient picker. For
sealing, this package therefore ships a minimal libadwaita dialog,
`$libexecdir/nostr-nautilus-seal`, with:

- one entry row for npubs, separated by spaces, commas or newlines;
- an optional `nostr:` prefix and 64-hex keys are also accepted;
- a bech32 checksum check on each npub, to catch typos before running
  nostr-seal (nostr-seal re-decodes every key and stays the authority);
- **Also encrypt for me** (on by default, maps to `--to-self`) so your
  signer's identity can open the result too.

It runs `nostr-seal encrypt` once per file. The output is `FILE.nsealed`
next to the original, and nostr-seal refuses to overwrite an existing one.
nostr-seal's error text is shown as-is.

v1 seals **files only**. Directories would need a tar step first
(`tar c dir/ | nostr-seal encrypt --to … -o dir.tar.nsealed -`), and there
is no contact list: you paste npubs.

## Cheap by design

`get_file_items` runs on Nautilus' main thread for every right-click. The
provider uses only:

- what Nautilus already knows about each file (MIME type, file type,
  location);
- up to four `stat()`s per selected directory for the git test;
- `PATH` lookups for the tools;
- GLib's cached desktop-file index to read the `MimeType=` list.

There is no file I/O on regular files, no network access and no signer call.
Tools are looked up on each request, so installing `nostr-seal` makes its
items appear without restarting Files. A selection that contains a
non-repository directory or a special file stops at that file.

Launching goes through a `GAppInfo` (`g_app_info_create_from_commandline`
with a correctly escaped Exec line) and GDK's launch context. That context
supplies a startup-notification / xdg-activation token, so the tool's
window is raised in front of Files instead of opening behind it.

## Disabling

- **Uninstall** the `nostr-nautilus` package (or remove
  `libnostr-nautilus.so` from
  `$(pkg-config --variable=extensiondir libnautilus-extension-4)`), then run
  `nautilus -q`.
- **Temporarily**: start Files with `NOSTR_NAUTILUS_DISABLE=1`. The module
  loads but registers no provider.
- Individual items disappear when their tool is not installed. For example,
  without `nostr-seal` there is no Encrypt or Decrypt item.

## Debugging

```sh
nautilus -q
G_MESSAGES_DEBUG=nostr-nautilus nautilus --new-window ~/Pictures
# nostr-nautilus-DEBUG: initialized (encrypt helper /usr/libexec/nostr-nautilus-seal)
# nostr-nautilus-DEBUG: 1 file(s): 3 item(s)
```

## Building

```sh
cmake -B build -DENABLE_NOSTR_NAUTILUS=ON -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build -L nostr-nautilus
```

`ENABLE_NOSTR_NAUTILUS` defaults to ON only when `libnautilus-extension-4`
(≥ 43) is found. Without it, only the core library and its test are built
(this is how it builds on macOS). Dependencies:

- the module needs GLib/GIO ≥ 2.74, `libnautilus-extension-4` and
  `gio-unix-2.0`;
- GTK 4 is optional, for the launch context;
- the Encrypt dialog needs libadwaita ≥ 1.4. Turn it off with
  `-DNOSTR_NAUTILUS_ENABLE_SEAL_HELPER=OFF`, which also hides the item.

The module installs to `${CMAKE_INSTALL_LIBDIR}/nautilus/extensions-4`,
which matches the pkg-config `extensiondir` on Debian and Fedora.
Override it with `-DNOSTR_NAUTILUS_EXTENSION_DIR=…`.

Tests (no Nautilus process, no network):

- `nostr_nautilus_core` covers:
  - the gating table for every item across MIME types, sealed/unsealed,
    git/plain directories, remote files, multi-selection and missing tools;
  - the argv for each action;
  - Exec-line escaping, round-tripped through a real `GDesktopAppInfo`
    launch with hostile filenames (`%f`, quotes, `$`, backticks, newlines);
  - npub parsing and checksums;
  - when the `nostr-seal` target is built, the dialog's argv run against the
    real CLI plus `nostr-seal inspect`.
- `nostr_nautilus_provider` (needs `libnautilus-extension-4`) covers:
  - the real module entry points;
  - `NautilusMenuProvider` against fake `NautilusFileInfo` objects, using the
    real `org.nostr.Share.desktop` and fake tools on a private `PATH`;
  - activation argv captured per item;
  - the background item, `NOSTR_NAUTILUS_DISABLE` and the missing-tools case.

## Not yet

- **"nostr-published" emblem.** This would need nostr-share to record what it
  published, for example an xattr `user.nostr.event` holding the event id.
  nostr-share does not write one today; that is bead **nostrc-tepd**. Once it
  does, a `NautilusInfoProvider` here adds the emblem. There is no properties
  page.
- Encrypting directories (tar first) and a contact picker fed from your
  kind-3 follows.
