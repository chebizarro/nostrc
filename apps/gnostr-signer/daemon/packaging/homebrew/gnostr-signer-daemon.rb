class GnostrSignerDaemon < Formula
  desc "GNostr Signer Daemon"
  homepage "https://github.com/chebizarro/nostrc"
  url "https://github.com/chebizarro/nostrc/archive/refs/tags/v0.0.0.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000" # update on release
  license "MIT"

  depends_on "cmake" => :build
  depends_on "pkg-config" => :build
  depends_on "ninja" => :build
  depends_on "glib" 

  def install
    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DENABLE_TCP_IPC=ON"
    system "cmake", "--build", "build", "-j", "--target", "gnostr-signer-daemon"
    bin.install "build/apps/gnostr-signer/gnostr-signer-daemon"
    (prefix/"homebrew.mxcl").mkpath
    (prefix/"homebrew.mxcl"/"org.gnostr.signer.daemon.plist").write plist
  end

  service do
    run [opt_bin/"gnostr-signer-daemon"]
    environment_variables NOSTR_SIGNER_ENDPOINT: "tcp:127.0.0.1:5897"
    keep_alive true
    run_type :immediate
    log_path var/"log/gnostr-signer-daemon.log"
    error_log_path var/"log/gnostr-signer-daemon.log"
  end

  def plist
    <<~EOS
    <?xml version="1.0" encoding="UTF-8"?>
    <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
    <plist version="1.0">
      <dict>
        <key>Label</key>
        <string>org.gnostr.signer.daemon</string>
        <key>ProgramArguments</key>
        <array>
          <string>#{opt_bin}/gnostr-signer-daemon</string>
        </array>
        <key>EnvironmentVariables</key>
        <dict>
          <key>NOSTR_SIGNER_ENDPOINT</key>
          <string>tcp:127.0.0.1:5897</string>
        </dict>
        <key>RunAtLoad</key>
        <true/>
        <key>KeepAlive</key>
        <true/>
        <key>StandardOutPath</key>
        <string>#{var}/log/gnostr-signer-daemon.log</string>
        <key>StandardErrorPath</key>
        <string>#{var}/log/gnostr-signer-daemon.log</string>
      </dict>
    </plist>
    EOS
  end
end
