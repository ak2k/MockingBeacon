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
    git-hooks = {
      url = "github:cachix/git-hooks.nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
      west2nix,
      git-hooks,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        pythonEnv = pkgs.python313.withPackages (
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
            tomlkit
          ]
        );

        testPythonEnv = pkgs.python313.withPackages (
          ps: with ps; [
            bumble
            pytest
            pytest-asyncio
          ]
        );

        pre-commit-check = git-hooks.lib.${system}.run {
          src = ./.;
          hooks = {
            clang-format = {
              enable = true;
              types_or = pkgs.lib.mkForce [
                "c"
                "c++"
              ];
              files = "^src/";
            };
            ruff = {
              enable = true;
            };
            ruff-format = {
              enable = true;
            };
            nixfmt-rfc-style.enable = true;
          };
        };

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
          # zephyr_module.py (invoked by sysbuild/MCUboot) reads the manifest
          # project's git metadata via `git rev-parse HEAD`. The west2nix hook
          # only fakes git repos for imported projects, not the manifest dir
          # itself. Without this, DFU builds fail at zephyr_module.py:589 with
          # `TypeError: unsupported operand type(s) for +=: 'NoneType' and 'str'`.
          ${pkgs.gitMinimal}/bin/git -C everytag init -q
          ${pkgs.gitMinimal}/bin/git -C everytag config user.email 'nix@build'
          ${pkgs.gitMinimal}/bin/git -C everytag config user.name 'Nix Build'
          ${pkgs.gitMinimal}/bin/git -C everytag add -A
          ${pkgs.gitMinimal}/bin/git -C everytag commit -q -m 'Nix build snapshot'
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
                -DCMAKE_BUILD_TYPE=MinSizeRel \
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
                -DCMAKE_BUILD_TYPE=MinSizeRel \
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

        # Board definitions: each gets dev + release builds, and optionally DFU.
        boards = [
          {
            short = "nrf52810";
            board = "kkm_p1_nrf52810";
            boardRoot = true;
            dfu = false;
          }
          {
            short = "nrf52832";
            board = "nrf52dk/nrf52832";
            boardRoot = false;
            dfu = true;
          }
          {
            short = "nrf52833";
            board = "nrf52833dk/nrf52833";
            boardRoot = false;
            dfu = true;
          }
          {
            short = "nrf54l15";
            board = "nrf54l15dk/nrf54l15/cpuapp";
            boardRoot = false;
            dfu = true;
          }
        ];

        # Generate firmware-<short> (dev) and firmware-<short>-release for each board,
        # plus firmware-<short>-dfu where dfu=true.
        firmwarePackages = builtins.listToAttrs (
          builtins.concatMap (
            b:
            [
              {
                name = "firmware-${b.short}";
                value = mkFirmware {
                  name = "everytag-firmware-${b.short}";
                  inherit (b) board boardRoot;
                };
              }
              {
                name = "firmware-${b.short}-release";
                value = mkFirmware {
                  name = "everytag-firmware-${b.short}-release";
                  inherit (b) board boardRoot;
                  confFile = "prj-lowpower.conf";
                };
              }
            ]
            ++ pkgs.lib.optionals b.dfu [
              {
                name = "firmware-${b.short}-dfu";
                value = mkFirmwareDfu {
                  name = "everytag-firmware-${b.short}-dfu";
                  inherit (b) board boardRoot;
                };
              }
            ]
          ) boards
        );
      in
      {
        packages = firmwarePackages // {
          # Legacy alias
          firmware-release = firmwarePackages.firmware-nrf52810-release;

          # nix build .#firmware — builds all dev board targets
          firmware = pkgs.symlinkJoin {
            name = "everytag-firmware-all";
            paths = map (b: firmwarePackages."firmware-${b.short}") boards;
          };

          default = self.packages.${system}.firmware;

          # BabbleSim components (fetched from pinned revisions in bsim west.yml)
          bsim =
            let
              bsimComponents = [
                {
                  dir = "components";
                  repo = "base";
                  rev = "a3dff9a57f334fb25daa9625841cd64cbfe56681";
                }
                {
                  dir = "components/ext_2G4_libPhyComv1";
                  repo = "ext_2G4_libPhyComv1";
                  rev = "aa4951317cc7d84f24152ea38ac9ac21e6d78a76";
                }
                {
                  dir = "components/ext_2G4_phy_v1";
                  repo = "ext_2G4_phy_v1";
                  rev = "04eeb3c3794444122fbeeb3715f4233b0b50cfbb";
                }
                {
                  dir = "components/ext_2G4_channel_NtNcable";
                  repo = "ext_2G4_channel_NtNcable";
                  rev = "20a38c997f507b0aa53817aab3d73a462fff7af1";
                }
                {
                  dir = "components/ext_2G4_channel_multiatt";
                  repo = "ext_2G4_channel_multiatt";
                  rev = "bde72a57384dde7a4310bcf3843469401be93074";
                }
                {
                  dir = "components/ext_2G4_modem_magic";
                  repo = "ext_2G4_modem_magic";
                  rev = "edfcda2d3937a74be0a59d6cd47e0f50183453da";
                }
                {
                  dir = "components/ext_2G4_modem_BLE_simple";
                  repo = "ext_2G4_modem_BLE_simple";
                  rev = "4d2379de510684cd4b1c3bbbb09bce7b5a20bc1f";
                }
                {
                  dir = "components/ext_2G4_device_burst_interferer";
                  repo = "ext_2G4_device_burst_interferer";
                  rev = "5b5339351d6e6a2368c686c734dc8b2fc65698fc";
                }
                {
                  dir = "components/ext_2G4_device_WLAN_actmod";
                  repo = "ext_2G4_device_WLAN_actmod";
                  rev = "9cb6d8e72695f6b785e57443f0629a18069d6ce4";
                }
                {
                  dir = "components/ext_2G4_device_playback";
                  repo = "ext_2G4_device_playback";
                  rev = "abb48cd71ddd4e2a9022f4bf49b2712524c483e8";
                }
                {
                  dir = "components/ext_2G4_device_playbackv2";
                  repo = "ext_2G4_device_playbackv2";
                  rev = "0a3c28ecd59b5ee08ed4668446c243d3ffd98b46";
                }
                {
                  dir = "components/ext_libCryptov1";
                  repo = "ext_libCryptov1";
                  rev = "236309584c90be32ef12848077bd6de54e9f4deb";
                }
              ];
            in
            pkgs.gccMultiStdenv.mkDerivation {
              name = "babblesim";
              srcs = map (
                c:
                builtins.fetchGit {
                  url = "https://github.com/BabbleSim/${c.repo}";
                  inherit (c) rev;
                  allRefs = true;
                }
              ) bsimComponents;
              sourceRoot = ".";
              nativeBuildInputs = [ pkgs.gnumake ];

              unpackPhase =
                let
                  dirs = map (c: c.dir) bsimComponents;
                  dirsStr = builtins.concatStringsSep " " dirs;
                in
                ''
                  srcs_arr=($srcs)
                  dirs=(${dirsStr})
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
          bsim-test = pkgs.gccMultiStdenv.mkDerivation {
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
        };

        devShells.default = pkgs.mkShell {
          inherit (pre-commit-check) shellHook;
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
          ]
          ++ pre-commit-check.enabledPackages;

          ZEPHYR_TOOLCHAIN_VARIANT = "gnuarmemb";
          GNUARMEMB_TOOLCHAIN_PATH = "${pkgs.gcc-arm-embedded}";
        };

        checks.default =
          let
            apps = self.apps.${system};
          in
          pkgs.runCommand "everytag-checks" { src = ./.; } ''
            ${apps.test.program} $src
            ${apps.lint.program} $src
            ${apps.cppcheck.program} $src
            ${apps.test-ble.program} $src
            mkdir -p $out
            echo "All checks passed" > $out/result.txt
          '';

        apps =
          let
            mkApp =
              {
                name,
                runtimeInputs ? [ ],
                text,
              }:
              let
                drv = pkgs.writeShellApplication { inherit name runtimeInputs text; };
              in
              {
                type = "app";
                program = "${drv}/bin/${name}";
              };
          in
          {
            test = mkApp {
              name = "everytag-test";
              runtimeInputs = [
                pkgs.cmake
                pkgs.clang
              ];
              text = ''
                cd "''${1:-.}/tests/host"
                cmake -B build \
                  -DCMAKE_CXX_COMPILER=clang++ \
                  -DCMAKE_C_COMPILER=clang \
                  2>/dev/null
                cmake --build build 2>&1
                ./build/host_tests
              '';
            };

            test-ble = mkApp {
              name = "everytag-test-ble";
              runtimeInputs = [ testPythonEnv ];
              text = ''
                cd "''${1:-.}"
                pytest tests/ble_client/ -v "$@"
              '';
            };

            format = mkApp {
              name = "everytag-format";
              runtimeInputs = [ pkgs.clang-tools ];
              text = ''
                cd "''${1:-.}"
                clang-format -i src/*.cpp src/*.hpp
                echo "Formatted src/*.cpp src/*.hpp"
              '';
            };

            lint = mkApp {
              name = "everytag-lint";
              runtimeInputs = [ pkgs.clang-tools ];
              text = ''
                cd "''${1:-.}"
                clang-format --dry-run --Werror src/*.cpp src/*.hpp
                echo "Lint passed"
              '';
            };

            cppcheck = mkApp {
              name = "everytag-cppcheck";
              runtimeInputs = [ pkgs.cppcheck ];
              text = ''
                cd "''${1:-.}"
                cppcheck --enable=warning,performance,portability \
                  --std=c++20 --error-exitcode=1 --force \
                  --suppress=missingIncludeSystem \
                  --suppress=normalCheckLevelMaxBranches \
                  src/*.cpp src/*.hpp
              '';
            };

            update-lockfile = mkApp {
              name = "update-west2nix";
              runtimeInputs = [
                pythonEnv
                pkgs.nix-prefetch-git
              ];
              text = ''
                cd "''${1:-.}"
                if [ ! -d ../nrf ] || [ ! -d ../zephyr ]; then
                  echo "Initializing west workspace..."
                  (cd .. && west init -l Everytag)
                fi
                echo "Updating west projects (this may take a few minutes)..."
                (cd .. && west update --narrow -o=--depth=1)
                echo "Regenerating west2nix.toml..."
                python3 scripts/west2nix.py -o west2nix.toml
                echo "Done. Review west2nix.toml and commit if acceptable."
              '';
            };
          };
      }
    );
}
