{
  pkgs ? import <nixpkgs> { system = "x86_64-linux"; },
}:

# NixOS VM test for BabbleSim BLE advertisement verification.
# Runs an x86_64 VM that verifies beacon broadcasts + key rotation.
#
# For local development on x86_64 Linux (requires KVM):
#   nix build -f tests/bsim/vmtest.nix -L
#
# For CI, use the GitHub Actions workflow instead (.github/workflows/bsim-test.yml)
# — it's faster since it skips the VM overhead.
#
# NOTE: Requires network access inside the VM (west update fetches Zephyr + bsim).
# Will NOT work on aarch64 hosts (nested QEMU too slow, Kconfig 64BIT mismatch).

let
  pythonEnv = pkgs.python312.withPackages (
    ps: with ps; [
      west
      pyelftools
      pyyaml
      pykwalify
      packaging
    ]
  );

  # Copy repo source into the Nix store (sandbox-safe)
  repoSrc = builtins.path {
    path = ../..;
    name = "everytag-src";
    filter =
      path: type:
      # Exclude build artifacts and .git
      let
        baseName = builtins.baseNameOf path;
      in
      baseName != ".git"
      && baseName != "build-810"
      && baseName != "build-test"
      && baseName != "build-k4p"
      && baseName != "build-wb"
      && baseName != "build-52832"
      && baseName != "result";
  };
in
pkgs.testers.nixosTest {
  name = "everytag-bsim-ble-test";

  nodes.machine =
    { pkgs, lib, ... }:
    {
      documentation.enable = false;

      environment.systemPackages = [
        pkgs.gcc
        pkgs.gnumake
        pkgs.cmake
        pkgs.ninja
        pkgs.dtc
        pkgs.gperf
        pkgs.git
        pythonEnv
      ];

      virtualisation = {
        memorySize = 4096;
        diskSize = 16384;
        cores = 4;
      };
    };

  testScript = ''
    machine.wait_for_unit("multi-user.target")

    # Copy source from Nix store to writable location
    machine.succeed("cp -r ${repoSrc} /root/Everytag && chmod -R u+w /root/Everytag")
    machine.succeed("cd /root/Everytag && git init -q && git add -A")

    # Initialize west workspace for Zephyr
    machine.succeed(
      "cd /root && west init -l Everytag 2>&1 | tail -3"
    )
    machine.succeed(
      "cd /root && west update --narrow -o=--depth=1 2>&1 | tail -5"
    )

    # Fetch and build BabbleSim
    machine.succeed(
      "cd /root/tools && west init -l bsim 2>&1 | tail -3"
    )
    machine.succeed(
      "cd /root/tools && west update --narrow -o=--depth=1 2>&1 | tail -5"
    )
    machine.succeed(
      "cd /root/tools/bsim && "
      "export BSIM_OUT_PATH=/root/bsim_out && "
      "make everything -j4 2>&1 | tail -5"
    )

    # Verify phy built
    machine.succeed("test -f /root/bsim_out/bin/bs_2G4_phy_v1")

    # Build the bsim test
    machine.succeed(
      "export ZEPHYR_BASE=/root/zephyr && "
      "export BSIM_OUT_PATH=/root/bsim_out && "
      "export BSIM_COMPONENTS_PATH=/root/tools/bsim/components && "
      "export BOARD=nrf52_bsim && "
      "export ZEPHYR_TOOLCHAIN_VARIANT=host && "
      "source $ZEPHYR_BASE/tests/bsim/compile.source && "
      "app=tests/bsim app_root=/root/Everytag _compile"
    )

    # Verify test binary built
    machine.succeed("test -f /root/bsim_out/bin/bs_nrf52_bsim_tests_bsim_prj_conf")

    # Run the BLE advertisement + key rotation + MAC verification test (nrf52_bsim)
    machine.succeed(
      "cd /root/bsim_out/bin && "
      "./bs_nrf52_bsim_tests_bsim_prj_conf -v=2 -s=everytag_test -d=0 -rs=420 -testid=advertiser & "
      "./bs_nrf52_bsim_tests_bsim_prj_conf -v=2 -s=everytag_test -d=1 -rs=69  -testid=scanner & "
      "./bs_2G4_phy_v1 -v=2 -s=everytag_test -D=2 -sim_length=5000000 && "
      "wait"
    )

    machine.log("BabbleSim nrf52_bsim test PASSED")

    # Build the same test for nrf54l15bsim
    machine.succeed(
      "export ZEPHYR_BASE=/root/zephyr && "
      "export BSIM_OUT_PATH=/root/bsim_out && "
      "export BSIM_COMPONENTS_PATH=/root/tools/bsim/components && "
      "export BOARD=nrf54l15bsim/nrf54l15/cpuapp && "
      "export ZEPHYR_TOOLCHAIN_VARIANT=host && "
      "source $ZEPHYR_BASE/tests/bsim/compile.source && "
      "app=tests/bsim app_root=/root/Everytag _compile"
    )

    machine.succeed("test -f /root/bsim_out/bin/bs_nrf54l15bsim_nrf54l15_cpuapp_tests_bsim_prj_conf")

    # Run the same test on nrf54l15bsim
    machine.succeed(
      "cd /root/bsim_out/bin && "
      "./bs_nrf54l15bsim_nrf54l15_cpuapp_tests_bsim_prj_conf -v=2 -s=everytag_54l_test -d=0 -rs=420 -testid=advertiser & "
      "./bs_nrf54l15bsim_nrf54l15_cpuapp_tests_bsim_prj_conf -v=2 -s=everytag_54l_test -d=1 -rs=69  -testid=scanner & "
      "./bs_2G4_phy_v1 -v=2 -s=everytag_54l_test -D=2 -sim_length=5000000 && "
      "wait"
    )

    machine.log("BabbleSim nrf54l15bsim test PASSED")
  '';
}
