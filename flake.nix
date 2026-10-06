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
        desktopVersion = "0.12.0-alpha1";
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
        desktopNativeBuildInputs = with pkgs; [ cmake ninja pkg-config python3 glib wrapGAppsHook4 ];
        desktopBuildInputs = with pkgs; [
          glib gtk4 libadwaita json-glib libsoup_3 libsecret sqlcipher libxml2
          gst_all_1.gstreamer gst_all_1.gst-plugins-base gst_all_1.gst-plugins-good
          openssl secp256k1 libsodium jansson libwebsockets nsync curl qrencode gdk-pixbuf
        ];
      in {
        packages = {
          groundhog = pkgs.stdenv.mkDerivation {
            pname = "groundhog";
            version = desktopVersion;
            src = ./.;
            nativeBuildInputs = desktopNativeBuildInputs;
            buildInputs = desktopBuildInputs;
            cmakeFlags = desktopCmakeFlags;
            ninjaFlags = [ "gnome/groundhog/all" ];
            doCheck = false;
            installPhase = ''
              runHook preInstall
              cmake --install gnome/groundhog
              runHook postInstall
            '';
            meta = with pkgs.lib; {
              description = "Private messaging on Nostr for GNOME (alpha)";
              homepage = "https://github.com/chebizarro/nostrc";
              license = licenses.mit;
              mainProgram = "groundhog";
              platforms = platforms.linux;
            };
          };
          grotto = pkgs.stdenv.mkDerivation {
            pname = "grotto";
            version = desktopVersion;
            src = ./.;
            nativeBuildInputs = desktopNativeBuildInputs;
            buildInputs = desktopBuildInputs;
            cmakeFlags = desktopCmakeFlags;
            ninjaFlags = [ "apps/grotto/all" ];
            doCheck = false;
            installPhase = ''
              runHook preInstall
              cmake --install apps/grotto
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
          grotto-daemon = pkgs.stdenv.mkDerivation {
            pname = "grotto-daemon";
            version = "0.0.0"; # update on tag
            src = ./.;
            nativeBuildInputs = with pkgs; [ cmake ninja pkg-config ];
            buildInputs = with pkgs; [ glib jansson openssl libsecp256k1 libwebsockets nsync gtk4 libadwaita libsecret ];
            cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" ];
            doCheck = false;
            installPhase = ''
              cmake --install build --prefix $out
            '';
          };
          grotto-daemon-tcp = pkgs.stdenv.mkDerivation {
            pname = "grotto-daemon-tcp";
            version = "0.0.0"; # update on tag
            src = ./.;
            nativeBuildInputs = with pkgs; [ cmake ninja pkg-config ];
            buildInputs = with pkgs; [ glib jansson openssl libsecp256k1 libwebsockets nsync gtk4 libadwaita libsecret ];
            cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" "-DENABLE_TCP_IPC=ON" ];
            doCheck = false;
            installPhase = ''
              cmake --install build --prefix $out
            '';
          };
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
