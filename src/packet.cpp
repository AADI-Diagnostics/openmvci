#include "mvci/packet.hpp"

#include <cstring>

namespace mvci {
namespace {

constexpr std::uint32_t kPacketMagic = 0x4D564349;

void appendU32(std::vector<std::uint8_t>& buffer, std::uint32_t value) {
  buffer.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  buffer.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  buffer.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  buffer.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

std::uint32_t readU32(const std::vector<std::uint8_t>& buffer, std::size_t offset) {
  return static_cast<std::uint32_t>(buffer[offset]) |
         (static_cast<std::uint32_t>(buffer[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(buffer[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(buffer[offset + 3]) << 24U);
}

} // namespace

std::vector<std::uint8_t> encodePacket(const PassThruMsg& msg, std::uint32_t channelId) {
  std::vector<std::uint8_t> buffer;
  buffer.reserve(24 + msg.dataSize);

  appendU32(buffer, kPacketMagic);
  appendU32(buffer, channelId);
  appendU32(buffer, msg.protocolId);
  appendU32(buffer, msg.txFlags);
  appendU32(buffer, msg.timestamp);
  appendU32(buffer, msg.dataSize);
  buffer.insert(buffer.end(), msg.data, msg.data + msg.dataSize);
  return buffer;
}

Status decodePacket(const std::vector<std::uint8_t>& bytes, PacketFrame& frame) {
  if (bytes.size() < 24) {
    return ERR_INVALID_MSG;
  }

  if (readU32(bytes, 0) != kPacketMagic) {
    return ERR_INVALID_MSG;
  }

  const auto payloadSize = readU32(bytes, 20);
  if (bytes.size() != 24 + payloadSize) {
    return ERR_INVALID_MSG;
  }

  frame.channel = readU32(bytes, 4);
  frame.protocolId = readU32(bytes, 8);
  frame.flags = readU32(bytes, 12);
  frame.timestamp = readU32(bytes, 16);
  frame.payload.assign(bytes.begin() + 24, bytes.end());
  return STATUS_NOERROR;
}

} // namespace mvci