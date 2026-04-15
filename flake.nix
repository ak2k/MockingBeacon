{
  description = "Everytag BLE beacon firmware";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    west2nix = {
      # Use PR #2 branch: pre-builds fake git repos as a cached derivation
      # (avoids 51x git-add-A on every build)
      url = "github:wrvsrx/west2nix/reduce-repeating-clone";
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
              export PYTHONPATH="${pythonEnv}/${pythonEnv.python.sitePackages}"

              west build --board ${board} -d build --pristine \
                -- \
                ${if boardRoot then "-DBOARD_ROOT=$PWD" else ""} \
                -DCONF_FILE=${confFile} \
                -DEXTRA_CONF_FILE=dfu.conf
            '';

            installPhase = ''
              mkdir -p $out
              cp build/everytag/zephyr/zephyr.elf $out/
              cp build/everytag/zephyr/zephyr.signed.hex $out/ 2>/dev/null || true
              cp build/merged.hex $out/ 2>/dev/null || true
              cp build/dfu_application.zip $out/ 2>/dev/null || true
              ${pkgs.gcc-arm-embedded}/bin/arm-none-eabi-size build/everytag/zephyr/zephyr.elf | tee $out/size.txt
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

        # BabbleSim components (fetched from pinned revisions in bsim west.yml)
        packages.bsim = pkgs.gccMultiStdenv.mkDerivation {
          name = "babblesim";
          # Fetch all bsim component repos at pinned revisions
          srcs = [
            (builtins.fetchGit { url = "https://github.com/BabbleSim/base"; rev = "a3dff9a57f334fb25daa9625841cd64cbfe56681"; allRefs = true; })
            (builtins.fetchGit { url = "https://github.com/BabbleSim/ext_2G4_libPhyComv1"; rev = "aa4951317cc7d84f24152ea38ac9ac21e6d78a76"; allRefs = true; })
            (builtins.fetchGit { url = "https://github.com/BabbleSim/ext_2G4_phy_v1"; rev = "04eeb3c3794444122fbeeb3715f4233b0b50cfbb"; allRefs = true; })
            (builtins.fetchGit { url = "https://github.com/BabbleSim/ext_2G4_channel_NtNcable"; rev = "20a38c997f507b0aa53817aab3d73a462fff7af1"; allRefs = true; })
            (builtins.fetchGit { url = "https://github.com/BabbleSim/ext_2G4_channel_multiatt"; rev = "bde72a57384dde7a4310bcf3843469401be93074"; allRefs = true; })
            (builtins.fetchGit { url = "https://github.com/BabbleSim/ext_2G4_modem_magic"; rev = "edfcda2d3937a74be0a59d6cd47e0f50183453da"; allRefs = true; })
            (builtins.fetchGit { url = "https://github.com/BabbleSim/ext_2G4_modem_BLE_simple"; rev = "4d2379de510684cd4b1c3bbbb09bce7b5a20bc1f"; allRefs = true; })
            (builtins.fetchGit { url = "https://github.com/BabbleSim/ext_2G4_device_burst_interferer"; rev = "5b5339351d6e6a2368c686c734dc8b2fc65698fc"; allRefs = true; })
            (builtins.fetchGit { url = "https://github.com/BabbleSim/ext_2G4_device_WLAN_actmod"; rev = "9cb6d8e72695f6b785e57443f0629a18069d6ce4"; allRefs = true; })
            (builtins.fetchGit { url = "https://github.com/BabbleSim/ext_2G4_device_playback"; rev = "abb48cd71ddd4e2a9022f4bf49b2712524c483e8"; allRefs = true; })
            (builtins.fetchGit { url = "https://github.com/BabbleSim/ext_2G4_device_playbackv2"; rev = "0a3c28ecd59b5ee08ed4668446c243d3ffd98b46"; allRefs = true; })
            (builtins.fetchGit { url = "https://github.com/BabbleSim/ext_libCryptov1"; rev = "236309584c90be32ef12848077bd6de54e9f4deb"; allRefs = true; })
          ];
          sourceRoot = ".";
          nativeBuildInputs = [ pkgs.gnumake ];

          # Arrange sources into components/ layout:
          #   components/         <- base repo (has Makefile with 'everything' target)
          #   components/ext_*/   <- extension repos
          unpackPhase = ''
            srcs_arr=($srcs)
            dirs=(
              components
              components/ext_2G4_libPhyComv1
              components/ext_2G4_phy_v1
              components/ext_2G4_channel_NtNcable
              components/ext_2G4_channel_multiatt
              components/ext_2G4_modem_magic
              components/ext_2G4_modem_BLE_simple
              components/ext_2G4_device_burst_interferer
              components/ext_2G4_device_WLAN_actmod
              components/ext_2G4_device_playback
              components/ext_2G4_device_playbackv2
              components/ext_libCryptov1
            )
            for i in "''${!srcs_arr[@]}"; do
              mkdir -p "''${dirs[$i]}"
              cp -r "''${srcs_arr[$i]}/." "''${dirs[$i]}"
            done
            chmod -R u+w .
          '';

          buildPhase = ''
            export BSIM_OUT_PATH=$out
            export BSIM_COMPONENTS_PATH=$PWD/components
            # Symlink at top level matches bsim_west layout
            ln -sf components/common/Makefile Makefile
            make everything -j1
          '';

          installPhase = ''
            test -f $out/bin/bs_2G4_phy_v1
            # Merge source headers into output — the Makefile only installs
            # binaries/libs to $out but downstream builds need headers from
            # paths like $BSIM_COMPONENTS_PATH/libUtilv1/src/bs_tracing.h
            chmod -R u+w $out/components
            for d in components/*/src; do
              target="$out/$d"
              mkdir -p "$target"
              cp -n "$d"/*.h "$target/" 2>/dev/null || true
            done
          '';
        };

        # BabbleSim BLE test derivation (Linux only)
        # gccMultiStdenv provides gcc with --enable-multilib (32-bit libgcc in 32/ subdir)
        packages.bsim-test = pkgs.gccMultiStdenv.mkDerivation {
          name = "everytag-bsim-test";
          src = ./.;
          nativeBuildInputs = commonBuildInputs;
          dontUseCmakeConfigure = true;
          dontUseWestConfigure = true;
          configurePhase = westConfigurePhase;

          buildPhase = ''
            export ZEPHYR_TOOLCHAIN_VARIANT=host
            export ZEPHYR_BASE="$PWD/../zephyr"
            export BSIM_COMPONENTS_PATH="${self.packages.${system}.bsim}/components"
            export BSIM_OUT_PATH="${self.packages.${system}.bsim}"

            BSIM=${self.packages.${system}.bsim}
            SRCDIR=$PWD
            RUNDIR=$PWD/run
            mkdir -p $RUNDIR

            # Set up run directory (phy uses ../lib/ relative paths)
            mkdir -p $RUNDIR/bin $RUNDIR/lib
            cp $BSIM/bin/* $RUNDIR/bin/
            cp $BSIM/lib/* $RUNDIR/lib/ 2>/dev/null || true

            # Build BLE adv test (nrf52_bsim)
            cmake -GNinja -B build-bsim \
              -DBOARD=nrf52_bsim \
              -DZEPHYR_TOOLCHAIN_VARIANT=host \
              -DBSIM_COMPONENTS_PATH="$BSIM_COMPONENTS_PATH" \
              -DWEST_PYTHON=${pythonEnv}/bin/python3 \
              -S tests/bsim
            ninja -C build-bsim
            cp build-bsim/zephyr/zephyr.exe $RUNDIR/bin/bs_nrf52_bsim_adv

            # Build BLE adv test (nrf54l15bsim)
            cmake -GNinja -B build-bsim-54l \
              -DBOARD=nrf54l15bsim/nrf54l15/cpuapp \
              -DZEPHYR_TOOLCHAIN_VARIANT=host \
              -DBSIM_COMPONENTS_PATH="$BSIM_COMPONENTS_PATH" \
              -DWEST_PYTHON=${pythonEnv}/bin/python3 \
              -S tests/bsim
            ninja -C build-bsim-54l
            cp build-bsim-54l/zephyr/zephyr.exe $RUNDIR/bin/bs_nrf54l15bsim_adv

            # Build SMP echo test (nrf52_bsim)
            cmake -GNinja -B build-bsim-dfu \
              -DBOARD=nrf52_bsim \
              -DZEPHYR_TOOLCHAIN_VARIANT=host \
              -DBSIM_COMPONENTS_PATH="$BSIM_COMPONENTS_PATH" \
              -DWEST_PYTHON=${pythonEnv}/bin/python3 \
              -S tests/bsim_dfu
            ninja -C build-bsim-dfu
            cp build-bsim-dfu/zephyr/zephyr.exe $RUNDIR/bin/bs_nrf52_bsim_smp

            # Run all tests
            cd $RUNDIR/bin

            echo "=== BLE adv + MAC test (nrf52_bsim) ==="
            ./bs_nrf52_bsim_adv -v=2 -s=adv52 -d=0 -rs=420 -testid=advertiser &
            ./bs_nrf52_bsim_adv -v=2 -s=adv52 -d=1 -rs=69  -testid=scanner &
            PID=$!; ./bs_2G4_phy_v1 -v=2 -s=adv52 -D=2 -sim_length=5000000; wait $PID

            echo "=== BLE adv + MAC test (nrf54l15bsim) ==="
            ./bs_nrf54l15bsim_adv -v=2 -s=adv54 -d=0 -rs=420 -testid=advertiser &
            ./bs_nrf54l15bsim_adv -v=2 -s=adv54 -d=1 -rs=69  -testid=scanner &
            PID=$!; ./bs_2G4_phy_v1 -v=2 -s=adv54 -D=2 -sim_length=5000000; wait $PID

            echo "=== SMP echo test (nrf52_bsim) ==="
            ./bs_nrf52_bsim_smp -v=2 -s=smp -d=0 -rs=420 -testid=smp_server &
            ./bs_nrf52_bsim_smp -v=2 -s=smp -d=1 -rs=69  -testid=smp_client &
            PID=$!; ./bs_2G4_phy_v1 -v=2 -s=smp -D=2 -sim_length=10000000; wait $PID

            # Build + run ZMS persistence test (native_sim, no BLE needed)
            echo "=== ZMS persistence test (native_sim) ==="
            cmake -GNinja -B $SRCDIR/build-zms \
              -DBOARD=native_sim \
              -DZEPHYR_TOOLCHAIN_VARIANT=host \
              -DWEST_PYTHON=${pythonEnv}/bin/python3 \
              -S $SRCDIR/tests/zms_persist
            ninja -C $SRCDIR/build-zms
            $SRCDIR/build-zms/zephyr/zephyr.exe
          '';

          installPhase = ''
            mkdir -p $out
            echo "All tests passed" > $out/result.txt
            cp $RUNDIR/bin/bs_nrf52_bsim_adv $out/
            cp $RUNDIR/bin/bs_nrf54l15bsim_adv $out/
            cp $RUNDIR/bin/bs_nrf52_bsim_smp $out/
          '';
        };

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
