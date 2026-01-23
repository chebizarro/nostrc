# GNostr Signer - Windows Installer

This directory contains scripts and configuration files for building a Windows
installer for GNostr Signer.

## Overview

The Windows build uses:
- **MSYS2/MinGW-w64**: For compiling the GTK4 application
- **NSIS**: For creating the Windows installer
- **GTK4 + libadwaita**: Bundled with the installer

## Build Options

### Option 1: Native Windows Build (MSYS2)

This is the recommended approach for most users.

#### Prerequisites

1. **Install MSYS2** from https://www.msys2.org/

2. **Open MSYS2 MinGW64 shell** (not the MSYS shell)

3. **Install dependencies**:
   ```bash
   ./build-msys2.sh --install-deps
   ```

#### Building

```bash
# Full build with installer
./build-msys2.sh --release --package

# Or step by step:
./build-msys2.sh --release
./bundle-deps.sh --build-dir ../../build-windows
makensis -DVERSION=1.0.0 -DARCH=x64 gnostr-signer.nsi
```

### Option 2: Cross-Compilation (Linux/macOS)

Cross-compiling GTK4 applications for Windows is complex due to the many
dependencies. We recommend using native Windows builds or CI/CD pipelines.

For CI/CD, see the GitHub Actions workflow in `.github/workflows/`.

### Option 3: CMake Integration

If you have MSYS2 set up in your PATH:

```powershell
# Configure with installer support
cmake -B build -G "MinGW Makefiles" `
    -DCMAKE_BUILD_TYPE=Release `
    -DBUILD_WINDOWS_INSTALLER=ON

# Build
cmake --build build

# Create installer
cmake --build build --target windows-installer
```

## Package Contents

The installer includes:

| Component | Description |
|-----------|-------------|
| gnostr-signer.exe | Main GTK4 application |
| gnostr-signer-daemon.exe | Background IPC daemon |
| GTK4 Runtime | Graphics toolkit (~100MB) |
| libadwaita | GNOME design patterns |
| Icons | Adwaita icon theme |
| GSettings | Application preferences |

## Dependencies

### Required MSYS2 Packages

```bash
# Build tools
pacman -S base-devel mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja

# GTK4 stack
pacman -S mingw-w64-x86_64-gtk4 mingw-w64-x86_64-libadwaita mingw-w64-x86_64-json-glib

# Crypto
pacman -S mingw-w64-x86_64-libsodium mingw-w64-x86_64-libsecp256k1

# Optional
pacman -S mingw-w64-x86_64-libsecret mingw-w64-x86_64-p11-kit mingw-w64-x86_64-nsync

# Installer
pacman -S mingw-w64-x86_64-nsis
```

## File Structure

```
windows/
  gnostr-signer.nsi    # NSIS installer script
  build-msys2.sh        # MSYS2 build script
  bundle-deps.sh        # Bash dependency bundler
  bundle-deps.ps1       # PowerShell dependency bundler
  README.md             # This file
```

## Windows-Specific Features

### Inter-Process Communication

On Windows, the daemon uses **Named Pipes** instead of D-Bus for IPC:
- Pipe name: `\\.\pipe\gnostr-signer`
- Protocol: Same JSON-RPC as Unix domain sockets

### Credential Storage

Secrets are stored using **Windows Credential Manager** (DPAPI):
- Encrypted with user's Windows login credentials
- Accessible only to the current user
- Survives reboots and password changes

### Autostart

The installer registers a scheduled task for the daemon:
- Runs at user logon
- Limited privileges (no admin required)
- Can be disabled via Task Scheduler

### URL Protocol Handlers

The installer registers handlers for:
- `nostr://` - Nostr protocol URIs
- `bunker://` - NIP-46 bunker connection strings

## Customizing the Installer

### Branding

Edit `gnostr-signer.nsi` to customize:
- Product name and publisher
- Icons (`MUI_ICON`, `MUI_UNICON`)
- Welcome/finish page bitmaps
- License file

### Components

Modify the sections in the NSIS script to:
- Add optional features
- Change default installation options
- Include additional files

## Troubleshooting

### DLL Not Found Errors

If the application fails to start with missing DLL errors:

1. Run `bundle-deps.sh` or `bundle-deps.ps1` to verify all DLLs are collected
2. Check that the `bin` directory is in PATH or the DLLs are in the app directory
3. Use [Dependencies](https://github.com/lucasg/Dependencies) to analyze missing DLLs

### GSettings Schema Errors

If you see GSettings schema errors:

```powershell
# Compile schemas manually
glib-compile-schemas.exe "C:\Program Files\GNostr Signer\share\glib-2.0\schemas"
```

### GTK Theme Issues

The application uses the Adwaita theme. If it looks wrong:

1. Ensure icon themes are installed in `share\icons`
2. Set `GTK_THEME=Adwaita` environment variable
3. Check that `share\gtk-4.0\settings.ini` exists

### Named Pipes Connection Failed

If other applications cannot connect to the daemon:

1. Verify the daemon is running: `tasklist | findstr gnostr-signer-daemon`
2. Check Named Pipe exists: `[System.IO.Directory]::GetFiles("\\.\pipe\") | Select-String gnostr`
3. Restart the daemon

## Building Installer on CI/CD

### GitHub Actions Example

```yaml
jobs:
  windows:
    runs-on: windows-latest
    defaults:
      run:
        shell: msys2 {0}
    steps:
      - uses: actions/checkout@v4
      - uses: msys2/setup-msys2@v2
        with:
          msystem: MINGW64
          install: >-
            mingw-w64-x86_64-gtk4
            mingw-w64-x86_64-libadwaita
            mingw-w64-x86_64-cmake
            mingw-w64-x86_64-ninja
            mingw-w64-x86_64-nsis
      - name: Build
        run: |
          cd apps/gnostr-signer/packaging/windows
          ./build-msys2.sh --release --package
      - uses: actions/upload-artifact@v4
        with:
          name: windows-installer
          path: apps/gnostr-signer/packaging/windows/*.exe
```

## License

GNostr Signer is distributed under the MIT License. See LICENSE file for details.

GTK4 and libadwaita are distributed under the LGPL-2.1+ license. The Windows
installer bundles these libraries in accordance with their license terms.
