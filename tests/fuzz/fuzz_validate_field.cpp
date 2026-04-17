// Fuzz target: validate_field — parses binary BLE write payloads into int32
// with range/allowed-value checking. Exercises all single-field GATT handlers
// (period, changeInterval, txPower, status, accel, fmdn flag, airtag flag).
#include <cstdint>
#include <cstring>

#include "beacon_config.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    int32_t out = 0;

    // Period: allowed values {1, 2, 4, 8}
    static const int32_t period_allowed[] = {1, 2, 4, 8};
    beacon::GattFieldSpec period_spec{sizeof(int32_t), 0, 0, period_allowed, 4};
    beacon::validate_field(data, size, true, period_spec, out);

    // changeInterval: range [30, 7200]
    beacon::GattFieldSpec interval_spec{sizeof(int32_t), 30, 7200, nullptr, 0};
    beacon::validate_field(data, size, true, interval_spec, out);

    // txPower: range [0, 2]
    beacon::GattFieldSpec tx_spec{sizeof(int32_t), 0, 2, nullptr, 0};
    beacon::validate_field(data, size, true, tx_spec, out);

    // accel threshold: range [0, 16383]
    beacon::GattFieldSpec accel_spec{sizeof(int32_t), 0, 16383, nullptr, 0};
    beacon::validate_field(data, size, true, accel_spec, out);

    // status: full int32 (no range constraint beyond size)
    beacon::GattFieldSpec status_spec{sizeof(int32_t), 0, 0, nullptr, 0};
    beacon::validate_field(data, size, true, status_spec, out);

    // fmdn/airtag flags: also int32
    beacon::validate_field(data, size, true, status_spec, out);

    // Unauthorized path
    beacon::validate_field(data, size, false, interval_spec, out);

    return 0;
}
