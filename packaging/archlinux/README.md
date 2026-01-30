# Arch Linux Packaging (AUR)

This directory contains PKGBUILD for submitting to the Arch User Repository (AUR).

## Packages

The PKGBUILD creates two packages:

| Package | Description |
|---------|-------------|
| `gnostr` | GTK4 Nostr client for the GNOME desktop |
| `gnostr-signer` | NIP-46 Remote Signer with GTK4 UI and systemd daemon |

## Installation from AUR (when published)

Using an AUR helper like `yay` or `paru`:

```bash
# Install both packages
yay -S gnostr gnostr-signer

# Or install individually
yay -S gnostr
yay -S gnostr-signer
```

Or manually:

```bash
git clone https://aur.archlinux.org/gnostr.git
cd gnostr
makepkg -si
```

## Local Build (Testing)

To test the PKGBUILD locally:

```bash
# Build packages
makepkg -s

# Install built packages
sudo pacman -U gnostr-*.pkg.tar.zst gnostr-signer-*.pkg.tar.zst
```

## Updating the PKGBUILD

For new releases:

1. Update `pkgver` to the new version
2. Update `sha256sums` with the actual checksum:
   ```bash
   curl -sL https://github.com/chebizarro/nostrc/archive/refs/tags/vX.Y.Z.tar.gz | sha256sum
   ```
3. Bump `pkgrel` to 1 (or increment if same version with package changes)

## Publishing to AUR

1. Create an AUR account at https://aur.archlinux.org
2. Add your SSH key to your AUR account
3. Clone the AUR package:
   ```bash
   git clone ssh://aur@aur.archlinux.org/gnostr.git
   ```
4. Copy PKGBUILD and generate .SRCINFO:
   ```bash
   cp /path/to/PKGBUILD .
   makepkg --printsrcinfo > .SRCINFO
   ```
5. Commit and push:
   ```bash
   git add PKGBUILD .SRCINFO
   git commit -m "Update to version X.Y.Z"
   git push
   ```

## Using the Daemon

After installing gnostr-signer, enable the daemon:

```bash
# Enable and start the daemon
systemctl --user enable --now gnostr-signer-daemon

# Check status
systemctl --user status gnostr-signer-daemon

# View logs
journalctl --user -u gnostr-signer-daemon -f
```

## Dependencies

### Runtime (gnostr)
- gtk4, libadwaita, glib2, json-glib
- jansson, libsecp256k1, libsodium
- libsoup3, openssl, nsync

### Runtime (gnostr-signer)
- Same as gnostr plus libsecret
- Optional: p11-kit for HSM support

### Optional
- gstreamer + plugins for video playback
