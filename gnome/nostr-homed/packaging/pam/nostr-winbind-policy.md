# Candidate PAM route policy (not an activatable PAM file)

This document fixes the route graph for the future Ubuntu 24.04 package. It is
**not** copied into `/etc/pam.d`, because D4 has not proven the installed GDM and
pam_winbind control flow. Packaging must refuse activation until the domain
manifest passes `validate_domain_profile.py --require-activation-ready` against
the target root.

## Exclusive routes

1. Canonical names beginning `n_` enter only the Nostr broker route. An unknown,
   disabled, unavailable, or failed Nostr account is fatal within that route.
2. Names containing the configured single `\\` separator enter only the
   `pam_winbind` route. Short domain names are rejected. A failed domain
   authentication never falls through to Nostr or local Unix authentication.
3. All other names retain the distribution local Unix policy.

The domain branch must run pam_winbind authentication and account policy,
domain-only `pam_mkhomedir` with `/home/%D/%U` mode `0700`, pam_winbind
credential/session handling, distribution access/failure-accounting modules,
and pam_systemd. Offline login is disabled. No Nostr code enrolls an AD user,
changes idmap ownership, generates a Kerberos ticket, or handles a domain
password.

## Activation preconditions

The activation manifest must pin Samba, winbind, libpam-winbind, and
libpam-runtime versions; point to D4 evidence; and list the SHA-256 of every PAM
file to be changed. The validator compares those hashes with regular,
non-symlink files below the supplied target root. Any unknown or locally
modified layout is a hard refusal. A future installer must snapshot those exact
files before replacement and must never infer jump offsets or append a globally
`sufficient` module.

## NSS ordering (plan §4.3 C2, Finding 15)

The `nss_nostr` module (`gnome/nostr-homed/src/nss/nss_nostr.c`) admits only
names matching the reserved Nostr prefix — the bit-exact regex `^n_[a-z0-9_]+$`
enforced by `gnome/nostr-homed/src/identity/identity_common.c:8-20`. This
admission is invariant and MUST NOT be widened; domain-qualified names
(`DOMAIN\user`, `user@REALM`) never pass it.

The safety property that keeps qualified names off the Nostr module therefore
comes entirely from NSS ORDERING, not from any C-side gate. The pinned
ordering for a joined host is:

```
passwd: files winbind nostr
group:  files winbind nostr
shadow: files
```

`winbind` MUST precede `nostr` so a positive winbind answer for `DOMAIN\user`
is what the caller sees. `files` MUST stay first so local system accounts
(root, service users) never depend on a network module. `shadow` stays
`files` only because neither module owns local password hashes.

The canonical snippet ships as `config/nsswitch.conf.snippet.sample`. The
distribution package MUST NOT overwrite `/etc/nsswitch.conf`; the operator or
a post-install helper splices those three lines into the distribution-shipped
copy after `net ads join` has succeeded.

Do NOT re-introduce a `nostr_domain_qualified_names` (or similarly named) knob
in `/etc/nss_nostr.conf`. The knob was proposed in an earlier draft and
rejected — an admission bit inside the NSS module cannot make winbind win any
race it does not already win via ordering, but it CAN change enrollment
semantics for the shared `identity_common` validator. If a legacy config file
carrying that key survives an upgrade, `validate_domain_profile.py` reports it
as deprecated and does-nothing (plan §4.3 C2 tail).
