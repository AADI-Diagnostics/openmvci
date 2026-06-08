#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mvci/j2534.hpp"

namespace mvci {

class ITransport {
public:
  virtual ~ITransport() = default;

  virtual Status open(const std::string& deviceName) = 0;
  virtual void close() = 0;
  virtual Status write(const std::vector<std::uint8_t>& packet) = 0;
  virtual Status read(std::vector<std::uint8_t>& packet, std::uint32_t timeoutMs) = 0;
  virtual Status controlTransfer(std::uint8_t requestType,
                                 std::uint8_t request,
                                 std::uint16_t value,
                                 std::uint16_t index,
                                 std::vector<std::uint8_t>& data,
                                 std::uint32_t timeoutMs) = 0;
  virtual void clearRx() = 0;
  virtual void clearTx() = 0;
};

std::unique_ptr<ITransport> createPlatformTransport();

} // namespace mvci