#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mvci/api.hpp"

namespace mvci {

struct DtcRecord {
  std::uint32_t code{0};
  std::uint8_t status{0};
};

std::vector<std::uint8_t> buildReadDtcRequest(std::uint8_t statusMask);
std::vector<std::uint8_t> buildClearDtcRequest();

Status sendUdsRequest(ChannelHandle channelId,
                      const std::vector<std::uint8_t>& request,
                      std::vector<std::vector<std::uint8_t>>& responses,
                      std::uint32_t timeoutMs);

Status parseActiveDtcResponses(const std::vector<std::vector<std::uint8_t>>& responses,
                               std::vector<DtcRecord>& dtcs,
                               std::uint8_t statusMask);

Status readActiveDtcs(ChannelHandle channelId,
                      std::vector<DtcRecord>& dtcs,
                      std::uint32_t timeoutMs,
                      std::uint8_t statusMask);

Status clearDtcs(ChannelHandle channelId, std::uint32_t timeoutMs);

std::string formatDtc(std::uint32_t code);
std::string statusToString(Status status);
std::string formatFrame(const std::vector<std::uint8_t>& frame);

} // namespace mvci