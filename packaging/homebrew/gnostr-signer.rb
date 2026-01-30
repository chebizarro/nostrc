# Homebrew formula for gnostr-signer
# Install with: brew install --build-from-source gnostr-signer.rb
# Or via tap: brew tap gnostr/gnostr && brew install gnostr-signer

class GnostrSigner < Formula
  desc "NIP-46 Remote Signer for Nostr - GTK4 app with background daemon"
  homepage "https://github.com/chebizarro/nostrc"
  url "https://github.com/chebizarro/nostrc/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "PLACEHOLDER_SHA256"  # Update with actual sha256 after release
  license "GPL-3.0-or-later"
  head "https://github.com/chebizarro/nostrc.git", branch: "master"

  depends_on "cmake" => :build
  depends_on "ninja" => :build
  depends_on "pkg-config" => :build

  # Runtime dependencies
  depends_on "gtk4"
  depends_on "libadwaita"
  depends_on "glib"
  depends_on "json-glib"
  depends_on "jansson"
  depends_on "libsecp256k1"
  depends_on "libsodium"
  depends_on "libsoup"
  depends_on "openssl@3"
  depends_on "libsecret"

  # Optional for PKCS#11 HSM support
  depends_on "p11-kit" => :optional

  def install
    # Set up environment for OpenSSL
    ENV["OPENSSL_ROOT_DIR"] = Formula["openssl@3"].opt_prefix
    ENV.prepend_path "PKG_CONFIG_PATH", Formula["openssl@3"].opt_lib/"pkgconfig"
    ENV.prepend_path "PKG_CONFIG_PATH", Formula["libsoup"].opt_lib/"pkgconfig"

    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_INSTALL_PREFIX=#{prefix}",
                    *std_cmake_args
    system "cmake", "--build", "build", "--target", "gnostr-signer"
    system "cmake", "--build", "build", "--target", "gnostr-signer-daemon"

    # Install binaries
    bin.install "build/apps/gnostr-signer/gnostr-signer"
    bin.install "build/apps/gnostr-signer/gnostr-signer-daemon"

    # Install desktop file and icons
    share.install Dir["apps/gnostr-signer/data/icons"]
    (share/"applications").install Dir["apps/gnostr-signer/data/*.desktop"]

    # Install GSettings schemas
    (share/"glib-2.0/schemas").install Dir["apps/gnostr-signer/data/schemas/*.xml"]

    # Install launchd plist for daemon auto-start
    (prefix/"LaunchAgents").install "apps/gnostr-signer/packaging/macos/org.gnostr.Signer.daemon.plist"
  end

  def post_install
    # Compile GSettings schemas
    system "glib-compile-schemas", "#{HOMEBREW_PREFIX}/share/glib-2.0/schemas"
  end

  def caveats
    <<~EOS
      To start the gnostr-signer-daemon automatically at login:
        cp #{opt_prefix}/LaunchAgents/org.gnostr.Signer.daemon.plist ~/Library/LaunchAgents/
        launchctl load ~/Library/LaunchAgents/org.gnostr.Signer.daemon.plist

      To stop and remove the daemon:
        launchctl unload ~/Library/LaunchAgents/org.gnostr.Signer.daemon.plist
        rm ~/Library/LaunchAgents/org.gnostr.Signer.daemon.plist
    EOS
  end

  test do
    # Test that binary runs and shows version/help
    assert_match "signer", shell_output("#{bin}/gnostr-signer --help 2>&1", 0)
  end
end
