#pragma once

#include <cstdint>
#include <vector>

#include "mvci/j2534.hpp"

namespace mvci {

struct PacketFrame {
  std::uint32_t channel{0};
  std::uint32_t protocolId{0};
  std::uint32_t flags{0};
  std::uint32_t timestamp{0};
  std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t> encodePacket(const PassThruMsg& msg, std::uint32_t channelId);
Status decodePacket(const std::vector<std::uint8_t>& bytes, PacketFrame& frame);

} // namespace mvci