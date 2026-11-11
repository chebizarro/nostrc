# Groundhog call interop runbook (NIP-AC, nostrc-mqrgk.6)

The cross-client matrix for Groundhog's NIP-AC calls has to be run by a person
with the devices: Amethyst and Nospeak on Android, Nospeak as a PWA in a
browser, and a packaged Groundhog build. No public cross-client NIP-AC call
record exists, so this run is the evidence. This runbook says what to set up,
what to do in each cell, what to capture on Groundhog's side, what counts as a
pass, and how to record the result. Bead `nostrc-mqrgk.6` stays open until the
table below is filled in from a real run; every failing cell gets its own bug
bead.

Design background: `docs/investigations/groundhog-voice-transport-rtp-quic-vs-nostr-native-2026-11-07.md`
("Decision update"). Signalling and routing: `docs/groundhog-call-signalling.md`.
Media, ICE policy and the TURN/relay-only contract:
`gnome/groundhog/tests/media/calls/README.md`.

## What is pinned

| Item | Value | Where it comes from |
|---|---|---|
| NIP-AC revision Groundhog implements | nostr-protocol/nips PR #2301, branch `vitorpamplona/nips:NIP-ac/AC.md`, commit `31c7a41` (2026-04-05) | investigation report, "Decision update" |
| Kinds | 25050 offer, 25051 answer, 25052 ICE candidate, 25053 hangup, 25054 reject (`busy` for an automatic reject), 25055 renegotiate; each signed inner event wrapped in a kind 21059 ephemeral wrap, no seal | `nips/nostr-kinds.h`, `gh-call-wire.c` |
| Amethyst wire vector | quartz `ce558d90e70d432157cc0672762b2d4119d7f904` | `nips/nip59/tests/nipac_wire_vector.h` |
| Nospeak wire-parity fixture | nospeak `518b7711d397d8d62bcbe9774c34300b4e33d9d4` | `gnome/groundhog/tests/fixtures/nipac-nospeak.json` |
| Amethyst | v1.09.0 or newer (Android) | first release with 25050–25055 |
| Nospeak | v1.1.x (web/PWA and Android) | first releases with 25050–25055 |
| Noscall | v0.5.2 (best effort: no 25055, publishes to its own relays) | shipped-implementations table in the report |

Record the exact app versions you installed and the PR #2301 commit you
compared against at run time; if the PR moved, note the new head and whether
the wire format changed.

## Groundhog behaviour the matrix relies on

- **Receiving.** Groundhog subscribes to kind 21059 addressed to the active
  account on the account's own **kind 10050 inbox relays** only, with a 60 s
  freshness window on the inner `created_at`. It does not watch NIP-65 read
  relays. A peer that publishes elsewhere (Noscall) is only heard if one of
  its relays is one of Groundhog's inbox relays.
- **Sending.** Groundhog publishes to the **peer's 10050 inbox relays**
  (resolved like a DM); the peer's NIP-65 read relays are used only when the
  peer has no 10050 list at all. Answers and rejects are also self-wrapped to
  Groundhog's own inbox relays so other devices of the account stop ringing.
- **Who can ring you.** A call from someone whose conversation you have not
  accepted does not ring unless Preferences › Calls › "Ring for Message
  Requests" is on; it is kept as a missed-call request. Hidden authors never
  ring. For the matrix, accept the peer's conversation first (or turn the
  switch on) so the setting is not what you are testing.
- **Busy.** A second offer while a call is ringing or active is rejected
  automatically with a kind 25054 whose content is `busy`.
- **Reject / hangup.** Declining a ringing call sends 25054; ending a
  connecting or active call sends 25053. Groundhog ends a call it has not
  answered after 60 s of ringing.
- **Voice → video upgrade.** Groundhog sends a 25055 with a new SDP offer and
  expects a 25051 answer; it accepts a peer's 25055 the same way. Noscall has
  no 25055: do not run the upgrade cells against it.
- **Network mode.** In Tor mode calls are unavailable (Preferences › Calls
  says so): WebRTC's UDP, STUN and TURN sockets would bypass Tor. Run the
  matrix in the System Settings or No Proxy mode.
- **Local network.** With Preferences › Network › "Allow local network" off
  (the default) calls work, but Groundhog never puts host (local-network or
  mDNS) candidates on the wire, blanks the related address of reflexive
  candidates, and rejects non-public candidates from the peer. Leave it off:
  that is the shipped behaviour. If a direct call fails only on one LAN,
  retest once with it on and record both.
