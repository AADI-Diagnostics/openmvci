#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mvci/api.hpp"

namespace mvci {

struct DtcRecord {
  std::uint32_t code{0};
  std::uint8_t status{0};
  std::uint32_t ecuAddress{0};  // Responding ECU CAN ID (e.g. 0x7E8 for ECM) when using ISO15765 OBD; 0 if unknown or not applicable.
};

MVCI_CPP_API std::vector<std::uint8_t> buildReadDtcRequest(std::uint8_t statusMask);
MVCI_CPP_API std::vector<std::uint8_t> buildClearDtcRequest();
MVCI_CPP_API std::vector<std::uint8_t> buildReadVinRequest();

MVCI_CPP_API Status sendUdsRequest(ChannelHandle channelId,
                                   const std::vector<std::uint8_t>& request,
                                   std::vector<std::vector<std::uint8_t>>& responses,
                                   std::uint32_t timeoutMs);

MVCI_CPP_API Status parseActiveDtcResponses(const std::vector<std::vector<std::uint8_t>>& responses,
                                            std::vector<DtcRecord>& dtcs,
                                            std::uint8_t statusMask);

MVCI_CPP_API Status readActiveDtcs(ChannelHandle channelId,
                                   std::vector<DtcRecord>& dtcs,
                                   std::uint32_t timeoutMs,
                                   std::uint8_t statusMask);

MVCI_CPP_API Status parseVinResponses(const std::vector<std::vector<std::uint8_t>>& responses,
                                      std::string& vin);

MVCI_CPP_API Status readVehicleVin(ChannelHandle channelId,
                                   std::string& vin,
                                   std::uint32_t timeoutMs);

MVCI_CPP_API Status clearDtcs(ChannelHandle channelId, std::uint32_t timeoutMs);

MVCI_CPP_API std::vector<std::uint8_t> stripCanIdPrefix(const std::vector<std::uint8_t>& frame);

MVCI_CPP_API std::string formatDtc(std::uint32_t code);
MVCI_CPP_API std::string statusToString(Status status);
MVCI_CPP_API std::string formatFrame(const std::vector<std::uint8_t>& frame);

} // namespace mvci