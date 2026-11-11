# gst-libav 1.24.1: `avdec_h264` leaks one AVPacket per frame; Groundhog calls never use it

11 November 2026. Local investigation and Ubuntu SRU report draft for `nostrc-mazyq`;
nothing submitted to GStreamer, Debian or Launchpad. No LSan suppression added.

## Summary

- **Location.** `gst_ffmpegviddec_handle_frame()` in gst-libav **1.24.1**
  (`ext/libav/gstavviddec.c`) allocates an `AVPacket` with `av_packet_alloc()` at
  line 2242 for every input frame, then releases it with `av_packet_unref()` at
  lines 2280 and 2283. `av_packet_unref()` frees the packet's payload, not the
  packet, so 104 bytes (`sizeof (AVPacket)` in FFmpeg 6.1 on LP64) are lost per
  frame. libavcodec, libavutil and libgstvideo are not at fault.
- **Upstream: fixed.** [bc8a3ca, “avviddec: Fix AVPacket leak”](https://github.com/GStreamer/gstreamer/commit/bc8a3ca526dd91ef31035f5dbe278800e5eab9e5)
  ([!6544](https://gitlab.freedesktop.org/gstreamer/gstreamer/-/merge_requests/6544))
  on main, backported as [ce4f0d57](https://github.com/GStreamer/gstreamer/commit/ce4f0d574607076e86b5a0392b1fbbbd3a6aac86)
  ([!6545](https://gitlab.freedesktop.org/gstreamer/gstreamer/-/merge_requests/6545)),
  both merged in April 2024. It is released in **1.24.2** and present in 1.28.6 (GNOME
  Platform 51) and 1.28.9 (Arch). Both call sites use `av_packet_free (&packet)` at
  the [1.24.2](https://github.com/GStreamer/gstreamer/blob/1.24.2/subprojects/gst-libav/ext/libav/gstavviddec.c#L2280-L2283),
  [1.28.6](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gst-libav/ext/libav/gstavviddec.c#L2469-L2472)
  and [1.28.9](https://github.com/GStreamer/gstreamer/blob/1.28.9/subprojects/gst-libav/ext/libav/gstavviddec.c#L2472-L2475)
  tags, and `av_packet_unref (packet)` at [1.24.1](https://github.com/GStreamer/gstreamer/blob/1.24.1/subprojects/gst-libav/ext/libav/gstavviddec.c#L2280-L2283).
- **Ubuntu 24.04 (noble): unfixed and unreported.** Noble mixes GStreamer core,
  base and bad 1.24.2 with `gstreamer1.0-libav` **1.24.1-1build1**. Launchpad's
  publishing history for `gst-libav1.0` in noble has only that version in the
  release pocket, with no `-updates` or `-proposed` upload since. The source
  package's bug list (23 bugs, all states) has no report of this leak. The
  draft below asks for the two-line backport as a stable release update (SRU).
- **Groundhog.** The call receive path names `openh264dec` explicitly and has
  done so since its first commit (`3cb5c9276`). It never auto-plugs, so no
  runtime selects `avdec_h264` for a call. This change keeps that, asserts it in
  tests, and stops a missing OpenH264 from disabling voice and VP8 calls.

## Why the stack first pointed at libgstvideo

The CI image's default ASan run reports `av_malloc` called from
`libgstvideo-1.0.so.0+0x453f8/0x47804/0x47f84`. Ubuntu builds `libgstlibav.so` and
libavcodec without frame pointers, and ASan's default fast unwinder skips them.
With `fast_unwind_on_malloc=0` the stack becomes `av_malloc` ←
`av_packet_alloc` (libavcodec.so.60) ← `libgstlibav.so+0x203b0`. A debug build of
the 1.24.1 source resolves that frame to `gst_ffmpegviddec_handle_frame
gstavviddec.c:2242`. The leaked size equals `sizeof (AVPacket)`, and the count
equals the number of decoded frames.

## Verification

Everything ran in local Docker on an arm64 macOS host (Docker Desktop). The
image `gh-fix-h264-ci:local` was built unchanged from `scripts/linux-ci.Dockerfile`,
including its ddebs step. The versions in it were:
`gstreamer1.0-libav 1.24.1-1build1`, `libgstreamer1.0-0 1.24.2-1ubuntu0.1`,
`gstreamer1.0-plugins-bad 1.24.2-1ubuntu4`, `libavcodec60`/`libavutil58
7:6.1.1-3ubuntu5` and `libopenh264-7 2.4.1+dfsg-1`. `gst-inspect-1.0` reports
`avdec_h264` at rank primary (256) and `openh264dec` at marginal (64).

The standalone [reproducer](gst-libav-avpacket-leak-2026-11-11-repro.c) contains
no Groundhog code. It waits for EOS, sets the pipeline to NULL, releases the bus
and pipeline, calls `gst_deinit()`, and prints EOS and the frame count reaching
`fakesink name=out`:

```sh
cc -g -fsanitize=address -fno-omit-frame-pointer \
  docs/investigations/gst-libav-avpacket-leak-2026-11-11-repro.c \
  $(pkg-config --cflags --libs gstreamer-1.0) -o repro
ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0 LSAN_OPTIONS= ./repro \
  'videotestsrc num-buffers=300 ! video/x-raw,width=640,height=480 ! openh264enc ! h264parse ! DECODER ! fakesink name=out'
```

| Run (300 frames, EOS reached in every row) | Exit | LSan |
| --- | ---: | --- |
| Stock `avdec_h264` | 1 | 31,200 bytes / 300 allocations |
| Stock `decodebin` (auto-plugs `avdec_h264`) | 1 | 31,200 bytes / 300 allocations |
| Stock `openh264dec` | 0 | none |
| gst-libav 1.24.1 tarball built from source, unpatched | 1 | 31,200 bytes / 300, `gstavviddec.c:2242` |
| Same tarball plus only `ce4f0d57` | 0 | none |

For the source A/B, the official
[gst-libav-1.24.1.tar.xz](https://gstreamer.freedesktop.org/src/gst-libav/gst-libav-1.24.1.tar.xz)
passed its published `.sha256sum`. It was unpacked twice, and
[`ce4f0d57.patch`](https://github.com/GStreamer/gstreamer/commit/ce4f0d574607076e86b5a0392b1fbbbd3a6aac86.patch)
was applied to one copy with `patch -p3`. Each copy was built with
`meson setup --buildtype=debug -Ddoc=disabled -Dtests=disabled`
against Ubuntu's own `libavcodec-dev 7:6.1.1-3ubuntu5`, and loaded with
`GST_PLUGIN_PATH=<build>/ext/libav` and a separate `GST_REGISTRY`.
`gst-inspect-1.0 avdec_h264` showed each build's `libgstlibav.so` as the loaded
file. FFmpeg and the rest of GStreamer were the stock packages.

Groundhog's call backend was checked with the recipe in
`gnome/groundhog/tests/media/calls/README.md` (ASan/LSan build,
`GROUNDHOG_CALL_TEST_REQUIRE_RUNTIME=1 ASAN_OPTIONS=detect_leaks=1 ctest -R
'^groundhog-call-media'`):

| Run | Exit | Result |
| --- | ---: | --- |
| CI image, this change | 0 | 2/2 tests; 10/10 subtests; `/call-media/h264-fallback` decoded 301 and 300 frames, `openh264dec` on both peers; no LSan report |
| CI image, scratch copy with only the decoder swapped to `avdec_h264` (source and expected name) | 8 | 10/10 subtests pass, then LSan: 63,752 bytes / 613 allocations (613 × 104) |
| Platform 51 image, base `a3a88229a` | 8 | aborts at `/call-media/learned-private` (voice): “Calls unavailable: install the GStreamer runtime plugin providing openh264enc” |
| Platform 51 image, this change, runtime optional | 0 | 2/2 tests; VP8 loopbacks pass; `/call-media/h264-fallback` skips: “H.264 unavailable without openh264enc” |
| Platform 51 image, this change, `GROUNDHOG_CALL_TEST_REQUIRE_RUNTIME=1` | 8 | `/call-media/h264-fallback` fails: “REQUIRED RUNTIME MISSING: H.264 calls need openh264enc” |

The negative control shows that the 300-frame test detects this leak under LSan
and is not just passing trivially. Its stack matches the original nostrc
observation. Platform 51 runs used
`quay.io/gnome_infrastructure/gnome-build-meta:sdk-51`, digest
`sha256:dc388c8467d1ba3f10a6fe68bc84f998a8e93d551351a6b0e1768f121a6b803b`
(GStreamer 1.28.6, `/etc/os-release` `org.gnome.Platform` 51). The image ran under
Docker's amd64 emulation, not in a Flatpak sandbox. OpenH264 emits an encoder
`cmInitParaError` line at drain on Ubuntu, but every run above still reached EOS
with the stated frame counts. Hosted amd64 CI, native Flatpak, native Arch and
real cameras were not run.

## Groundhog decoder selection

`GhCallMedia` handles `webrtcbin::pad-added` itself. `webrtcbin` delivers RTP,
and `create_receiver()` builds a fixed `rtph264depay ! h264parse ! openh264dec`
(or `rtpvp8depay ! vp8dec`) bin. There is no `decodebin`, so ranks do not matter.
The test raises `avdec_h264` above primary rank and still asserts `openh264dec`.
Before this change, `build_pipeline()` required all OpenH264 and H.264 RTP elements
for every call. It now treats them as an H.264 capability: without them, offers
carry only VP8 and an H.264-only peer gets `G_IO_ERROR_NOT_SUPPORTED` with
“H.264 unavailable”. No other decoder is substituted.

| Runtime | H.264 decoders registered | Groundhog before (`a3a88229a`) | Groundhog after |
| --- | --- | --- | --- |
| Ubuntu 24.04 CI image (`scripts/linux-ci.Dockerfile`) | `avdec_h264` (1.24.1, leaks; `decodebin` picks it) and `openh264dec` | `openh264dec`, not asserted | `openh264dec`, asserted for 300 frames per peer under LSan |
| Flatpak `org.gnome.Platform//51` (SDK OCI image) | none: no `libgstopenh264.so`, and the Platform FFmpeg has no H.264 decoder | no calls at all, voice included (measured) | VP8 video and voice; H.264-only peers get “H.264 unavailable” (measured) |
| Arch (package metadata only) | `openh264dec` from `gst-plugins-bad` 1.28.9, which Groundhog's package depends on; `avdec_h264` only if optional `gst-libav` 1.28.9 is installed (fixed) | `openh264dec` | `openh264dec` |

Groundhog's Flatpak manifest neither bundles OpenH264 nor requests a codec
extension. The OCI image has an empty `codecs-extra` extension point. An extension
that provides FFmpeg H.264 could register `avdec_h264` 1.28.6, which already has
the fix, but Groundhog still would not select it. No extension was installed or
tested. The Arch row comes from the
[gstreamer packaging recipe](https://gitlab.archlinux.org/archlinux/packaging/packages/gstreamer/-/blob/main/PKGBUILD)
(1.28.9-2 on main) and archlinux.org's
[gst-plugins-bad](https://archlinux.org/packages/extra/x86_64/gst-plugins-bad/)
and [gst-libav](https://archlinux.org/packages/extra/x86_64/gst-libav/) entries
(1.28.9-1 in `extra`, 10 November 2026), plus
`packaging/archlinux/groundhog/PKGBUILD`. No Arch system was measured.

## Prior art

- GStreamer: fixed by the two commits above, from Seungha Yang. The commit message
  says only that `av_packet_unref()` does not free the packet; no issue is linked.
- Ubuntu: later series carry the fix; for example, oracular moved to 1.24.8
  ([LP: #2082087](https://bugs.launchpad.net/ubuntu/+source/gst-libav1.0/+bug/2082087)).
  For noble, Launchpad's `getPublishedSources` returned `1.24.1-1build1` (Release,
  published 3 April 2024) as the only live version. `searchTasks` over every bug
  status returned 23 `gst-libav1.0` bugs, none about this leak.
- No other upstream report is needed; this is an already-merged upstream fix
  that one distribution release lacks.

## Ready-to-paste Launchpad bug report

**Project:** `gst-libav1.0 (Ubuntu)`; nominate for **Noble**.

**Title:** [SRU] avviddec leaks one AVPacket per decoded video frame (fixed upstream in 1.24.2)

```text
[ Impact ]

gstreamer1.0-libav 1.24.1-1build1 in noble leaks an AVPacket (104 bytes on
64-bit) for every compressed frame decoded by any avdec_* video decoder,
including avdec_h264, which decodebin/playbin auto-plug at primary rank.
gst_ffmpegviddec_handle_frame() allocates the packet with av_packet_alloc()
but releases it with av_packet_unref(), which frees only the payload.
Memory grows for as long as video plays (about 11 MB per hour at 30 fps), and
AddressSanitizer/LeakSanitizer builds of applications that decode video fail
their leak checks. The rest of GStreamer in noble is 1.24.2, so the leak looks
like a 1.24.2 bug.

Upstream fixed this in 1.24.2:
  main:  https://github.com/GStreamer/gstreamer/commit/bc8a3ca526dd91ef31035f5dbe278800e5eab9e5
         https://gitlab.freedesktop.org/gstreamer/gstreamer/-/merge_requests/6544
  1.24:  https://github.com/GStreamer/gstreamer/commit/ce4f0d574607076e86b5a0392b1fbbbd3a6aac86
         https://gitlab.freedesktop.org/gstreamer/gstreamer/-/merge_requests/6545
Both replace the two av_packet_unref (packet) calls in ext/libav/gstavviddec.c
with av_packet_free (&packet). The 1.24 commit applies to the 1.24.1 tarball
with patch -p3.

[ Test Plan ]

1. On noble, install gstreamer1.0-libav gstreamer1.0-plugins-bad
   libgstreamer1.0-dev gcc pkg-config.
2. Build the attached reproducer (a gst_parse_launch() pipeline that waits
   for EOS, sets NULL, unrefs and calls gst_deinit()):
     cc -g -fsanitize=address -fno-omit-frame-pointer repro.c \
       $(pkg-config --cflags --libs gstreamer-1.0) -o repro
3. Run it:
     ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0 ./repro \
       'videotestsrc num-buffers=300 ! video/x-raw,width=640,height=480 ! openh264enc ! h264parse ! avdec_h264 ! fakesink name=out'
   With 1.24.1-1build1: "eos=yes frames=300", exit 1, and
   "Direct leak of 31200 byte(s) in 300 object(s)" from av_packet_alloc.
   With the fixed package: "eos=yes frames=300", exit 0, no leak report.
4. Repeat with decodebin in place of avdec_h264 (same result), and play a
   local H.264 file with gst-play-1.0 (gstreamer1.0-plugins-base-apps) as a
   smoke test.

The patched result was confirmed by building the upstream 1.24.1 tarball with only
ce4f0d57 applied, against noble's libavcodec60 7:6.1.1-3ubuntu5: 300
frames, exit 0, no leak; the unpatched build of the same tarball leaked
31200 bytes in 300 allocations at gstavviddec.c:2242.

[ Where problems could occur ]

av_packet_free() calls av_packet_unref() and then frees the struct and nulls
the pointer, so payload handling is unchanged. The packet is not used after
either call site: the error path jumps to send_packet_failed, and the success
path only receives frames from the codec context. A regression would show
up as a crash or use-after-free in avdec_* video decoding (playback,
thumbnailing, transcoding). Audio decoders (gstavauddec.c) use a stack
AVPacket, are not affected, and are untouched.
Upstream has shipped this change since 1.24.2 (April 2024), and it is in every
later Ubuntu series.

[ Other Info ]

Found while running LeakSanitizer CI for an application on noble. The
standalone reproducer above contains no application code.
```

Attach [`gst-libav-avpacket-leak-2026-11-11-repro.c`](gst-libav-avpacket-leak-2026-11-11-repro.c)
as `repro.c`. An uploader would add the upstream commit as a `debian/patches`
entry with DEP-3 headers. No debdiff was built or test-installed for this draft.

## Delivery and version decision

Groundhog: PATCH-class correction, folded into unreleased 0.13.0, with no version
bump and no public API, storage or wire-format change. This document and the
reproducer are documentation only. Nothing was submitted to GStreamer, Debian or
Launchpad. `tests/lsan.supp` is unchanged, and no suppression or leak-detection
bypass was used.