- **STUN.** Only the first entry of Preferences › Calls › STUN Servers is
  used. The defaults are Cloudflare then Google.
- **TURN and relay-only.** Preferences › Calls › TURN Server takes a
  credential-free `turn://` or `turns://` address and a username; the
  credential is saved in the platform secret store (on macOS this is not
  implemented yet, `nostrc-zrvr8`; the page says so). "Relay Every Call"
  stays insensitive until a server, username and saved credential exist.
  With it on, Groundhog emits and accepts only `typ relay` candidates and
  refuses to fall back to a direct path, so the forced-TURN cells are driven
  from Groundhog's side regardless of what the peer offers. Groundhog ships
  no TURN server and no third-party credential: use your own coturn or a
  server you have an account on.
- **Codecs.** Opus for audio; video is offered VP8 first, then H.264
  constrained baseline (`42e01f`/`42e032`). A peer that strips VP8 still
  negotiates H.264.
- **Echo cancellation.** A speaker route needs GStreamer's `webrtcdsp` and
  `webrtcechoprobe`; without them Groundhog refuses the call rather than
  degrade. Use a headset if your build lacks them, and record which.

## Capturing SDP and NIP-AC events on Groundhog's side

Groundhog has no call logging by default: `gh-call-media.c` deliberately
drops GStreamer error text (it can carry SDP and credential-bearing TURN
URLs) and nothing is written to the message store. Do **not** use
`GST_DEBUG` for owner calls (see the media README). The one capture hook is
an environment variable read when the call service starts:

```sh
GROUNDHOG_CALL_TRACE="$HOME/gh-call-trace-$(date +%Y%m%d-%H%M).jsonl" groundhog
# Flatpak:
flatpak run --env=GROUNDHOG_CALL_TRACE="$HOME/gh-call-trace.jsonl" org.nostr.Groundhog
```

Unset (the default) nothing is written and nothing else changes. Set, every
validated inner call event is appended as one JSON line, **in both
directions**:

```json
{"ts":1762800000123456,"dir":"out","event":{"kind":25050,"pubkey":"…","created_at":1762800000,"tags":[["p","…"],["call-id","…"],["call-type","voice"],["alt","…"]],"content":"v=0\r\n…","id":"…","sig":"…"}}
```

- `dir` is `out` for an event Groundhog signed (recorded before wrapping and
  publishing: it is an attempted send, not a relay acknowledgement) and `in`
  for a wrap Groundhog decrypted and validated (a correct signature, one
  recipient and call-id, fresh enough). Events that fail validation are not
  recorded; if a peer's event never appears, check the relay overlap first.
- `event.content` is the raw SDP for 25050, 25051 and 25055; the JSON
  `{"candidate":…,"sdpMid":…,"sdpMLineIndex":…}` for 25052; empty or a
  reason (`busy`) for 25053 and 25054.
- The peer's SDP and candidates are in the `in` lines, so one Groundhog
  trace covers the signalling of both sides of a call. What it cannot show
  is the peer's local view (what the peer's app rejected or never sent).
- The `out` SDP is the wire copy: with local network off it has no host
  candidates and blanked related addresses, which is what the peer receives.

Useful extracts (`jq`):

```sh
T=$HOME/gh-call-trace.jsonl
# the events of one call, in order
jq -c 'select(.event.tags[][1]=="<call-id>") | [.ts,.dir,.event.kind,.event.content[0:60]]' "$T"
# the SDP Groundhog offered / answered with, and the peer's
jq -r 'select(.dir=="out" and (.event.kind==25050 or .event.kind==25051 or .event.kind==25055)) | .event.content' "$T"
jq -r 'select(.dir=="in"  and (.event.kind==25050 or .event.kind==25051 or .event.kind==25055)) | .event.content' "$T"
# candidate types Groundhog put on the wire (expect no "typ host"; only "typ relay" with Relay Every Call)
jq -r 'select(.dir=="out" and .event.kind==25052) | .event.content | fromjson | .candidate' "$T" | grep -o 'typ [a-z]*' | sort | uniq -c
# how the call ended
jq -c 'select(.event.kind==25053 or .event.kind==25054) | [.dir,.event.kind,.event.content]' "$T"
```

The trace holds decrypted SDP with IP addresses and the peers' public keys.
Keep it with the run, redact addresses before pasting any of it into a bead or
issue, and delete it afterwards. It is for the owner's own runs; never ship a
build with the variable set.

