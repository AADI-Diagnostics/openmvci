#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mvci/platform/transport.hpp"

struct libusb_context;
struct libusb_device_handle;

namespace mvci {

class UsbVciInterface final : public ITransport {
public:
  UsbVciInterface();
  ~UsbVciInterface() override;

  Status open(const std::string& deviceName) override;
  void close() override;
  Status write(const std::vector<std::uint8_t>& packet) override;
  Status read(std::vector<std::uint8_t>& packet, std::uint32_t timeoutMs) override;
  Status controlTransfer(std::uint8_t requestType,
                         std::uint8_t request,
                         std::uint16_t value,
                         std::uint16_t index,
                         std::vector<std::uint8_t>& data,
                         std::uint32_t timeoutMs) override;
  void clearRx() override;
  void clearTx() override;

private:
  struct EndpointSet {
    std::uint8_t in{0};
    std::uint8_t out{0};
    std::uint8_t interfaceNumber{0};
    std::uint8_t altSetting{0};
  };

  Status openUsb(const std::string& deviceName);
  Status discoverDevice(std::uint16_t vid, std::uint16_t pid, const std::string& serial);
  Status discoverAnyMatchingDevice();
  Status claimInterface();
  Status findEndpoints();
  Status flushInput();
  Status recoverEndpoint(std::uint8_t endpointAddress);
  Status initializeMiniVci();
  Status writeRaw(const std::vector<std::uint8_t>& bytes);
  Status readRaw(std::vector<std::uint8_t>& bytes, std::uint32_t timeoutMs);
  bool waitForMiniReply(const std::vector<std::vector<std::uint8_t>>& acceptedReplies,
                        std::uint32_t timeoutMs);
  static bool miniBootstrapEnabled();
  static bool parseDeviceString(const std::string& deviceName,
                                std::uint16_t& vid,
                                std::uint16_t& pid,
                                std::string& serial);

  libusb_context* context_{nullptr};
  libusb_device_handle* handle_{nullptr};
  EndpointSet endpoints_{};
  std::uint16_t detectedVid_{0};
  std::uint16_t detectedPid_{0};
  bool miniVciMode_{false};
  bool miniVciReady_{false};
  int activeConfiguration_{-1};
  std::vector<std::uint8_t> rxBuffer_;
};

std::unique_ptr<ITransport> createUsbVciTransport();

} // namespace mvci