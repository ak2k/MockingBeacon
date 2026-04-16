# NCS migration baselines

Captures pre-migration state of the firmware on `main` so post-migration
diffs can be verified per-phase. See
`/Users/akirby/Downloads/Everytag/docs/plans/2026-04-16-refactor-ncs-3.2.4-upgrade-plan.md`.

## NCS 2.9.2 baseline (`main` @ bcfddc9, captured 2026-04-16)

Captured by Phase 0 implementor on host: `aarch64-darwin` (macOS).

| File | Contents | Source command |
|---|---|---|
| `sizes-2.9.2.log` | All 6 boards built via `nix build .#firmware`; full nix-build log including per-board text/data/bss memory regions and final symlinkJoin of all artifacts | `nix build .#firmware -L 2>&1 \| tee` |
| `config-nrf52810-2.9.2` | Resolved Kconfig for `kkm_p1_nrf52810` (default custom 52810 board, `BOARD_ROOT=$(pwd)`, no-sysbuild) | `west build --board kkm_p1_nrf52810 -d build-810 --pristine --no-sysbuild -- -DBOARD_ROOT=$(pwd)` then `cp build-810/zephyr/.config` |
| `config-nrf52832dk-2.9.2` | Resolved Kconfig for `nrf52dk/nrf52832` (Zephyr built-in DK, no-sysbuild) | `west build --board nrf52dk/nrf52832 -d build-832 --pristine --no-sysbuild` |
| `config-nrf54l15dk-2.9.2` | Resolved Kconfig for `nrf54l15dk/nrf54l15/cpuapp` (no-sysbuild) | `west build --board nrf54l15dk/nrf54l15/cpuapp -d build-54l --pristine --no-sysbuild` |
| `symbols-nrf52810-2.9.2.txt` | Top-60 largest symbols (by --size-sort) in nrf52810 ELF | `arm-none-eabi-nm --print-size --size-sort --radix=d build-810/zephyr/zephyr.elf \| tail -60` |
| `dfu-builds-2.9.2.log` | All 5 DFU sysbuild variants built via `ls -la result*` after `nix build`; proves commit 42793bf's `git init` fix is intact. Note: the plan calls the nrf52840 variant `firmware-nrf52840dk-dfu`, but the actual flake attribute is `firmware-nrf52840-dfu` (no `dk`). Plan typo, not a regression. | `nix build .#firmware-{nrf52832,nrf52833,nrf52840,nrf54l15,thingy52}-dfu -L`, then `ls -la result*` |
| `host-test-2.9.2.log` | host-native unit tests via clang/cmake — 290/290 assertions passed | `nix run .#test 2>&1 \| tee` |
| `bsim-test-2.9.2.log` | Summary doc: BabbleSim baseline (Linux-only). Local macOS run cannot evaluate `glibc-nolibgcc`. Linux CI run for the same SHA succeeded — see embedded URL. | curated; see `bsim-test-2.9.2-darwin-failure.log` for raw local failure |
| `bsim-test-2.9.2-darwin-failure.log` | Raw `nix build .#bsim-test` failure trace on aarch64-darwin (forensic) | `nix build .#bsim-test 2>&1 \| tee` |
| `ble-client-2.9.2.log` | Bumble virtual-BLE pytest run — 20/20 tests passed in 93s | `uv run --with bumble --with pytest --with pytest-asyncio pytest tests/ble_client/ -v 2>&1 \| tee` |
| `signing-key-fingerprint-2.9.2.txt` | "NO KEY FILE" — confirms pre-existing dev-key signing per plan §Phase 0 security note | `sha256sum sysbuild/mcuboot/boards/keys/*.pem 2>/dev/null \|\| echo "NO KEY FILE"` |
| `nrfutil-status-2.9.2.md` | nrfutil verification result + coordinator action item for Phase 1 | n/a — narrative document |

## Phase 0 acceptance summary

| Criterion | Result |
|---|---|
| `nix build .#firmware` (all 6 boards) | PASS |
| `nix build .#firmware-nrf52832-dfu` | PASS |
| `nix build .#firmware-nrf52833-dfu` | PASS |
| `nix build .#firmware-nrf52840-dfu` (was `-nrf52840dk-dfu` in plan; corrected) | PASS |
| `nix build .#firmware-nrf54l15-dfu` | PASS |
| `nix build .#firmware-thingy52-dfu` | PASS |
| `nix run .#test` (290 assertions) | PASS |
| `pytest tests/ble_client/` (20 tests) | PASS |
| `nix build .#bsim-test` | LOCAL N/A (Linux-only) — Linux CI for SHA = success |
| Signing key fingerprint = NO KEY FILE | PASS (expected per plan) |
| `nrfutil device --version` >= 2.8.8 | MISSING — see `nrfutil-status-2.9.2.md` (deferred to Phase 1 prereq per plan wording) |
| Per-board `.config` snapshots for nrf52810/nrf52832dk/nrf54l15dk | CAPTURED |
| Top-60 nrf52810 symbol sizes | CAPTURED |
