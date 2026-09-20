# Domain profile activation and rollback contract

No activation program is shipped at this checkpoint. The real GDM/AD laboratory
and D4 evidence are absent, so the sample manifest is deliberately
`activation_ready: false`.

A future package transaction must perform these steps atomically where the
platform permits:

1. Verify exact package pins, D4 evidence, the route and smb.conf samples, and
   every installed PAM-file SHA-256 with `validate_domain_profile.py`.
2. Prove the break-glass console account before changing NSS, Samba, or PAM.
3. Copy each reviewed original to the dedicated root-owned rollback directory,
   record mode/owner/hash, fsync the files and directory, and refuse symlinks.
4. Install only the reviewed distribution-specific profile. Do not overwrite a
   changed file and do not activate merely because examples were installed.
5. After activation, run the pinned route-isolation checks. A failed check
   immediately restores the exact originals and restarts/reloads only services
   named by the reviewed D4 procedure.

Rollback order is: restore the previous PAM/GDM files; restore the previous NSS
and winbind configuration; disable the new domain profile; verify break-glass
local login; then verify Nostr-local account visibility without enabling the old
cache-presence PAM implementation. Preserve UID/GID ownership and domain idmap
ranges. Never delete homes, authority data, Kerberos caches, or machine-trust
credentials as an automatic rollback side effect.
