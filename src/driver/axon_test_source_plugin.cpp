// Registration shim for the Intan RHD2132 peripheral plugin.
//
// SCIFI_REGISTER_PERIPHERAL wires our class type to the SDK's standard
// make_peripheral<T> factory (which handles null-checks + safe_make for us)
// and emits the scifi_plugin_entry symbol the host dlsym's. There's no
// factory function to write here.

#include <cstddef>

#include "scifi-peripheral-sdk/plugin.h"

#include "intan_rhd2132_peripheral.h"

SCIFI_REGISTER_PERIPHERAL(
    intan_rhd2132::IntanRhd2132Peripheral,                  // Class you implement
    "intan_rhd2132",                                        // Name shown in device logs
    "0.1.0",                                                // Plugin .deb package version
    static_cast<uint32_t>(0xF001));  // Peripheral ID (user-assignable window 0xF001..0xFFFE)
