// beacon_nvs_ids.hpp — NVS/ZMS field IDs (C/C++ compatible)
//
// Single source of truth for the persistent settings layout. The ZMS
// backend stores each setting under a uint16_t key; these are those keys.
//
// X-macro form: `BEACON_NVS_IDS(X)` expands to one X(name, val) per ID.
// Adding a new ID here automatically updates the enum used by production
// code AND the iteration array used by tests/zms_persist/, so they can
// never drift apart.
//
// Greenfield: no deployed devices, so IDs may be renumbered freely.
#ifndef BEACON_NVS_IDS_HPP
#define BEACON_NVS_IDS_HPP

#include <stdint.h>

#define BEACON_NVS_IDS(X)                                                                          \
    X(fmdn_NVS, 0x01)                                                                              \
    X(airtag_NVS, 0x02)                                                                            \
    X(period_NVS, 0x03)                                                                            \
    X(changeInterval_NVS, 0x04)                                                                    \
    X(key_NVS, 0x05)                                                                               \
    X(auth_NVS, 0x06)                                                                              \
    X(power_NVS, 0x07)                                                                             \
    X(fmdnKey_NVS, 0x08)                                                                           \
    X(timeOffset_NVS, 0x09)                                                                        \
    X(settingsMAC_NVS, 0x0a)                                                                       \
    X(status_NVS, 0x0b)                                                                            \
    X(accel_NVS, 0x0c)                                                                             \
    X(turnedOn_NVS, 0x0d)

#ifdef __cplusplus
namespace beacon {
enum NvsFieldId : uint16_t {
#define X(name, val) ID_##name = val,
    BEACON_NVS_IDS(X)
#undef X
};
} // namespace beacon
#else
enum {
#define X(name, val) ID_##name = val,
    BEACON_NVS_IDS(X)
#undef X
};
#endif

#endif // BEACON_NVS_IDS_HPP
