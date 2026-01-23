#!/bin/bash
# install.sh - Install GNostr Signer native messaging host for Chrome/Firefox
#
# Usage:
#   ./install.sh [OPTIONS]
#
# Options:
#   --chrome-only     Install only for Chrome/Chromium
#   --firefox-only    Install only for Firefox
#   --extension-id ID Set Chrome extension ID (required for Chrome)
#   --uninstall       Remove the native messaging host
#   --prefix DIR      Install prefix (default: /usr/local)
#   --user            Install to user directories only
#   --help            Show this help message
#
# The script will:
# 1. Install the native host binary to PREFIX/bin
# 2. Install manifest files to appropriate browser locations
# 3. Update manifest paths to match installation

set -e

# Configuration
HOST_NAME="org.gnostr.signer"
BINARY_NAME="gnostr-signer-native"
PREFIX="/usr/local"
CHROME_EXTENSION_ID=""
FIREFOX_EXTENSION_ID="nip07@gnostr.org"
INSTALL_CHROME=true
INSTALL_FIREFOX=true
UNINSTALL=false
USER_INSTALL=false

# Detect OS
detect_os() {
  case "$(uname -s)" in
    Linux*)   OS="linux";;
    Darwin*)  OS="macos";;
    MINGW*|MSYS*|CYGWIN*) OS="windows";;
    *)        OS="unknown";;
  esac
}

# Get Chrome manifest directory
chrome_manifest_dir() {
  if [ "$USER_INSTALL" = true ]; then
    case "$OS" in
      linux)
        echo "$HOME/.config/google-chrome/NativeMessagingHosts"
        ;;
      macos)
        echo "$HOME/Library/Application Support/Google/Chrome/NativeMessagingHosts"
        ;;
      windows)
        echo "$(cygpath -u "$LOCALAPPDATA")/Google/Chrome/User Data/NativeMessagingHosts"
        ;;
    esac
  else
    case "$OS" in
      linux)
        echo "/etc/opt/chrome/native-messaging-hosts"
        ;;
      macos)
        echo "/Library/Google/Chrome/NativeMessagingHosts"
        ;;
      windows)
        # System-wide on Windows requires registry key
        echo ""
        ;;
    esac
  fi
}

# Get Chromium manifest directory
chromium_manifest_dir() {
  if [ "$USER_INSTALL" = true ]; then
    case "$OS" in
      linux)
        echo "$HOME/.config/chromium/NativeMessagingHosts"
        ;;
      macos)
        echo "$HOME/Library/Application Support/Chromium/NativeMessagingHosts"
        ;;
      *)
        echo ""
        ;;
    esac
  else
    case "$OS" in
      linux)
        echo "/etc/chromium/native-messaging-hosts"
        ;;
      macos)
        echo "/Library/Application Support/Chromium/NativeMessagingHosts"
        ;;
      *)
        echo ""
        ;;
    esac
  fi
}

# Get Firefox manifest directory
firefox_manifest_dir() {
  if [ "$USER_INSTALL" = true ]; then
    case "$OS" in
      linux)
        echo "$HOME/.mozilla/native-messaging-hosts"
        ;;
      macos)
        echo "$HOME/Library/Application Support/Mozilla/NativeMessagingHosts"
        ;;
      windows)
        echo "$(cygpath -u "$APPDATA")/Mozilla/NativeMessagingHosts"
        ;;
    esac
  else
    case "$OS" in
      linux)
        echo "/usr/lib/mozilla/native-messaging-hosts"
        ;;
      macos)
        echo "/Library/Application Support/Mozilla/NativeMessagingHosts"
        ;;
      windows)
        echo ""
        ;;
    esac
  fi
}

# Print usage
usage() {
  echo "Usage: $0 [OPTIONS]"
  echo ""
  echo "Install GNostr Signer native messaging host for Chrome/Firefox"
  echo ""
  echo "Options:"
  echo "  --chrome-only         Install only for Chrome/Chromium"
  echo "  --firefox-only        Install only for Firefox"
  echo "  --extension-id ID     Set Chrome extension ID"
  echo "  --uninstall           Remove the native messaging host"
  echo "  --prefix DIR          Install prefix (default: /usr/local)"
  echo "  --user                Install to user directories only"
  echo "  --help                Show this help message"
  echo ""
  echo "Example:"
  echo "  $0 --extension-id abcdefghijklmnop --user"
  echo ""
}

# Parse arguments
while [ $# -gt 0 ]; do
  case "$1" in
    --chrome-only)
      INSTALL_FIREFOX=false
      shift
      ;;
    --firefox-only)
      INSTALL_CHROME=false
      shift
      ;;
    --extension-id)
      CHROME_EXTENSION_ID="$2"
      shift 2
      ;;
    --uninstall)
      UNINSTALL=true
      shift
      ;;
    --prefix)
      PREFIX="$2"
      shift 2
      ;;
    --user)
      USER_INSTALL=true
      shift
      ;;
    --help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      usage
      exit 1
      ;;
  esac
done

# Detect OS
detect_os
echo "Detected OS: $OS"

# Check for required extension ID for Chrome
if [ "$INSTALL_CHROME" = true ] && [ "$UNINSTALL" = false ] && [ -z "$CHROME_EXTENSION_ID" ]; then
  echo "Warning: No Chrome extension ID specified."
  echo "Use --extension-id to specify your extension's ID."
  echo "The manifest will need to be updated manually."
  CHROME_EXTENSION_ID="EXTENSION_ID_PLACEHOLDER"
fi

# Determine binary path
if [ "$USER_INSTALL" = true ]; then
  BINARY_PATH="$HOME/.local/bin/$BINARY_NAME"