On the peer's side, record what the app shows (ringing, connected, ended and
why) and its version. For Nospeak in a browser, `chrome://webrtc-internals`
(Chromium) or `about:webrtc` (Firefox) shows the browser's local and remote
SDP and the selected candidate pair, which is the only way to see the peer's
view of a forced-TURN cell. Android apps give nothing comparable without a
debug build; their behaviour is inferred from Groundhog's `in` lines and the
screen.

## Setup

1. **Two accounts.** The Groundhog account and the peer app's account, each
   with a published kind 10050 inbox relay list (Groundhog: the onboarding
   relay step / Preferences › Network › Where People Reach You). At least one
   relay must be reachable by both. Both accounts must accept each other's
   conversation (send one DM each way) or Groundhog's "Ring for Message
   Requests" must be on.
2. **Groundhog build.** A packaged build (Flatpak or AppImage) of the commit
   under test, run with `GROUNDHOG_CALL_TRACE` set. Note the commit, the
   package type and the GStreamer version inside it. Network mode: System
   Settings or No Proxy. Allow local network: off.
3. **Media.** A headset or a build with the AEC plugins; a camera for the
   video cells. Note what you used.
4. **TURN.** Your own coturn (or an account on a TURN service) with
   long-term credentials. Enter it in Preferences › Calls, apply the server
   and username, then the credential; "Saved Credential" must read "Saved for
   <username> at <server>" and "Relay Every Call" must become sensitive.
   Keep it off for the direct cells and on for the forced-TURN cells.
5. **Noscall only.** Make Noscall's account publish to (or at least include)
   one of Groundhog's inbox relays, since Noscall writes to its own relays.

## Procedure per cell

For every cell: start a fresh trace file name or note the timestamp before the
cell, run the cell, then confirm in the trace that the expected events exist
in the expected direction and that the call reached the expected state on both
screens. "Connected" means audio heard in both directions (and video seen in
both, for the video cells) for at least 30 s.

