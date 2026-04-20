---
title: Avatars render at a different size on nearly every surface, Discover avatars are oval, and profile images often don't load
issue_type: bug
priority: 2
area: chrome, avatars
labels: [cosmetic, avatars, sizing, shape, image-loading, supersedes-my-earlier-claim]
tester: claude-uat
session: 2026-04-19 (second pass)
build: build/apps/gnostr/gnostr (rebuilt during session)
---

## Summary
Three related but distinct avatar problems compound each other:

1. **Almost every surface renders avatars at a different pixel size.**
2. **Discover profile avatars are oval**, not circular — noticeably
   taller than they are wide.
3. **Profile images fail to load on Discover (and probably elsewhere)**
   even for accounts that clearly have kind-0 profile pictures.
   Discover rows show initial-letter placeholders (A, A, M, C, …)
   for everyone but the logged-in user.

Earlier in this UAT pass I claimed the avatars looked consistent
after spot-checking. That was wrong — a closer zoom confirms the
problems above. This bead supersedes that earlier claim.

## Steps to reproduce
1. Launch gnostr, signed in (custom profile picture set).
2. Walk the surfaces below and compare the same user's / a
   placeholder's avatar at each.

## Observed sizes (approximate pixels, same build, same window)
| Surface                                   | Approx size | Shape                |
|-------------------------------------------|-------------|----------------------|
| Title-bar avatar (top-right of chrome)    | ~30 × 30    | Circular             |
| Avatar popover — "Signed in" big tile     | ~50 × 50    | Circular             |
| Avatar popover — "Floof Farm" dropdown row| ~24 × 24    | Circular (smallest)  |
| Notification row avatar                   | ~22 × 22    | Circular             |
| Timeline note-card avatar                 | ~38 × 38    | Circular             |
| Discover profile-list avatar              | ~40 × 44    | **Oval, taller than wide** |
| Profile view (hero, half over banner)     | ~100 × 90   | Possibly oval; certainly not the same proportion as elsewhere |
| Git Repos card avatar (prior pass)        | ~40 × 40    | Circular             |

Eight or nine distinct sizes for the same conceptual element.

## Image-loading inconsistency
- The signed-in user's avatar (Floof Farm) renders as the custom
  image everywhere I can verify (title bar, popover big tile,
  popover dropdown row, profile hero).
- On Discover, every row shows an initial-letter placeholder even
  for accounts whose NIP-05 address implies they're fully-formed
  profiles (AirportStatusBot (DFW) / (DEN) / (ATL), MintPress News,
  Colonial Outcasts, The Canary — these all very likely publish
  kind-0 events with a `picture` URL).
- Timeline notes from hex-pubkey authors (0f92c4a4…, d36e8083…,
  fea186c2…) also show initials — but those are genuinely
  avatar-less anon bots, so it's not obvious from timeline alone
  whether the renderer is broken; Discover is the better evidence.

## Expected
- **One avatar component**, parameterised only by a few canonical
  sizes (e.g. xs=20, sm=28, md=40, lg=56, xl=96 — whatever the
  design-system rung is) that every call site opts into. Ban one-off
  widths.
- **Shape is always a perfect circle.** Oval rendering usually means
  the container's width ≠ height and the CSS/GTK equivalent isn't
  enforcing aspect ratio + clip-to-border-radius properly. Lock
  `width == height` on every avatar container and `border-radius:
  50%` (or the Adwaita-equivalent circular mask).
- **Profile images render consistently** — if a `picture` URL is
  present on kind-0, fetch, cache, and show it. Fall back to
  deterministic colored tile with initials only when no picture is
  set or the fetch has permanently failed. Surface loading state
  (skeleton → final), not a silent placeholder.

## Actual
- Each surface picks its own size.
- Discover rows are oval.
- Most Discover avatars don't even attempt to render the profile
  picture.

## Notes
- If there's a shared `Avatar` widget, consolidate onto it and audit
  callers. If there isn't, that's the first refactor.
- The profile hero probably sits inside a banner-crossing container
  that's stretching the avatar box; confirm the container isn't
  doing the stretching and the avatar itself isn't fighting it.
- Worth adding a visual regression test that renders the avatar
  component at every size with and without a loaded image.
