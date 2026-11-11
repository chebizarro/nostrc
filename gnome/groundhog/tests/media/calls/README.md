# Call-stack packaging check

This device-free harness uses the **same** `cmake/GroundhogCalls.cmake` dependency
and capability target as Groundhog. The original `test-groundhog-call-loopback`
is a packaging probe; `test-groundhog-call-media` exercises the concrete production
backend through `GhCallMediaBackend`. Neither implements Nostr signalling. `GROUNDHOG_ENABLE_CALLS` defaults to ON; missing WebRTC/SDP >= 1.20
or `nice` pkg-config modules disable calls without disabling voice notes or the
rest of Groundhog. The GLib-only probe still returns an actionable unavailable
error when compiled out.

Run inside the target Linux environment, not merely on the build host:

```sh
cmake -S gnome/groundhog/tests/media/calls -B build-calls -G Ninja
# Request the target explicitly: absent development dependencies must NOT make
# a packaging qualification appear to pass by running only the disabled stub.
cmake --build build-calls --target test-groundhog-call-capabilities test-groundhog-call-loopback
ctest --test-dir build-calls --output-on-failure -V
```

The capability test removes each required factory from the real process-local
GStreamer registry, checks the named unavailable error, and restores it. The
loopback sends Opus and VP8 in **both directions** between two `webrtcbin`s,
exchanges SDP and trickled ICE in process, and requires at least ten decoded raw
audio and video buffers at each peer. It uses host ICE, DTLS-SRTP, synthetic live
sources and fakesinks: no devices, relays, STUN/TURN services or credentials.
The timeout is a failure bound, never a success condition. CTest also bounds
negotiation/promise waits. Missing plugins are failures, not skips.

To verify a deliberately disabled build:

```sh
cmake -S gnome/groundhog/tests/media/calls -B build-calls-off -G Ninja -DGROUNDHOG_ENABLE_CALLS=OFF
cmake --build build-calls-off
ctest --test-dir build-calls-off --output-on-failure
```

## Flatpak runtime check

Platform 51 already supplies libnice and the baseline plugins; the manifest
intentionally does not bundle competing copies. Its post-install checks run in
**the SDK**, so also execute the Linux binaries in **the Platform** (same CPU
architecture, built against compatible GStreamer/GLib versions):

```sh
flatpak install --user flathub org.gnome.Platform//51
flatpak run --user --filesystem="$PWD/build-calls:ro" --share=network \
  --command="$PWD/build-calls/test-groundhog-call-capabilities" org.gnome.Platform//51
flatpak run --user --filesystem="$PWD/build-calls:ro" --share=network \
  --command="$PWD/build-calls/test-groundhog-call-loopback" org.gnome.Platform//51
```

On a macOS Docker host these commands run **inside Linux**, with private system
and session D-Bus instances. Nested Flatpak sandbox creation may require a
privileged disposable container. Do not substitute macOS executables or host
plugins. Successful Platform execution is still not a built Groundhog Flatpak
artifact test; run the same harness inside that artifact when qualifying it.

## Echo cancellation contract

Use GStreamer's paired `webrtcdsp` + `webrtcechoprobe` for application AEC. They
are **not** required for a headset-only call, because Ubuntu 24.04's bad-plugin
package does not provide them. `gh_call_capabilities_check(TRUE, error)` requires
both and fails closed for a speaker route without them; `FALSE` is only for a
headset route or independently verified system AEC. There is no silent claim
that PulseAudio/PipeWire automatically provides echo cancellation, and no new
AEC implementation or system configuration mutation in this work item.

