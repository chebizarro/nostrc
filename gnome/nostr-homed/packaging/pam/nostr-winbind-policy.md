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
