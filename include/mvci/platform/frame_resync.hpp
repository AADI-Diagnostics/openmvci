#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mvci {

constexpr std::uint32_t kMvciPacketMagic = 0x4D564349U;
constexpr std::uint32_t kMvciMaxPayloadSize = 1U << 20U;

// Attempts to extract one full MVCI-framed packet from a byte stream buffer.
// Returns true when a complete packet is produced and removed from the buffer.
inline bool tryExtractMvcIFrame(std::vector<std::uint8_t>& buffer,
                                std::vector<std::uint8_t>& packet,
                                std::uint32_t maxPayloadSize = kMvciMaxPayloadSize) {
  while (buffer.size() >= 4U) {
    std::size_t magicPos = buffer.size();
    for (std::size_t i = 0; i + 3U < buffer.size(); ++i) {
      const std::uint32_t value = static_cast<std::uint32_t>(buffer[i]) |
                                  (static_cast<std::uint32_t>(buffer[i + 1U]) << 8U) |
                                  (static_cast<std::uint32_t>(buffer[i + 2U]) << 16U) |
                                  (static_cast<std::uint32_t>(buffer[i + 3U]) << 24U);
      if (value == kMvciPacketMagic) {
        magicPos = i;
        break;
      }
    }

    if (magicPos == buffer.size()) {
      // Preserve up to three trailing bytes in case packet magic straddles reads.
      if (buffer.size() > 3U) {
        buffer.erase(buffer.begin(), buffer.end() - 3);
      }
      return false;
    }

    if (magicPos > 0U) {
      buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(magicPos));
    }

    if (buffer.size() < 24U) {
      return false;
    }

    const std::uint32_t payloadSize = static_cast<std::uint32_t>(buffer[20]) |
                                      (static_cast<std::uint32_t>(buffer[21]) << 8U) |
                                      (static_cast<std::uint32_t>(buffer[22]) << 16U) |
                                      (static_cast<std::uint32_t>(buffer[23]) << 24U);
    if (payloadSize > maxPayloadSize) {
      // Corrupt header; shift by one byte and continue scanning.
      buffer.erase(buffer.begin());
      continue;
    }

    const std::size_t totalSize = 24U + static_cast<std::size_t>(payloadSize);
    if (buffer.size() < totalSize) {
      return false;
    }

    packet.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(totalSize));
    buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(totalSize));
    return true;
  }

  return false;
}

} // namespace mvci
