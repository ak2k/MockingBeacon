{
  description = "Everytag BLE beacon firmware dev environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        pythonEnv = pkgs.python312.withPackages (ps: with ps; [
          west
          pyelftools
          pyyaml
          pykwalify
          packaging
          progress
          cbor2
          intelhex
          click
          cryptography
          pillow
        ]);
      in
      {
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            # Build tools
            cmake
            ninja
            dtc
            gperf

            # ARM cross-compiler
            gcc-arm-embedded

            # Python + west + Zephyr deps
            pythonEnv

            # C++ quality tools
            clang-tools
            cppcheck

            # Renode (Phase 7)
            # renode  # uncomment when needed

            # Misc
            git
            wget
            cacert
          ];

          shellHook = ''
            export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
            export GNUARMEMB_TOOLCHAIN_PATH="${pkgs.gcc-arm-embedded}"

            # Initialize west workspace if not done
            if [ ! -d .west ]; then
              echo "No .west directory found. Run:"
              echo "  west init -l ."
              echo "  west update"
            fi
          '';
        };
      }
    );
}
