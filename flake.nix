{
  description = "Everytag BLE beacon firmware";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    west2nix = {
      url = "github:adisbladis/west2nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
      west2nix,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        pythonEnv = pkgs.python312.withPackages (
          ps: with ps; [
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
          ]
        );

        west2nixPkgs = west2nix.lib.mkWest2nix { inherit pkgs; };
        west2nixHook = west2nixPkgs.mkWest2nixHook {
          manifest = ./west2nix.toml;
        };

        commonBuildInputs = [
          pkgs.cmake
          pkgs.ninja
          pkgs.dtc
          pkgs.gperf
          pkgs.gcc-arm-embedded
          pkgs.gitMinimal
          pythonEnv
          west2nixHook
        ];

        # Workspace layout used by both builders:
        #   $sourceRoot/everytag/  — project source
        #   $sourceRoot/zephyr/    — Zephyr tree (from west2nixHook)
        westConfigurePhase = ''
          runHook preConfigure
          cd ..
          mv $sourceRoot everytag
          mkdir -p $sourceRoot
          mv everytag $sourceRoot/
          cd $sourceRoot
          runHook postConfigure
          west init -l everytag
          cd everytag
        '';

        # Plain cmake build (no sysbuild, no MCUboot) for small-flash boards.
        mkFirmware =
          {
            name,
            board ? "kkm_p1_nrf52810",
            boardRoot ? true,
            confFile ? "prj.conf",
          }:
          pkgs.stdenv.mkDerivation {
            inherit name;
            src = ./.;
            nativeBuildInputs = commonBuildInputs;
            dontUseCmakeConfigure = true;
            dontUseWestConfigure = true;
            configurePhase = westConfigurePhase;

            buildPhase = ''
              export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
              export GNUARMEMB_TOOLCHAIN_PATH="${pkgs.gcc-arm-embedded}"
              export ZEPHYR_BASE="$PWD/../zephyr"

              cmake -GNinja -B build \
                -DBOARD=${board} \
                ${if boardRoot then "-DBOARD_ROOT=$PWD" else ""} \
                -DCONF_FILE=${confFile} \
                -DWEST_PYTHON=${pythonEnv}/bin/python3 \
                -S .
              ninja -C build
            '';

            installPhase = ''
              mkdir -p $out
              cp build/zephyr/zephyr.elf $out/
              cp build/zephyr/zephyr.hex $out/
              ${pkgs.gcc-arm-embedded}/bin/arm-none-eabi-size build/zephyr/zephyr.elf | tee $out/size.txt
            '';
          };

        # Sysbuild (MCUboot + app) for DFU-capable boards (>= 512KB flash).
        mkFirmwareDfu =
          {
            name,
            board,
            boardRoot ? false,
            confFile ? "prj-lowpower.conf",
          }:
          pkgs.stdenv.mkDerivation {
            inherit name;
            src = ./.;
            nativeBuildInputs = commonBuildInputs;
            dontUseCmakeConfigure = true;
            dontUseWestConfigure = true;
            configurePhase = westConfigurePhase;

            buildPhase = ''
              export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
              export GNUARMEMB_TOOLCHAIN_PATH="${pkgs.gcc-arm-embedded}"
              export ZEPHYR_BASE="$PWD/../zephyr"

              # Sysbuild spawns sub-projects (MCUboot) whose CMake may resolve
              # a different Python.  Export site-packages so pykwalify etc. are
              # importable regardless of which interpreter CMake finds.
              export PYTHONPATH="${pythonEnv}/${pythonEnv.python.sitePackages}"

              west build --board ${board} -d build --pristine \
                -- \
                ${if boardRoot then "-DBOARD_ROOT=$PWD" else ""} \
                -DCONF_FILE=${confFile} \
                -DEXTRA_CONF_FILE=dfu.conf
            '';

            installPhase = ''
              mkdir -p $out
              cp build/zephyr/zephyr.elf $out/
              cp build/zephyr/zephyr.signed.hex $out/ 2>/dev/null || true
              cp build/zephyr/app_update.bin $out/ 2>/dev/null || true
              cp build/zephyr/zephyr.hex $out/ 2>/dev/null || true
              ${pkgs.gcc-arm-embedded}/bin/arm-none-eabi-size build/zephyr/zephyr.elf | tee $out/size.txt
            '';
          };
      in
      {
        # Individual board targets
        packages.firmware-nrf52810 = mkFirmware { name = "everytag-firmware-nrf52810"; };

        packages.firmware-nrf54l15 = mkFirmware {
          name = "everytag-firmware-nrf54l15";
          board = "nrf54l15dk/nrf54l15/cpuapp";
          boardRoot = false;
        };

        packages.firmware-release = mkFirmware {
          name = "everytag-firmware-release";
          confFile = "prj-lowpower.conf";
        };

        # DFU-enabled targets (sysbuild + MCUboot)
        packages.firmware-nrf52832-dfu = mkFirmwareDfu {
          name = "everytag-firmware-nrf52832-dfu";
          board = "nrf52dk/nrf52832";
        };

        packages.firmware-nrf52833-dfu = mkFirmwareDfu {
          name = "everytag-firmware-nrf52833-dfu";
          board = "nrf52833dk/nrf52833";
        };

        packages.firmware-nrf54l15-dfu = mkFirmwareDfu {
          name = "everytag-firmware-nrf54l15-dfu";
          board = "nrf54l15dk/nrf54l15/cpuapp";
        };

        # nix build .#firmware — builds all board targets
        packages.firmware = pkgs.symlinkJoin {
          name = "everytag-firmware-all";
          paths = [
            self.packages.${system}.firmware-nrf52810
            self.packages.${system}.firmware-nrf54l15
          ];
        };

        packages.default = self.packages.${system}.firmware;

        devShells.default = pkgs.mkShell {
          packages = [
            pkgs.cmake
            pkgs.ninja
            pkgs.dtc
            pkgs.gperf
            pkgs.gcc-arm-embedded
            pythonEnv
            pkgs.clang-tools
            pkgs.cppcheck
            pkgs.git
            pkgs.wget
            pkgs.cacert
          ];

          shellHook = ''
            export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
            export GNUARMEMB_TOOLCHAIN_PATH="${pkgs.gcc-arm-embedded}"
          '';
        };

        # nix run .#test — host-native tests (ASan/UBSan)
        apps.test = {
          type = "app";
          program = toString (
            pkgs.writeShellScript "everytag-test" ''
              set -euo pipefail
              cd "''${1:-.}/tests/host"
              ${pkgs.cmake}/bin/cmake -B build \
                -DCMAKE_CXX_COMPILER=${pkgs.clang}/bin/clang++ \
                -DCMAKE_C_COMPILER=${pkgs.clang}/bin/clang \
                2>/dev/null
              ${pkgs.cmake}/bin/cmake --build build 2>&1
              ./build/host_tests
            ''
          );
        };

        # nix run .#format — auto-format C++ sources
        apps.format = {
          type = "app";
          program = toString (
            pkgs.writeShellScript "everytag-format" ''
              set -euo pipefail
              cd "''${1:-.}"
              ${pkgs.clang-tools}/bin/clang-format -i src/*.cpp src/*.hpp
              echo "Formatted src/*.cpp src/*.hpp"
            ''
          );
        };

        # nix run .#lint — check formatting (dry-run, fails on diff)
        apps.lint = {
          type = "app";
          program = toString (
            pkgs.writeShellScript "everytag-lint" ''
              set -euo pipefail
              cd "''${1:-.}"
              ${pkgs.clang-tools}/bin/clang-format --dry-run --Werror src/*.cpp src/*.hpp
              echo "Lint passed"
            ''
          );
        };

        # nix run .#update-lockfile — regenerate west2nix.toml
        apps.update-lockfile = {
          type = "app";
          program = toString (
            pkgs.writeShellScript "update-west2nix" ''
              set -euo pipefail
              echo "This requires a west workspace (run from parent of Everytag/)."
              echo "Run: cd .. && west update && cd Everytag && nix run .#update-lockfile"
              ${pythonEnv}/bin/python3 scripts/west2nix.py -o west2nix.toml
            ''
          );
        };
      }
    );
}
