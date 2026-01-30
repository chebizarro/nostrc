# Homebrew formula for gnostr
# Install with: brew install --build-from-source gnostr.rb
# Or via tap: brew tap gnostr/gnostr && brew install gnostr

class Gnostr < Formula
  desc "A GTK4 Nostr client for the GNOME desktop"
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
  depends_on "nsync"

  # Optional for video playback
  depends_on "gstreamer" => :optional

  def install
    # Set up environment for OpenSSL
    ENV["OPENSSL_ROOT_DIR"] = Formula["openssl@3"].opt_prefix
    ENV.prepend_path "PKG_CONFIG_PATH", Formula["openssl@3"].opt_lib/"pkgconfig"
    ENV.prepend_path "PKG_CONFIG_PATH", Formula["libsoup"].opt_lib/"pkgconfig"

    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_INSTALL_PREFIX=#{prefix}",
                    *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  def post_install
    # Compile GSettings schemas
    system "glib-compile-schemas", "#{HOMEBREW_PREFIX}/share/glib-2.0/schemas"
  end

  test do
    # Test that binary runs and shows version/help
    assert_match "gnostr", shell_output("#{bin}/gnostr --help 2>&1", 0)
  end
end