| Cell | Do | Expected in the trace | Pass when |
|---|---|---|---|
| Voice, Groundhog → peer, direct | Place a voice call from the conversation header; peer answers | `out` 25050 (`call-type` voice), `in` 25051, 25052 both ways with no `typ host` in `out` | connected 30 s; hang up from Groundhog sends `out` 25053 and the peer shows ended |
| Voice, peer → Groundhog, direct | Peer calls; answer in Groundhog | `in` 25050, `out` 25051, 25052 both ways | connected; hang up from the peer gives `in` 25053 and Groundhog shows ended |
| Video, both directions, direct | As above with video | `call-type` video; the SDP has an audio and a video m-section; VP8 or H.264 agreed | audio and video both ways |
| Voice/video, both directions, forced TURN | Relay Every Call on in Groundhog; repeat the four cells above | every `out` 25052 is `typ relay`; the SDP `out` has no host or srflx candidates | connected through your TURN server (the server's log shows the allocation) |
| Reject, peer rejects | Call the peer; peer declines | `out` 25050, `in` 25054 | Groundhog shows declined, no media started |
| Reject, Groundhog rejects | Peer calls; decline in Groundhog | `in` 25050, `out` 25054 (and a self-copy of it to your inbox relays) | peer shows declined |
| Busy, peer busy | Put the peer in another call (or ringing), then call it | `in` 25054 with content `busy` | Groundhog shows busy |
| Busy, Groundhog busy | While a Groundhog call is ringing or active, have the peer call | `out` 25054 with content `busy`; the first call continues | peer shows busy; first call unaffected |
| Hangup, each side, voice and video | During a connected call end it from one side; repeat from the other | `out` or `in` 25053 | both screens end within a few seconds; no further 25052 after it |
| Voice → video upgrade, Groundhog initiates | In a connected voice call, enable video in Groundhog | `out` 25055 with a video m-section, `in` 25051 | video both ways (or one way if the peer keeps its camera off: record which) |
| Voice → video upgrade, peer initiates | Peer enables video | `in` 25055, `out` 25051 | as above |
| No answer | Call the peer, nobody answers | `out` 25050, after 60 s `out` 25053 | Groundhog shows no answer; peer stops ringing |
| Noscall voice, best effort | Voice both directions and hangup, after the relay overlap is set up | as the voice cells | connected; note any H.264 rewrite in `in` SDP; skip upgrade and video |

Run each peer-direction pair twice: once with Groundhog on a different network
from the peer (NAT between them) and once on the same Wi-Fi. The direct cells
are only meaningful across a NAT; with local network off, same-Wi-Fi direct
calls go through the STUN-reflexive path or fail, and that result is worth
recording too.

## Results table template

Copy this into the investigation report (or a dated results file next to it)
and fill it from the run. One row per cell, per peer app, per network layout.

```
Run date:                    2026-__-__
Groundhog commit:            ____________ (git describe), package: Flatpak / AppImage, GStreamer __.__
NIP-AC revision compared:    PR #2301 commit 31c7a41 (branch head at run time: ____________)
Groundhog network mode:      system / none; allow-local-network: off / on
STUN in use:                 stun://…      TURN: turn(s)://…  (relay-only cells only)
Audio route:                 headset / speakers with webrtcdsp+webrtcechoprobe
Trace files:                 gh-call-trace-….jsonl (kept privately, addresses redacted in any excerpt)

| # | Peer app + version + platform | Network layout | Direction | Media | Path | Scenario | Events seen (out / in) | Connected? | Ended by / reason shown | Result | Bug bead |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | Amethyst v1.09.__ Android __ | NAT between | G → A | voice | direct | connect + hangup from G | 25050/25051, 25052×n/×m, 25053 out | yes, 30 s | G hangup | pass | |
| 2 | Amethyst … | NAT between | A → G | voice | direct | connect + hangup from A | | | | | |
| 3 | Amethyst … | NAT between | G → A | video | direct | | | | | | |
| 4 | Amethyst … | NAT between | A → G | video | direct | | | | | | |
| 5 | Amethyst … | NAT between | G → A | voice | relay-only | | | | | | |
| 6 | Amethyst … | NAT between | A → G | voice | relay-only | | | | | | |
| 7 | Amethyst … | NAT between | G → A | video | relay-only | | | | | | |
| 8 | Amethyst … | NAT between | A → G | video | relay-only | | | | | | |
| 9 | Amethyst … | NAT between | G → A | voice | direct | peer rejects | | | | | |
| 10 | Amethyst … | NAT between | A → G | voice | direct | Groundhog rejects | | | | | |
| 11 | Amethyst … | NAT between | G → A | voice | direct | peer busy | | | | | |
| 12 | Amethyst … | NAT between | A → G | voice | direct | Groundhog busy | | | | | |
| 13 | Amethyst … | NAT between | G → A | voice | direct | upgrade to video from G (25055) | | | | | |
| 14 | Amethyst … | NAT between | A → G | voice | direct | upgrade to video from A (25055) | | | | | |
| 15 | Amethyst … | NAT between | G → A | voice | direct | no answer, 60 s | | | | | |
| … | Nospeak v1.1.__ PWA (browser __) | … | rows 1–15 again | | | | | | | | |
| … | Nospeak v1.1.__ Android __ | … | rows 1–15 again | | | | | | | | |
| … | Noscall v0.5.2 Android | … | voice connect/hangup both ways only | | | | | | | best effort | |
| … | (each peer) | same Wi-Fi | rows 1–4 again | | | | | | | | |
```

Result values: `pass`, `fail` (with a bug bead), `blocked` (say by what, e.g.
no TURN server), `n/a` (Noscall upgrade). A cell passes only in a packaged
build; a developer-tree run is a rehearsal, not the record.

## Filing a failing cell

```sh
bd create --title="Calls interop: <peer app> <direction> <media> <path> <scenario>" \
  --type=bug --priority=1 \
  --description="Run <date>, Groundhog <commit> (<package>). Expected <events>; saw <events>. Peer showed <state>. Trace excerpt (addresses redacted): …"
bd dep add <new-id> nostrc-mqrgk.6   # the gate depends on the fix
```

Attach the redacted trace lines of that call-id, the peer's version and what
its screen showed. If the peer's event never reached Groundhog, say which
relays each side publishes to and reads from; that is a routing bug or a relay
overlap mistake, not a media bug, and the two are filed differently.

## Closing the gate

`nostrc-mqrgk.6` is closed by the owner when every cell against Amethyst and
Nospeak (both platforms) is `pass` in a packaged build and the filled table is
in the report, with the client versions and the PR #2301 commit recorded. Any
captured event that differs from the pinned fixtures (`nipac-nospeak.json`,
`nipac_wire_vector.h`) goes to `nostrc-mqrgk.3` as a new regression fixture
for `gh-call-service`, with the SDP body replaced by a short placeholder and
the keys by test keys.
