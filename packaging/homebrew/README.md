# Homebrew Formulas for Gnostr

This directory contains Homebrew formulas for installing Gnostr on macOS.

## Available Formulas

| Formula | Description |
|---------|-------------|
| `gnostr.rb` | GTK4 Nostr client for the GNOME desktop |
| `gnostr-signer.rb` | NIP-46 Remote Signer with GTK4 UI and background daemon |

## Installation

### From Local Formula (Development)

```bash
# Install from local formula file
brew install --build-from-source ./gnostr.rb
brew install --build-from-source ./gnostr-signer.rb
```

### From HEAD (Latest Development)

```bash
# Install latest from git master
brew install --HEAD ./gnostr.rb
```

### From Tap (When Available)

Once published to a Homebrew tap:

```bash
# Add the tap
brew tap gnostr/gnostr

# Install
brew install gnostr
brew install gnostr-signer
```

## Dependencies

The formulas automatically install required dependencies:

- **Build tools**: cmake, ninja, pkg-config
- **GTK4 stack**: gtk4, libadwaita, glib, json-glib
- **Crypto**: libsecp256k1, libsodium, openssl@3
- **Network**: libsoup
- **Sync**: nsync

### Optional Dependencies

- `gstreamer` - For video playback (gnostr)
- `p11-kit` - For PKCS#11 HSM support (gnostr-signer)

## Running the Signer Daemon

To start the gnostr-signer-daemon automatically at login:

```bash
# Copy launchd plist
cp /opt/homebrew/opt/gnostr-signer/LaunchAgents/org.gnostr.Signer.daemon.plist ~/Library/LaunchAgents/

# Load the daemon
launchctl load ~/Library/LaunchAgents/org.gnostr.Signer.daemon.plist

# Check status
launchctl list | grep gnostr
```

## Creating a Tap

To create a Homebrew tap for distribution:

1. Create a new GitHub repo named `homebrew-gnostr`
2. Add the formula files to the root of the repo
3. Users can then install with:
   ```bash
   brew tap gnostr/gnostr
   brew install gnostr
   ```

## Updating the Formula

When releasing a new version:

1. Update the `url` to point to the new release tarball
2. Update the `sha256` with the actual checksum:
   ```bash
   curl -sL https://github.com/chebizarro/nostrc/archive/refs/tags/vX.Y.Z.tar.gz | shasum -a 256
   ```
3. Commit and push to the tap repo

## Troubleshooting

### GSettings schemas not found

After installation, if you get GSettings errors:

```bash
glib-compile-schemas /opt/homebrew/share/glib-2.0/schemas
```

### OpenSSL not found

Ensure OpenSSL is linked:

```bash
brew link --force openssl@3
```

### pkg-config path issues

If dependencies aren't found, check PKG_CONFIG_PATH:

```bash
export PKG_CONFIG_PATH="/opt/homebrew/opt/openssl@3/lib/pkgconfig:/opt/homebrew/opt/libsoup@3/lib/pkgconfig:$PKG_CONFIG_PATH"
```
