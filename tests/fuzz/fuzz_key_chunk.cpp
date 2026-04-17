// Fuzz target: validate_key_chunk — validates 14-byte half-key writes with
// bounds checking on keys_received counter. Catches: off-by-one in max-keys
// check, buffer handling with unusual lengths.
#include <cstdint>

#include "beacon_config.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Normal case: authorized, reasonable keys_received
    beacon::validate_key_chunk(data, size, true, 0, 40);
    beacon::validate_key_chunk(data, size, true, 79, 40);  // at boundary
    beacon::validate_key_chunk(data, size, true, 80, 40);  // over boundary

    // Unauthorized
    beacon::validate_key_chunk(data, size, false, 0, 40);

    // Edge: max_keys = 0
    beacon::validate_key_chunk(data, size, true, 0, 0);

    return 0;
}
