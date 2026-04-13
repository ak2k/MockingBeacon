# Build & Test Commands

## Prerequisites
Run `nix develop` to enter the dev shell with all dependencies.
Run `cd .. && west init -l Everytag && west update --narrow -o=--depth=1` to fetch Zephyr + NCS modules.

## Cross-compile check (verify firmware compiles for real boards)
west build --board kkm_p1_nrf52810 -d build-810 --pristine --no-sysbuild -- -DBOARD_ROOT=$(pwd)

## Host-native tests (macOS/Linux, ASan/UBSan, no Zephyr dependency)
cd tests/host && cmake -B build && cmake --build build && ./build/host_tests

## C++ quality checks
clang-format --dry-run --Werror src/*.cpp src/*.hpp
clang-tidy src/*.cpp -p tests/host/build

## C baseline binary size (nrf52810)
# text=151968, data=2872, bss=19930 (with empty C++ stubs)
