// Registration shim for the Intan RHD2132 peripheral plugin.
//
// This is the only file unique to "this is a plugin .so" — everything else
// in src/ is the verbatim implementation copied from headstage. The host
// dlopens this .so, looks up scifi_plugin_entry, validates ABI version, and
// indexes the descriptor by peripheral ID.

#include <cstddef>
#include <memory>

#include "scifi-peripheral-sdk/plugin.h"
#include "scifi-peripheral-sdk/axon/protocol.h"

#include "intan_rhd2132_peripheral.h"

namespace {

constexpr uint32_t kPeripheralIds[] = {
    static_cast<uint32_t>(axon::PeripheralId::SCI_INTAN_RHD2132),
};

std::shared_ptr<axon::RecordPeripheral> make_intan_rhd2132(
    const scifi::plugin::PeripheralContext* ctx,
    const scifi::plugin::HostServices* host) {
  if (ctx == nullptr || host == nullptr || host->zmq_context == nullptr) {
    return nullptr;
  }

  // use safe_make to ensure uncaught throws result in nullptrs being returned,
  // as is technically UB across .so barrier
  return scifi::plugin::safe_make<chips::intan_rhd2132::IntanRhd2132Peripheral>(
      ctx->synapse_peripheral_id,
      ctx->hw_addr,
      *host->zmq_context,
      host->axon_tx_endpoint,
      host->axon_rx_endpoint);
}

}  // namespace

SCIFI_PLUGIN_REGISTER({
    .abi_version = scifi::plugin::ABI_VERSION,
    .name = "intan_rhd2132",
    .version = "0.1.0",
    .peripheral_ids = kPeripheralIds,
    .peripheral_ids_len = std::size(kPeripheralIds),
    .requires_pairing = false,
    .make_record = &make_intan_rhd2132,
});
