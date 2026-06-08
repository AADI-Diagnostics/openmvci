#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "mvci/j2534.hpp"
#include "mvci/platform/frame_resync.hpp"

namespace {

void appendU32(std::vector<std::uint8_t>& buffer, std::uint32_t value) {
  buffer.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  buffer.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  buffer.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  buffer.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

std::vector<std::uint8_t> buildPacket(std::uint32_t channel,
                                      std::uint32_t protocol,
                                      const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> packet;
  packet.reserve(24U + payload.size());
  appendU32(packet, mvci::kMvciPacketMagic);
  appendU32(packet, channel);
  appendU32(packet, protocol);
  appendU32(packet, 0U);
  appendU32(packet, 0U);
  appendU32(packet, static_cast<std::uint32_t>(payload.size()));
  packet.insert(packet.end(), payload.begin(), payload.end());
  return packet;
}

} // namespace

TEST_CASE("frame resync recovers packet after random noise prefix") {
  std::vector<std::uint8_t> buffer{0x01, 0x60, 0x0b, 0x00, 0xaa, 0xbb};
  const auto packet = buildPacket(7U, mvci::PROTOCOL_CAN, {0x10, 0x03, 0x7f});
  buffer.insert(buffer.end(), packet.begin(), packet.end());

  std::vector<std::uint8_t> out;
  CHECK(mvci::tryExtractMvcIFrame(buffer, out));
  CHECK(out == packet);
  CHECK(buffer.empty());
}

TEST_CASE("frame resync handles magic split across input chunks") {
  const auto packet = buildPacket(9U, mvci::PROTOCOL_ISO15765, {0x22, 0xf1, 0x90});
  std::vector<std::uint8_t> buffer{0xaa, 0xbb, 0xcc};
  std::vector<std::uint8_t> out;

  // Feed all but first byte of magic and verify no extraction yet.
  buffer.push_back(packet[0]);
  CHECK_FALSE(mvci::tryExtractMvcIFrame(buffer, out));

  // Remaining bytes complete the packet and extraction should succeed.
  buffer.insert(buffer.end(), packet.begin() + 1, packet.end());
  CHECK(mvci::tryExtractMvcIFrame(buffer, out));
  CHECK(out == packet);
  CHECK(buffer.empty());
}

TEST_CASE("frame resync skips corrupt oversized header and recovers next valid packet") {
  std::vector<std::uint8_t> buffer;
  // Corrupt header starts with valid magic but absurd payload size.
  appendU32(buffer, mvci::kMvciPacketMagic);
  appendU32(buffer, 1U);
  appendU32(buffer, mvci::PROTOCOL_CAN);
  appendU32(buffer, 0U);
  appendU32(buffer, 0U);
  appendU32(buffer, 0x7fffffffU);

  const auto packet = buildPacket(3U, mvci::PROTOCOL_CAN, {0x02, 0x10, 0x01});
  buffer.insert(buffer.end(), packet.begin(), packet.end());

  std::vector<std::uint8_t> out;
  CHECK(mvci::tryExtractMvcIFrame(buffer, out));
  CHECK(out == packet);
  CHECK(buffer.empty());
}
