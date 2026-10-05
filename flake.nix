{
  description = "GNostr monorepo (gnostr, grotto, grotto-daemon)";

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
          grotto-daemon = self.packages.${final.system}.grotto-daemon;
          grotto-daemon-tcp = self.packages.${final.system}.grotto-daemon-tcp;
        };
      };
}
