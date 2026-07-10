// Registration shim for the axon_test_source peripheral plugin.
//
// SCIFI_REGISTER_PERIPHERAL wires our class type to the SDK's standard
// make_peripheral<T> factory (which handles null-checks + safe_make for us)
// and emits the scifi_plugin_entry symbol the host dlsym's. There's no
// factory function to write here.

#include <cstddef>

#include "axon-peripheral-driver-sdk/plugin.h"

#include "axon_test_source_peripheral.h"

SCIFI_REGISTER_PERIPHERAL(
    axon_test_source::AxonTestSourcePeripheral,             // Class you implement
    "axon_test_source",                                     // Name shown in device logs
    "0.1.0",                                                // Plugin .deb package version
    static_cast<uint32_t>(0xF001));  // Peripheral ID (user-assignable window 0xF001..0xFFFE)
