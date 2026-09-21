# `pam_nostr` provider-choice UX (bucket B5, nostrc-zcll.6)

`pam_nostr.so` is the broker-backed PAM module. It stays a thin conversation
layer — every real decision is delegated to the privileged broker over
`/run/nostr-auth/auth.sock`.

## Flow

1. **BEGIN_LOGIN** on a dedicated broker connection. The broker looks the
   account up and returns `{"result":"ok","providers":[...]}` where the
   array is the account's enabled providers by canonical name
   (`"local"` for the encrypted vault, `"nip46"` for the external signer).
2. **Provider selection**:
   - `provider=local|nip46|remote` module arg → pinned non-interactively.
     Fails PAM_AUTH_ERR if the pinned provider is not in the account's list.
   - Exactly one provider enabled → selected silently.
   - More than one → prompted via `PAM_PROMPT_ECHO_ON` with
     `"Sign in with (local/remote): "`. Only trimmed lowercase ASCII
     `"local"` or `"remote"`/`"nip46"` is accepted; anything else is rejected
     (informed via `pam_error`) and consumes one of the invocation's
     invalid-attempt slots.
3. **Unlock**:
   - `local` → `PAM_PROMPT_ECHO_OFF` for the passphrase, forwarded as the
     `SUBMIT_UNLOCK` secret. On `INVALID_PROOF`/`DENIED` the user is told and
     re-prompted (each failure costs one attempt).
   - `nip46` → `pam_info` shows an approval hint (no passphrase) and the
     module sends the SUBMIT_UNLOCK approval placeholder. If the bunker
     denies, the module returns without silently looping.
4. **Retry budget**: `NH_AUTH_PAM_MAX_INVALID_ATTEMPTS = 3` invalid attempts
   total, across the choice and passphrase steps. Exhausting the budget
   returns `PAM_MAXTRIES`.

## Exercising with `pamtester`

The interactive conversation is not covered by the C tests — those exercise
the broker/client layer directly. To drive the actual PAM conversation on a
VM where `pam_nostr.so` is installed:

```sh
# Point PAM at the module for a throwaway service. Example /etc/pam.d/nostr-test:
#   auth required pam_nostr.so socket=/run/nostr-auth/auth.sock
# For a pinned-provider run, add e.g.:
#   auth required pam_nostr.so socket=/run/nostr-auth/auth.sock provider=local

# Two providers enabled → interactive choice; wrong tokens cost attempts.
pamtester nostr-test alice authenticate

# Pin the local vault non-interactively.
sudo tee /etc/pam.d/nostr-test-local >/dev/null <<'PAM'
auth required pam_nostr.so socket=/run/nostr-auth/auth.sock provider=local
PAM
pamtester nostr-test-local alice authenticate

# Pin the external NIP-46 signer — no passphrase; approve at the bunker.
sudo tee /etc/pam.d/nostr-test-nip46 >/dev/null <<'PAM'
auth required pam_nostr.so socket=/run/nostr-auth/auth.sock provider=nip46
PAM
pamtester nostr-test-nip46 alice authenticate
```

Expected outcomes:
- Correct passphrase / bunker approval → `pamtester` reports `authenticate:
  Success`.
- Three wrong passphrases in a row → `pamtester` reports `authenticate:
  Have exhausted maximum number of retries for service.` (`PAM_MAXTRIES`).
- Unknown account / disabled → `PAM_USER_UNKNOWN` / `PAM_ACCT_EXPIRED`.
- Broker socket unreachable → `PAM_AUTHINFO_UNAVAIL`.

For interactive-choice testing, `pamtester` reads from the calling terminal;
typing anything other than `local` / `remote` / `nip46` after trimming
whitespace consumes an attempt and re-prompts until the budget is spent.
