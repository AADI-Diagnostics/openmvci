#include "mvci/platform/transport.hpp"
#include "mvci/platform/serial_transport.hpp"
#include "mvci/platform/usb_vci.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace mvci {
namespace {

bool macOsVerboseEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_VERBOSE_USB");
    if (value && value[0] && value[0] != '0') return true;
    const char* alt = std::getenv("MVCI_VERBOSE_SERIAL");
    return alt && alt[0] && alt[0] != '0';
  }();
  return enabled;
}

void macLog(const char* msg) {
  if (!macOsVerboseEnabled()) return;
  std::fprintf(stderr, "[mvci macos] %s\n", msg);
}

std::vector<std::string> findUsbSerialNodes() {
  std::vector<std::string> nodes;
  DIR* dir = ::opendir("/dev");
  if (!dir) return nodes;
  while (auto* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name.rfind("cu.usbserial", 0) == 0 ||
        name.rfind("tty.usbserial", 0) == 0 ||
        name.rfind("cu.usbmodem", 0) == 0 ||
        name.rfind("tty.usbmodem", 0) == 0) {
      nodes.push_back("/dev/" + name);
    }
  }
  ::closedir(dir);

  // Prefer usbserial names before usbmodem, then tty before cu for stable probing.
  std::sort(nodes.begin(), nodes.end(), [](const std::string& a, const std::string& b) {
    auto rank = [](const std::string& p) {
      int r = 0;
      if (p.find("usbmodem") != std::string::npos) r += 10;
      if (p.find("/dev/cu.") != std::string::npos) r += 1;
      return r;
    };
    const int ra = rank(a);
    const int rb = rank(b);
    if (ra != rb) return ra < rb;
    return a < b;
  });

  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  return nodes;
}

class MacOsDispatchTransport final : public ITransport {
public:
  Status open(const std::string& deviceName) override {
    close();

    if (deviceName == "loopback") {
      inner_ = createUsbVciTransport();
      return inner_ ? inner_->open(deviceName) : ERR_FAILED;
    }

    const bool explicitSerial = !deviceName.empty() &&
                                (deviceName.rfind("serial:", 0) == 0 || deviceName[0] == '/');

    if (explicitSerial) {
      inner_ = createSerialTransport();
      return inner_ ? inner_->open(deviceName) : ERR_FAILED;
    }

    if (deviceName.empty()) {
      const auto nodes = findUsbSerialNodes();
      for (const auto& node : nodes) {
        macLog(("trying serial node " + node).c_str());
        auto serial = createSerialTransport();
        if (serial && serial->open(node) == STATUS_NOERROR) {
          inner_ = std::move(serial);
          return STATUS_NOERROR;
        }
      }
      macLog("no usable /dev/{tty,cu}.usb{serial,modem}-* node; falling back to libusb path");
    }

    inner_ = createUsbVciTransport();
    return inner_ ? inner_->open(deviceName) : ERR_FAILED;
  }

  void close() override {
    if (inner_) inner_->close();
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

  Status miniRawRequest(const std::vector<std::uint8_t>& request,
                        std::vector<std::uint8_t>& response,
                        std::uint32_t timeoutMs) override {
    return inner_ ? inner_->miniRawRequest(request, response, timeoutMs) : ERR_NOT_INITIALIZED;
  }

  void clearRx() override { if (inner_) inner_->clearRx(); }
  void clearTx() override { if (inner_) inner_->clearTx(); }

private:
  std::unique_ptr<ITransport> inner_;
};

} // namespace

std::unique_ptr<ITransport> createPlatformTransport() {
  return std::make_unique<MacOsDispatchTransport>();
}

} // namespace mvci