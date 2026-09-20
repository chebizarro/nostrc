# Samba/AD desktop login candidate

**Status:** configuration contract only; no support or acceptance claim.

This path is conventional Samba winbind authentication. It does not translate a
Nostr signature into an AD password, certificate, Kerberos credential, or TGT,
and it does not enroll domain identities in the Nostr authority.

The inert samples are `config/domain-route.conf.sample` and
`config/smb.conf.winbind.sample`. Validate them with:

```sh
python3 packaging/domain/validate_domain_profile.py \
  --route config/domain-route.conf.sample \
  --smb config/smb.conf.winbind.sample \
  --manifest packaging/domain/domain-profile.manifest.json.sample
```

The reserved routes are mutually exclusive: `n_` is Nostr-only,
`DOMAIN\\user` is winbind-only, and unqualified names retain the distribution
local policy. `winbind use default domain = no`, non-overlapping idmap ranges,
`/home/%D/%U`, mode `0700`, and offline domain login disabled are mandatory.
Domain authentication failure cannot fall through to another route.

The samples are installed under the documentation data directory only when the
`domain_config` feature is explicitly enabled. They are never installed as
`/etc/samba/smb.conf`, `nsswitch.conf`, or a PAM policy. Activation requires a
future distribution-specific package after D4 proves join/trust, actual GDM PAM
dispatch, Kerberos-versus-samlogon behavior, credential-cache lifecycle, and
route isolation on pinned packages.

Required real-lab evidence remains: `net ads testjoin`, `wbinfo -t`, fully
qualified `getent`/`id`, GDM login, cache ownership and cleanup, DC-unavailable
behavior, same-short-name isolation, safe home creation, and preservation of
Nostr-local and break-glass login during winbind failure. None is available in
the current environment.