The concrete backend places the echo probe on far-end playback and the DSP on
capture in the same top-level pipeline at matching 48 kHz mono sample rates,
creating the playback probe before capture activates. See the [upstream DSP contract](https://gstreamer.freedesktop.org/documentation/webrtcdsp/webrtcdsp.html).
Factory presence does not prove acoustic effectiveness; that requires actual
audio-device testing when the call pipeline lands.

## Concrete backend and settings

Link `groundhog-call-media` and construct `gh_call_media_new(settings)` on the
owning main context (`settings` uses `org.nostr.Groundhog`). Cast to
`GhCallMediaBackend` and call its interface vtable. The interface GType/signals
are implemented by `groundhog-call-media-backend`, available even with calls
compiled out. The concrete constructor is available only with
`GROUNDHOG_CALLS_AVAILABLE`; the service must use the packaging availability
guard. No service factory existed on the signalling branch when this backend
was written. Construction opens no capture or ICE sockets.

The concrete header exposes mute, camera enable, explicit device discovery,
a `GListModel` of `GstDevice` objects, device selection, and remote/local
`GdkPaintable` objects. Camera off stops capture, not just network transmission.
Video capture begins after video negotiation, not while ringing. Selected
microphone/camera removal stops that capture and emits `device-removed`; another
input can be selected without renegotiating. Output removal ends the call rather
than silently switching a headset onto speakers. Monitor events supplement, not
replace, fatal device errors from the pipeline. Physical/default-device behavior
still needs hardware qualification.

Speakers require both AEC plugins: **block rather than degrade** if absent.
An explicit headset output can opt out with `gh_call_media_set_headset(TRUE)`;
the system-default output cannot be marked safe because it may change routes.
Changing a headset output requires ending the call and re-evaluating AEC.
There is no unverified PipeWire/PulseAudio or system-AEC bypass.

Settings, owned by this backend (the Preferences page is separate work):

| Key | Type | Default |
|---|---|---|
| `call-stun-servers` | `as` | Cloudflare `stun://stun.cloudflare.com:3478`, Google `stun://stun.l.google.com:19302` |
| `call-turn-url` | `s` | empty |
| `call-turn-username` | `s` | empty |
| `call-relay-only` | `b` | false |

libnice supports one active STUN endpoint; the first configured entry is used.
The ordered list is retained for user choice, **not automatic STUN failover**.
TURN credentials use the asynchronous `gh_call_turn_password_*` APIs and the
platform Secret Service, keyed by exact URL and username. They are not stored
in GSettings, logs or call history. This implementation does not access the
macOS login keychain; a Secret Service is required on that development platform.
Do not enable GStreamer protocol debugging for owner calls: upstream debugging
can expose SDP or credential-bearing TURN URLs.

`configure(FALSE)` fails **before creating a pipeline**: libnice's relay policy
still gathers local base sockets, so filtering signalled host candidates alone
cannot implement the stronger no-host-gathering consent requirement. A call
therefore requires explicit local-network consent, even when relay-only is set.
With consent, relay-only configures the native ICE relay policy and admits only
relay candidates in both SDP and trickle ICE. Missing TURN configuration or
credentials is an error, never a direct fallback. Tor blocking and policy-change
notifications belong to the service. Reconfiguration of an active/pending backend
tears it down and requires a new instance. Teardown waits for libnice's TURN
allocation close completion on a private context before releasing its agent;
it does not re-enter the UI context or leave refresh sources alive.

Video is offered VP8 first, then H.264 constrained-baseline `42e01f`/`42e032`.
Negotiation precedes attaching the selected video encoder, otherwise webrtcbin
intersects the offer with an already-running VP8 encoder and loses the fallback.
OpenH264 provides both H.264 encoding and decoding. The independent Ubuntu
`avdec_h264` leak is tracked as `nostrc-mazyq`; no LSan suppression is used.

## Concrete transport checks

```sh
cmake -S gnome/groundhog/tests/media/calls -B build-calls-asan \
  -DCMAKE_C_FLAGS='-fsanitize=address -fno-omit-frame-pointer -g'
cmake --build build-calls-asan --target test-groundhog-call-media
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-calls-asan \
  -R '^groundhog-call-media' --output-on-failure
```

The in-process test covers actual Opus/VP8 and constrained-baseline H.264,
voice-to-video upgrade, pending teardown, candidate rejection and fail-closed
configuration, texture delivery, and injected device removal/replacement using
the production handler. The two-process test uses pipes only for SDP/ICE;
media crosses real ICE/DTLS-SRTP sockets. Deadlines are failure bounds; no sleep
is used as evidence of connectivity. Device fixtures do not establish physical
microphone/camera or acoustic-AEC performance.

For forced TURN plus real credential storage, run **inside a disposable Linux
container**, with coturn, gnome-keyring and dbus installed (never on the owner's
session bus). Both subprocesses assert every emitted candidate is relay type:

```sh
GH_CALL_TEST_PRIVATE_BUS=1 ASAN_OPTIONS=detect_leaks=1 GSETTINGS_BACKEND=memory \
GSETTINGS_SCHEMA_DIR="$PWD/build-calls-asan/call-schemas" \
dbus-run-session -- sh gnome/groundhog/tests/media/calls/turn_session.sh \
  "$PWD/build-calls-asan/test-groundhog-call-media"
```

The fixture waits for bus ownership and coturn's listener-ready event, removes
its private keyring and TURN state, and has no third-party TURN credentials.
The local coturn loopback test is not an Internet NAT/firewall interoperability
test. Passing any of these tests does not qualify NIP-AC signalling, external
clients, physical devices, GTK window presentation or packaged artifacts.
AppImage qualification belongs to `nostrc-h3kh`.
