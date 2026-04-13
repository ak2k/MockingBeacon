/* C API wrappers for beacon_logic C++ functions */
#include "beacon_logic.hpp"

extern "C" {

void beacon_derive_mac(const uint8_t *key, uint8_t *mac_out)
{
    beacon::derive_mac_from_key(key, mac_out);
}

void beacon_fill_template(const uint8_t *key, uint8_t *tmpl, size_t tmpl_size)
{
    beacon::fill_adv_template(key, tmpl, tmpl_size);
}

} /* extern "C" */
