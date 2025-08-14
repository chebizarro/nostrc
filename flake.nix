{
  description = "GNostr monorepo (gnostr, gnostr-signer, gnostr-signer-daemon)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; }; 
      in {
        packages = {
          gnostr-signer-daemon = pkgs.stdenv.mkDerivation {
            pname = "gnostr-signer-daemon";
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
          gnostr-signer-daemon-tcp = pkgs.stdenv.mkDerivation {
            pname = "gnostr-signer-daemon-tcp";
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
        defaultPackage = self.packages.${system}.gnostr-signer-daemon;

        apps.default = {
          type = "app";
          program = "${self.packages.${system}.gnostr-signer-daemon}/bin/gnostr-signer-daemon";
        };
      }) // {
        overlays.default = final: prev: {
          gnostr-signer-daemon = self.packages.${final.system}.gnostr-signer-daemon;
          gnostr-signer-daemon-tcp = self.packages.${final.system}.gnostr-signer-daemon-tcp;
        };
      };
}
