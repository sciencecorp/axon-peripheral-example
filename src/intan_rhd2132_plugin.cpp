// Registration shim for the Intan RHD2132 peripheral plugin.
//
// SCIFI_REGISTER_PERIPHERAL wires our class type to the SDK's standard
// make_peripheral<T> factory (which handles null-checks + safe_make for us)
// and emits the scifi_plugin_entry symbol the host dlsym's. There's no
// factory function to write here.

#include <cstddef>

#include "scifi-peripheral-sdk/plugin.h"

#include "intan_rhd2132_peripheral.h"

namespace {
constexpr uint32_t kPeripheralIds[] = {
    static_cast<uint32_t>(axon::PeripheralId::SCI_INTAN_RHD2132),
};
}  // namespace

SCIFI_REGISTER_PERIPHERAL(
    chips::intan_rhd2132::IntanRhd2132Peripheral,
    "intan_rhd2132",
    "0.1.0",
    kPeripheralIds);