else
  BINARY_PATH="$PREFIX/bin/$BINARY_NAME"
fi

# Get script directory (where manifest templates are)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Uninstall
if [ "$UNINSTALL" = true ]; then
  echo "Uninstalling native messaging host..."

  # Remove binary
  if [ -f "$BINARY_PATH" ]; then
    rm -f "$BINARY_PATH"
    echo "Removed: $BINARY_PATH"
  fi

  # Remove Chrome manifests
  if [ "$INSTALL_CHROME" = true ]; then
    CHROME_DIR=$(chrome_manifest_dir)
    if [ -n "$CHROME_DIR" ] && [ -f "$CHROME_DIR/$HOST_NAME.json" ]; then
      rm -f "$CHROME_DIR/$HOST_NAME.json"
      echo "Removed: $CHROME_DIR/$HOST_NAME.json"
    fi

    CHROMIUM_DIR=$(chromium_manifest_dir)
    if [ -n "$CHROMIUM_DIR" ] && [ -f "$CHROMIUM_DIR/$HOST_NAME.json" ]; then
      rm -f "$CHROMIUM_DIR/$HOST_NAME.json"
      echo "Removed: $CHROMIUM_DIR/$HOST_NAME.json"
    fi
  fi

  # Remove Firefox manifest
  if [ "$INSTALL_FIREFOX" = true ]; then
    FIREFOX_DIR=$(firefox_manifest_dir)
    if [ -n "$FIREFOX_DIR" ] && [ -f "$FIREFOX_DIR/$HOST_NAME.json" ]; then
      rm -f "$FIREFOX_DIR/$HOST_NAME.json"
      echo "Removed: $FIREFOX_DIR/$HOST_NAME.json"
    fi
  fi

  echo "Uninstall complete."
  exit 0
fi

# Install
echo "Installing native messaging host..."

# Check if binary exists
BUILD_DIR="${SCRIPT_DIR}/../../../build/apps/gnostr-signer"
if [ -f "$BUILD_DIR/$BINARY_NAME" ]; then
  SOURCE_BINARY="$BUILD_DIR/$BINARY_NAME"
elif [ -f "$SCRIPT_DIR/$BINARY_NAME" ]; then
  SOURCE_BINARY="$SCRIPT_DIR/$BINARY_NAME"
else
  echo "Error: Binary '$BINARY_NAME' not found."
  echo "Please build the project first, or place the binary in this directory."
  exit 1
fi

# Install binary
echo "Installing binary to $BINARY_PATH..."
mkdir -p "$(dirname "$BINARY_PATH")"
cp "$SOURCE_BINARY" "$BINARY_PATH"
chmod 755 "$BINARY_PATH"
echo "Installed: $BINARY_PATH"

# Install Chrome manifest
if [ "$INSTALL_CHROME" = true ]; then
  CHROME_DIR=$(chrome_manifest_dir)
  if [ -n "$CHROME_DIR" ]; then
    echo "Installing Chrome manifest to $CHROME_DIR..."
    mkdir -p "$CHROME_DIR"

    # Generate manifest with correct path and extension ID
    cat > "$CHROME_DIR/$HOST_NAME.json" << EOF
{
  "name": "$HOST_NAME",
  "description": "GNostr Signer - NIP-07 Native Messaging Host",
  "path": "$BINARY_PATH",
  "type": "stdio",
  "allowed_origins": [
    "chrome-extension://$CHROME_EXTENSION_ID/"
  ]
}
EOF
    echo "Installed: $CHROME_DIR/$HOST_NAME.json"
  fi

  # Also install for Chromium
  CHROMIUM_DIR=$(chromium_manifest_dir)
  if [ -n "$CHROMIUM_DIR" ] && [ "$CHROMIUM_DIR" != "$CHROME_DIR" ]; then
    echo "Installing Chromium manifest to $CHROMIUM_DIR..."
    mkdir -p "$CHROMIUM_DIR"

    cat > "$CHROMIUM_DIR/$HOST_NAME.json" << EOF
{
  "name": "$HOST_NAME",
  "description": "GNostr Signer - NIP-07 Native Messaging Host",
  "path": "$BINARY_PATH",
  "type": "stdio",
  "allowed_origins": [
    "chrome-extension://$CHROME_EXTENSION_ID/"
  ]
}
EOF
    echo "Installed: $CHROMIUM_DIR/$HOST_NAME.json"
  fi
fi

# Install Firefox manifest
if [ "$INSTALL_FIREFOX" = true ]; then
  FIREFOX_DIR=$(firefox_manifest_dir)
  if [ -n "$FIREFOX_DIR" ]; then
    echo "Installing Firefox manifest to $FIREFOX_DIR..."
    mkdir -p "$FIREFOX_DIR"

    cat > "$FIREFOX_DIR/$HOST_NAME.json" << EOF
{
  "name": "$HOST_NAME",
  "description": "GNostr Signer - NIP-07 Native Messaging Host",
  "path": "$BINARY_PATH",
  "type": "stdio",
  "allowed_extensions": [
    "$FIREFOX_EXTENSION_ID"
  ]
}
EOF
    echo "Installed: $FIREFOX_DIR/$HOST_NAME.json"
  fi
fi

echo ""
echo "Installation complete!"
echo ""
if [ "$CHROME_EXTENSION_ID" = "EXTENSION_ID_PLACEHOLDER" ]; then
  echo "NOTE: Chrome manifest uses placeholder extension ID."
  echo "Update the manifest with your extension's ID to enable Chrome support."
fi
echo ""
echo "To test the installation, run:"
echo "  echo '{\"method\":\"getPublicKey\",\"id\":1}' | $BINARY_PATH"
echo ""
