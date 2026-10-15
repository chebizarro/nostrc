{
  description = "nostrc monorepo: Groundhog (private Nostr messaging), Grotto (signer)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        # The desktop apps (W30, nostrc-csrz). Both link the in-tree
        # libraries statically and install only their own files; BUILD_TESTING
        # must stay OFF (a testing build refuses to install). From a git
        # checkout build with `nix build '.?submodules=1#groundhog'`: the
        # nostrdb and nsync submodules are part of the source.
        desktopVersion = "0.12.0-alpha7";
        desktopCmakeFlags = [
          "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
          "-DCMAKE_INSTALL_LIBDIR=lib"
          "-DBUILD_GROUNDHOG=ON"
          "-DBUILD_APPS=ON"
          "-DBUILD_GNOSTR_APP=OFF"
          "-DBUILD_NATIVE_HOST=OFF"
          "-DBUILD_SIGNER_TESTS=OFF"
          "-DGROTTO_WITH_PKCS11=OFF"
          "-DGROTTO_WITH_HW_WALLET=OFF"
          "-DBUILD_RELAYD=OFF"
          "-DBUILD_LIBHANAMI=OFF"
          "-DSIGNET_ENABLE=OFF"
          "-DBUILD_TESTING=OFF"
          "-DBUILD_TESTING_FRAMEWORK=OFF"
          "-DNOSTR_USE_SYSTEM_NSYNC=ON"
        ];
        # gettext: xgettext/msgfmt build the locale catalogs (nostrc-gofet.10).
        desktopNativeBuildInputs = with pkgs; [ cmake ninja pkg-config python3 glib gettext wrapGAppsHook4 ];
        desktopBuildInputs = with pkgs; [
          glib gtk4 libadwaita json-glib libsoup_3 libsecret sqlcipher libxml2
          gst_all_1.gstreamer gst_all_1.gst-plugins-base gst_all_1.gst-plugins-good
          openssl secp256k1 libsodium jansson libwebsockets nsync curl
          qrencode # Groundhog's NIP-46 pairing QR and Grotto's own QR UI
          gdk-pixbuf zbar
        ];
      in {
        packages = {
          groundhog = pkgs.stdenv.mkDerivation {
            pname = "groundhog";
            version = desktopVersion;
            src = ./.;
            nativeBuildInputs = desktopNativeBuildInputs;
            buildInputs = desktopBuildInputs;
            cmakeFlags = desktopCmakeFlags ++ [ "-DGROUNDHOG_PACKAGE_CHANNEL=nix" ];
            ninjaFlags = [ "gnome/groundhog/all" ];
            doCheck = false;
            installPhase = ''
              runHook preInstall
              cmake --install gnome/groundhog
              # The locale machinery must ship (alpha 6 release blocker).
              test -f "$out/share/locale/en@pseudo/LC_MESSAGES/groundhog.mo"
              runHook postInstall
            '';
            meta = with pkgs.lib; {
              description = "Private Nostr messaging with local Grotto or NIP-46 remote signers (alpha)";
              homepage = "https://github.com/chebizarro/nostrc";
              license = licenses.mit;
              mainProgram = "groundhog";
              platforms = platforms.linux;
            };
          };
          grotto = pkgs.stdenv.mkDerivation {
            pname = "grotto";
            version = "0.1.0";
            src = ./.;
            nativeBuildInputs = desktopNativeBuildInputs;
            buildInputs = desktopBuildInputs;
            # wrapGAppsHook renames the ELF; attest the actual executable,
            # never the shell wrapper. Installed acceptance must verify this.
            cmakeFlags = desktopCmakeFlags ++ [
              "-DNIP55L_APPROVAL_UI_PATHS=${builtins.placeholder "out"}/bin/.grotto-wrapped"
            ];
            ninjaFlags = [ "apps/grotto/all" ];
            doCheck = false;
            installPhase = ''
              runHook preInstall
              cmake --install apps/grotto
              substituteInPlace "$out/share/systemd/user/grotto-daemon.service" \
                "$out/share/doc/grotto/examples/"*.conf.example \
                --replace-fail /usr/bin/grotto-daemon "$out/bin/grotto-daemon"
              runHook postInstall
            '';
            meta = with pkgs.lib; {
              description = "Keeps your Nostr keys and signs for your apps (preview)";
              homepage = "https://github.com/chebizarro/nostrc";
              license = licenses.mit;
              mainProgram = "grotto";
              platforms = platforms.linux;
            };
          };
          # The native signer is one GUI+daemon output, also for headless use.
          grotto-daemon = self.packages.${system}.grotto;
        };
        defaultPackage = self.packages.${system}.grotto-daemon;

        apps.default = {
          type = "app";
          program = "${self.packages.${system}.grotto-daemon}/bin/grotto-daemon";
        };
      }) // {
        overlays.default = final: prev: {
          groundhog = self.packages.${final.system}.groundhog;
          grotto = self.packages.${final.system}.grotto;
          grotto-daemon = self.packages.${final.system}.grotto-daemon;
          grotto-daemon-tcp = self.packages.${final.system}.grotto-daemon-tcp;
        };
      };
}
