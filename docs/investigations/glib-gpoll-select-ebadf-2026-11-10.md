# GLib select fallback: report invalid descriptors instead of failing the poll

10 November 2026. Local upstream-patch draft; nothing submitted upstream or pushed.
Related nostrc context: `nostrc-kbcje`, `nostrc-pm7ol`, `nostrc-by2qt` and
`nostrc-5aaq1`. This documentation task is `nostrc-zi27r`; it does not close those
bugs or replace the separate nostrc fixture-teardown work.

## Source and mechanism

The accompanying [format-patch](glib-gpoll-select-ebadf-2026-11-10.patch) is against
GLib main `107dde15d2961a7080f8d58ca27707d69df2d819` (9 November 2026,
version 2.91.0). Its upstream draft commit is
`7ab80377dd952dc04a9e466aa7971598fcd4b784`. A fresh scratch worktree at the base
accepted `git am`; its resulting tree matched the tested patched tree exactly.

The GLib 2.90.0 tag was also inspected. Its
[`meson.build:306–309`](https://gitlab.gnome.org/GNOME/glib/-/blob/2.90.0/meson.build#L306-309)
sets `BROKEN_POLL` for Darwin and Windows. In
[`glib/gpoll.c:63–73`](https://gitlab.gnome.org/GNOME/glib/-/blob/2.90.0/glib/gpoll.c#L63-73),
that definition disables `HAVE_POLL`, selecting the Unix `select()` emulation on
macOS rather than the direct `poll()` wrapper. The corresponding main-branch
build condition is at
[`meson.build:321–324`](https://gitlab.gnome.org/GNOME/glib/-/blob/107dde15d2961a7080f8d58ca27707d69df2d819/meson.build#L321-324).

In both inspected revisions, the unpatched
[`glib/gpoll.c:616–635`](https://gitlab.gnome.org/GNOME/glib/-/blob/107dde15d2961a7080f8d58ca27707d69df2d819/glib/gpoll.c#L616-635)
returns `select()`'s result without recovering from `EBADF`. One closed descriptor
therefore fails the whole call; the normal positive-result loop does not fill
`revents`. In
[`glib/gmain.c:5007–5012`](https://gitlab.gnome.org/GNOME/glib/-/blob/107dde15d2961a7080f8d58ca27707d69df2d819/glib/gmain.c#L5007-5012),
`g_main_context_poll_unlocked()` warns on negative results other than `EINTR`.
Under `g_test_init()` that warning is fatal. This differs from `poll()` reporting
`POLLNVAL` on the individual invalid entries. The build's `POLLNVAL` mapping is in
[`meson.build:2048`](https://gitlab.gnome.org/GNOME/glib/-/blob/107dde15d2961a7080f8d58ca27707d69df2d819/meson.build#L2048),
and the context propagates `G_IO_NVAL` regardless of requested events at
[`glib/gmain.c:4452–4456`](https://gitlab.gnome.org/GNOME/glib/-/blob/107dde15d2961a7080f8d58ca27707d69df2d819/glib/gmain.c#L4452-4456).

The related nostrc beads record GIO closing a listener or GDBus socket while a
source still watches its descriptor. This draft addresses the poll-backend
behavior exposed by that lifetime window; it does not make close-before-source-
removal ordering safe, prevent fd reuse, or claim to fix every GIO lifetime race.

Ordinary Linux builds retain `HAVE_POLL`: the unmodified
[`glib/gpoll.c:126–132`](https://gitlab.gnome.org/GNOME/glib/-/blob/107dde15d2961a7080f8d58ca27707d69df2d819/glib/gpoll.c#L126-132)
uses native `poll()`, and the default main-context path can use `ppoll()` at
`glib/gmain.c:4980–4995`. Those backends already report invalid entries individually;
this change does not compile into their `g_poll()` implementation. This is a
source-based explanation, not a new Linux execution result.

## Patch rationale and boundaries

Only the Unix fallback changes. After `select()` returns `-1/EBADF`, one linear
scan clears every entry's `revents` and probes nonnegative descriptors with
`fcntl(fd, F_GETFD)`. Only a probe returning `-1/EBADF` marks `G_IO_NVAL`. The
return value counts marked array entries, including duplicates, not distinct fd
numbers. Negative descriptors remain ignored; a zero-event entry for the same
invalid fd is also marked, as with `poll()`.

There is no retry or new successful-path overhead. If all offending descriptors
have disappeared or been reused before the scan, the function still returns
`-1/EBADF`, rather than spinning or inventing readiness. Other `select()` errors
are unchanged. All paths reach the existing `g_free(heap_storage)`, including
fds beyond `FD_SETSIZE`. Valid entries are cleared in this recovery result;
readiness for them is left to a subsequent iteration. This is deliberately not
a redesign of every `select()`/`poll()` semantic difference, including the case
where an invalid descriptor with no requested events alone never reaches
`select()`'s fd sets.

The patch adds Unix regressions to existing test programs rather than introducing
a new runner. `poll-fdset` covers ordinary and high invalid fds, duplicate entries,
a valid non-ready entry, a zero-event invalid entry, ignored negative fds, stale
`revents`, and the returned count. `mainloop` attaches a Unix-fd source, closes its
fd, and requires a nonblocking context iteration to dispatch exactly `G_IO_NVAL`
and remove that source without a warning. Its private context is created before
the close, avoiding incidental fd reuse by context initialization.

## Native GLib verification

Execution was on arm64 macOS 27.0.1 (26A434), Apple clang 21.0.0, Meson 1.11.1 and
Ninja 1.13.2. Homebrew already supplied Meson/Ninja; no package installation or
system-library replacement was needed. GLib was cloned into
`/tmp/nostrc-glib-ebadf-20261110`, without installing it:

```sh
meson setup "$GLIB/_build" "$GLIB" -Dintrospection=disabled \
  -Ddocumentation=false -Dman-pages=disabled -Dsysprof=disabled
ninja -C "$GLIB/_build" -j 6 glib/tests/mainloop glib/tests/poll-fdset glib/tests/unix
meson test -C "$GLIB/_build" --no-rebuild --suite glib \
  mainloop poll-fdset unix --print-errorlogs
```

The final run passed **3/3 test programs**: `mainloop` **50 subtests passed,
2 timing-heavy subtests skipped** (8.89 s), `poll-fdset` **3 passed, none skipped**
(0.01 s), and `unix` **22 passed, 1 platform skip** (6.73 s). The default mainloop
mode skips `/mainloop/timeouts` and `/mainloop/timeouts_ns`; the Unix suite skips
`/glib-unix/child-wait/no-waitid-pidfd` because child watches do not use pidfds
on this platform. All three new regressions and the existing high-fd test ran;
none of those skipped. An earlier patched run also passed `mainloop` and
`poll-fdset`. Meson currently labels these `glib+core - glib:…`;
`glib/tests/gpoll.c` is a Windows-only program, selected only under the Windows
condition in `glib/tests/meson.build:264–271`. It was not run on macOS;
`poll-fdset` exercises the relevant Unix `g_poll()` path.

For negative controls, the new tests were first compiled against the base
`gpoll.c`. `/mainloop/unix-fd-invalid` terminated with SIGTRAP and the fatal
bad-descriptor warning. Both `/glib/poll/invalid-fd` and
`/glib/poll/invalid-high-fd` terminated with SIGABRT because `g_poll()` returned
`-1` instead of the expected three entries. Replacing only `gpoll.c` with the
patch made all three pass as part of the complete programs above.

A separate scratch diagnostic compiled the patched `gpoll.c` with a renamed
`select()` stub. With a valid fd and an injected `EBADF`, it verified one call,
cleared `revents`, and `-1/EBADF`; an injected `EINVAL` remained `-1/EINVAL` with
no retry. This is fault-injection evidence, not a real concurrent-fd-reuse test
or an additional upstream test. Its initial link failed over Darwin's select
symbol suffix; compiling the stub with `_DARWIN_UNLIMITED_SELECT` corrected it.
No sanitizer, Linux, Windows, hosted-CI, or unrestricted full-GLib run is claimed.

## nostrc library-swap demonstration

The nostrc source baseline was `8d6bec4928c7b795b145c4b48c201551a9ab369b`.
No nostrc source or test files were edited, including the sibling-owned bus and
fixtures. The first configure failed because this fresh worktree lacked nostrdb.
An initial broad submodule initialization also encountered the configured local
`docs/nips` URL's file-transport restriction. Initializing only `third_party/nostrdb`
and `third_party/nsync` supplied the build prerequisites; no transport policy was
changed. The native build then succeeded:

```sh
cmake -S . -B build-glib-ebadf -G Ninja -DBUILD_GROUNDHOG=ON \
  -DBUILD_TESTING=ON -DBUILD_APPS=OFF -DNIP55L_SECRET_BACKEND=libsecret \
  -DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service
cmake --build build-glib-ebadf --target \
  test-groundhog-social-sync test-groundhog-relay-guard -j 6
```

The demonstration used `test-groundhog-social-sync` directly, not CTest or its
full fixture environment. It set `G_DEBUG=fatal-criticals`,
`GSETTINGS_BACKEND=memory` and
`GSETTINGS_SCHEMA_DIR=$PWD/build-glib-ebadf/gnome/groundhog/test-schemas`.
The executable's own private-bus fixture remained in use; no GUI mode or developer
keychain was used. The relay-guard binary was built but not executed.

Each measured batch ran four concurrent `/usr/bin/yes` processes with output to
`/dev/null`; the harness terminated and reaped them afterward. Variants were
interleaved for each attempt, with a 30-second process timeout, no failure retry,
no warning suppression and no change to test registration order. The isolated
command added `-p /privacy/social/server-list-explicit-public-post`; the full-suite
command supplied no selection arguments and ran the existing 16 cases.

| Sample, 60 processes per row | Passed | EBADF / SIGTRAP | Other failures / timeouts |
| --- | ---: | ---: | ---: |
| Isolated, Homebrew GLib 2.90.1 | 60 | 0 | 0 / 0 |
| Isolated, patched main, initial sample | 60 | 0 | 0 / 0 |
| Full suite, Homebrew GLib 2.90.1 | 58 | 2 | 0 / 0 |
| Full suite, patched main, initial sample | 60 | 0 | 0 / 0 |
| Isolated, unpatched main, corrected matched control | 60 | 0 | 0 / 0 |
| Isolated, patched main, matched sample | 60 | 0 | 0 / 0 |
| Full suite, unpatched main, corrected matched control | 56 | 4 | 0 / 0 |
| Full suite, patched main, matched sample | 60 | 0 | 0 / 0 |

The original 2.90.0 observation belongs to the related beads. The installed
Homebrew library at execution time was **2.90.1**, not 2.90.0. Homebrew failures
were attempts 6 and 58. Matched unpatched-main failures were attempts 1, 13, 14
and 18, all exit `-5` (SIGTRAP). A representative failure ended:

```text
ok 3 /privacy/social/server-list-explicit-public-post
# DEBUG: signer permission declaration not applied: Required signer service or method is unavailable
# GLib-WARNING: poll(2) failed due to: Bad file descriptor.
```

The isolated case did not reproduce the abort; its clean baseline must not be
presented as a measured improvement. The full suite exposes the window when a
subsequent case iterates the context. The matched-main sample changes only the
GLib implementation, avoiding the Homebrew-versus-main version confound. Zero
failures in these patched samples is not a guarantee of race freedom.

`DYLD_PRINT_LIBRARIES=1` verified actual loading, not merely an environment
assignment. The successful matched probes reported these distinct Mach-O UUIDs:

```text
unpatched: DB2CB604-5DB3-3B69-931D-6380675671B6
  /private/tmp/nostrc-glib-ebadf-20261110/_build/glib-unpatched/libglib-2.0.0.dylib
patched:   CE1D5E1B-8A39-38C0-B526-794F2757DB5A
  /private/tmp/nostrc-glib-ebadf-20261110/_build/glib/libglib-2.0.0.dylib
```

The baseline dylib was saved before applying the implementation change. Its
successful `DYLD_LIBRARY_PATH` was `$GLIB/_build/glib-unpatched`; the patched
path was `$GLIB/_build/glib`. Moving the baseline initially to an unrelated
directory broke its relative `@rpath` to GLib's `proxy-libintl-0.5` dependency.
Two premature control batches (60 isolated and 60 full-suite attempts), plus one
smoke attempt, failed in dyld with missing `_g_libintl_bind_textdomain_codeset`,
not inside the test. A first dependency-path correction omitted the `-0.5`
suffix. Those **120 batch attempts and one smoke failure are loader failures**,
not EBADF results or passing tests, and are excluded from the measured table.
Relocating the unchanged saved dylib under `_build/glib-unpatched` restored its
relative dependency path; both variants passed explicit load probes before the
corrected matched batches. The valid Homebrew and patched results from the
earlier batches remain reported above rather than being discarded.

## Prior art

Unauthenticated GitLab REST searches of both issues and merge requests, all
states, for `EBADF`, `Bad file descriptor` and `gpoll` found the exact open report
[#2644, “g_poll on macOS fails with EBADF instead of setting POLLNVAL since 2.72.1”](https://gitlab.gnome.org/GNOME/glib/-/issues/2644).
It describes the same regression, a libvirt fatal-warning test failure, and
proposes checking descriptors with `fcntl()` after `select()` fails. It links the
merged [!2571, “meson: Set BROKEN_POLL in macOS builds”](https://gitlab.gnome.org/GNOME/glib/-/merge_requests/2571).
This draft should therefore be offered against that existing issue, not as a
newly discovered upstream bug.

The searches did not identify an MR implementing this EBADF recovery. They also
returned [!5238, “gpoll: Correctly zero-out heap-allocated fd_sets on macOS”](https://gitlab.gnome.org/GNOME/glib/-/merge_requests/5238),
a related but different merged heap-fd-set correction, and
[#2785](https://gitlab.gnome.org/GNOME/glib/-/issues/2785), a closed `g_close()`
assertion report rather than this fallback behavior. Web-index queries did not
surface the exact report and the web tool could not open the filtered GitLab
lists; the successful project REST searches supplied the finding. Follow-up
notes requests returned 401 and individual MR-detail requests returned 406,
so no claim is made to have reviewed every discussion or every possible MR.

## Ready-to-paste merge-request description

### gpoll: Report invalid descriptors in the select fallback

On macOS, `BROKEN_POLL` selects the Unix `select()` fallback. A closed descriptor
currently makes `g_poll()` fail the whole call with `EBADF`, instead of reporting
`G_IO_NVAL` for that entry. A main context then emits a bad-descriptor warning,
which is fatal in tests. This reproduces GLib #2644 and was also observed in
nostrc when GIO socket closure precedes removal of a watching source.

After `select()` fails with `EBADF`, probe each nonnegative fd with `F_GETFD`,
clear other entries, mark invalid entries `G_IO_NVAL`, and return their count.
Count duplicate entries separately. Keep the existing error return if no invalid
fd remains, do not retry, and retain common cleanup for heap-backed fd sets.
Native `poll()` and Windows implementations are unchanged.

Add Unix regressions for stack/high-fd storage and delivery through an attached
source under fatal warnings. Against base
`107dde15d2961a7080f8d58ca27707d69df2d819`, all three new cases fail; with the
patch, native macOS `mainloop` (50 passed, 2 default timing skips), `poll-fdset`
(3 passed), and `unix` (22 passed, 1 pidfd-platform skip) succeed. The new
regressions and high-fd coverage do not skip. A matched native nostrc full-suite
sample under four CPU
hogs produced 4/60 EBADF aborts with unpatched main and 0/60 with patched main.
The isolated case passed 60/60 with both, so no improvement is inferred from it.
These are local results, not hosted CI or a claim of general fd-lifetime safety.

Closes: https://gitlab.gnome.org/GNOME/glib/-/issues/2644

## Delivery and version decision

This nostrc change contains only this write-up and the upstream format-patch.
Groundhog and every other nostrc component retain their manifest versions:
documentation-only work changes no shipped nostrc artifact. The upstream change
is a backward-compatible bug fix; the draft does not change GLib's release
version. No upstream issue, MR, release, push, merge or rebase was performed.
The scratch GLib checkouts and task-owned external build output are removed at
handoff; native build logs and per-attempt results are local ignored evidence in
`build-glib-ebadf/evidence/`, not additional committed deliverables.
