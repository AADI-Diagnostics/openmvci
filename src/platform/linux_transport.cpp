#include "mvci/platform/transport.hpp"
#include "mvci/platform/serial_transport.hpp"
#include "mvci/platform/usb_vci.hpp"

#include <dirent.h>

#include <memory>
#include <string>
#include <vector>

namespace mvci {

namespace {

std::vector<std::string> findLinuxSerialNodes() {
  std::vector<std::string> nodes;
  DIR* dir = ::opendir("/dev");
  if (!dir) {
    return nodes;
  }

  while (auto* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name.rfind("ttyUSB", 0) == 0 || name.rfind("ttyACM", 0) == 0) {
      nodes.push_back("/dev/" + name);
    }
  }
  ::closedir(dir);
  return nodes;
}

bool isMiniVciSelector(const std::string& selector) {
  if (selector.empty()) {
    return false;
  }
  if (selector.rfind("0403:6001", 0) == 0 || selector.rfind("0403:6010", 0) == 0) {
    return true;
  }
  return false;
}

class LinuxDispatchTransport final : public ITransport {
public:
  Status open(const std::string& deviceName) override {
    close();

    const bool explicitSerial = !deviceName.empty() &&
                                (deviceName.rfind("serial:", 0) == 0 || deviceName[0] == '/');

    if (explicitSerial) {
      inner_ = createSerialTransport();
      return inner_ ? inner_->open(deviceName) : ERR_FAILED;
    }

    if (deviceName.empty() || isMiniVciSelector(deviceName)) {
      const auto nodes = findLinuxSerialNodes();
      for (const auto& node : nodes) {
        auto serial = createSerialTransport();
        if (serial && serial->open(node) == STATUS_NOERROR) {
          inner_ = std::move(serial);
          return STATUS_NOERROR;
        }
      }
    }

    inner_ = createUsbVciTransport();
    return inner_ ? inner_->open(deviceName) : ERR_FAILED;
  }

  void close() override {
    if (inner_) {
      inner_->close();
    }
    inner_.reset();
  }

  Status write(const std::vector<std::uint8_t>& packet) override {
    return inner_ ? inner_->write(packet) : ERR_NOT_INITIALIZED;
  }

  Status read(std::vector<std::uint8_t>& packet, std::uint32_t timeoutMs) override {
    return inner_ ? inner_->read(packet, timeoutMs) : ERR_NOT_INITIALIZED;
  }

  Status controlTransfer(std::uint8_t requestType,
                         std::uint8_t request,
                         std::uint16_t value,
                         std::uint16_t index,
                         std::vector<std::uint8_t>& data,
                         std::uint32_t timeoutMs) override {
    return inner_ ? inner_->controlTransfer(requestType, request, value, index, data, timeoutMs)
                  : ERR_NOT_INITIALIZED;
  }

  void clearRx() override {
    if (inner_) {
      inner_->clearRx();
    }
  }

  void clearTx() override {
    if (inner_) {
      inner_->clearTx();
    }
  }

private:
  std::unique_ptr<ITransport> inner_;
};

} // namespace

std::unique_ptr<ITransport> createPlatformTransport() {
  return std::make_unique<LinuxDispatchTransport>();
}

} // namespace mvci