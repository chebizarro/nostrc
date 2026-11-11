# Call-stack packaging check

This device-free harness uses the **same** `cmake/GroundhogCalls.cmake` dependency
and capability target as Groundhog. It is not call signalling or the production
call pipeline. `GROUNDHOG_ENABLE_CALLS` defaults to ON; missing WebRTC/SDP >= 1.20
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

Later call code must place the echo probe on far-end playback and the DSP on
capture in the same top-level pipeline at matching sample rates, creating the
probe before the DSP sink activates. See the [upstream DSP contract](https://gstreamer.freedesktop.org/documentation/webrtcdsp/webrtcdsp.html).
Factory presence does not prove acoustic effectiveness; that requires actual
audio-device testing when the call pipeline lands.

## Evidence boundaries

Passing this harness proves the Opus/VP8 media transport in the environment in
which it ran. It does not prove microphone/camera access, H.264 constrained-
baseline negotiation, TURN, NIP-AC interoperability, network/privacy policy, UI,
or a separately packaged artifact. The capability probe opens no devices or
sockets. A future call service must consume its result and separately enforce
Groundhog's network policy. AppImage qualification belongs to `nostrc-h3kh`.
