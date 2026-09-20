# VM testing for nostr-homed (Linux parity + acceptance)

Tracks beads: `nostrc-rb0e.1` [D0] (full lab), `nostrc-rb0e.14` [D-arch]
(arch decision), `nostrc-rb0e.15` [D-ci] (this dev loop).

## Scope and honest limits

There are two very different things a VM can do here:

1. **Linux build/test parity (available now).** Prove the portable core
   (`auth_core` + `identity_core` + `domain_config`) builds and passes its 11
   portable tests on real Linux. Driven by `scripts/vm-linux-ci.sh`.
2. **Installed login acceptance (NOT available yet).** GDM/PAM/NSS/Samba login
   cannot be tested because the runtime does not install:
   `NOSTR_HOMED_ENABLE_AUTH_INSTALL` is a configure-time `FATAL_ERROR` until the
   broker+PAM (`nostrc-zcll.5` [B4]) and NSS install ABI (`nostrc-nxpb.6` [A5])
   land. The acceptance harness (`run_acceptance.py`) is complete and
   fail-closed, but its per-suite **adapters** are unwritten and every matrix
   role is `unavailable`.

**Do #1 today. #2 is gated on B4/A3 shipping an installable runtime.**

## Architecture note (important)

`tests/acceptance/matrix.json` pins `architecture: amd64`, but Apple Silicon
Macs are arm64. UTM runs **arm64** Ubuntu 24.04 at native speed (Apple
Virtualization); **amd64** requires QEMU TCG emulation and is impractically slow
for GDM/GNOME. Use arm64 for the dev loop; satisfy the amd64 release pin in
cloud CI or on x86 hardware. Record the decision in `nostrc-rb0e.14`.

## One-time VM prep (UTM, Ubuntu 24.04 arm64)

1. Create the VM in UTM (Virtualize, not Emulate) with the arm64 Ubuntu Server
   or Desktop 24.04 ISO. Give it >=4 vCPU / 4 GB for comfortable builds.
2. Enable SSH and key-based login (BatchMode auth is required by the script):
   ```sh
   sudo apt-get install -y openssh-server
   # on the Mac: ssh-copy-id ubuntu@<vm-ip>
   ```
   Find the VM IP with `ip -4 addr` inside the VM (UTM shared/bridged network).
3. Passwordless sudo (disposable VM convenience) so apt installs run
   unattended, or pass `SUDO=` and pre-install deps and run with `SKIP_DEPS=1`.
4. **Snapshot the clean VM** so every run starts known-good. UTM ships
   `utmctl`:
   ```sh
   utmctl list
   utmctl start "Ubuntu 24.04"
   # snapshots via the UTM UI, or `utmctl clone` for a disposable copy
   ```

## Run the loop

```sh
# from the repo root on the Mac:
make -f Makefile.vm vm-test VM_SSH=ubuntu@192.168.64.5
# or with sanitizers / meson too:
make -f Makefile.vm vm-test-asan VM_SSH=ubuntu@192.168.64.5
make -f Makefile.vm vm-test-full VM_SSH=ubuntu@192.168.64.5
```

The script SSHes in, installs deps, syncs the branch, builds libgo → libnostr
(nostrdb OFF) → nostr-homed against a local prefix, runs the portable ctest
suite, stages the inert domain-config install and runs the domain validator,
then pulls all logs back to `vm-ci-logs/<timestamp>/`. Exit non-zero on any
failure. See `scripts/vm-linux-ci.sh` for all env vars.

## When B4/A3 land: acceptance adapters

`run_acceptance.py` spawns one executable per suite, named by
`NOSTR_ACCEPTANCE_{GDM,SMB,WINBIND}_ADAPTER`, invoked with
`--matrix --evidence-dir --results`. An adapter must:

- restore a **disposable** VM snapshot (opt-in literal
  `NOSTR_ACCEPTANCE_LAB_OPT_IN=I_UNDERSTAND_THIS_RUNS_AGAINST_A_DISPOSABLE_LAB`),
- drive the scenario over SSH,
- write evidence files under `--evidence-dir` (non-empty, no symlinks, must not
  contain the injected secret canary),
- emit `results` JSON: `{schema_version:1, suite, results:[{id,status,evidence}]}`
  (`status` in pass/fail/skip/unavailable; passing cases require evidence).
- exit 0 (results consumed), 77 (skip), or 69 (unavailable).

One VM is not enough for a full suite: `gdm` also needs a controlled relay,
controlled + real external signer, and fault injection; `smb` needs a Windows 11
client and a GNOME Files client; `winbind` needs a separate Samba AD DC. Fill
the `roles` pins in `matrix.json` (image_sha256, package versions) as each is
provisioned.
