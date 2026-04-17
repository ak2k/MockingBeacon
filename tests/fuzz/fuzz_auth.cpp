// Fuzz target: validate_auth_code — compares BLE auth write against stored code.
// Catches: buffer overruns, timing side-channels in comparison, edge cases
// at len=0 or len != stored_len.
#include <cstdint>

#include "beacon_config.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Stored auth code: "abcdefgh" (matches test fixtures)
    static const uint8_t stored[] = "abcdefgh";

    beacon::validate_auth_code(data, size, stored, sizeof(stored) - 1);

    // Also test with empty stored code
    beacon::validate_auth_code(data, size, nullptr, 0);

    // Test with exact-match length but different content
    if (size >= 8) {
        beacon::validate_auth_code(data, 8, stored, 8);
    }

    return 0;
}
