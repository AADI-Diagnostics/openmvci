#include "mvci/platform/transport.hpp"
#include "mvci/platform/serial_transport.hpp"
#include "mvci/platform/usb_vci.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
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

// Always emit discovery progress so users running read_dtcs (or other tools)
// can see which serial node was selected without having to enable verbose.
void macDiscoveryLog(const char* msg) {
  std::fprintf(stderr, "[mvci macos] %s\n", msg);
}

// macOS does not expose /dev/{cu,tty}.usb* via readdir("/dev") -- those
// nodes are created dynamically by IOKit and only show up via stat(). The
// portable way to enumerate them is to query the IORegistry for matching
// `IOSerialBSDClient` services and read the `IOCalloutDevice` property:
std::vector<std::string> findUsbSerialNodes() {
  std::vector<std::string> nodes;

  CFMutableDictionaryRef matching = IOServiceMatching(kIOSerialBSDServiceValue);
  if (!matching) return nodes;
  // Restrict to RS-232 / USB-serial style devices (excludes Bluetooth-SPP, etc.):
  CFDictionarySetValue(matching, CFSTR(kIOSerialBSDTypeKey), CFSTR(kIOSerialBSDAllTypes));

  io_iterator_t it = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &it) != KERN_SUCCESS) {
    return nodes;
  }

  for (io_object_t svc = IOIteratorNext(it); svc; svc = IOIteratorNext(it)) {
    CFTypeRef pathRef = IORegistryEntryCreateCFProperty(
        svc, CFSTR(kIOCalloutDeviceKey), kCFAllocatorDefault, 0);
    if (pathRef && CFGetTypeID(pathRef) == CFStringGetTypeID()) {
      char buf[256] = {};
      if (CFStringGetCString(static_cast<CFStringRef>(pathRef),
                             buf, sizeof(buf), kCFStringEncodingUTF8)) {
        const std::string path = buf;
        if (path.find("/cu.usbserial") != std::string::npos ||
            path.find("/cu.usbmodem") != std::string::npos) {
          struct stat st {};
          if (::stat(path.c_str(), &st) == 0) nodes.push_back(path);
        }
      }
    }
    if (pathRef) CFRelease(pathRef);
    IOObjectRelease(svc);
  }
  IOObjectRelease(it);

  // Prefer usbserial (FTDI) over usbmodem (CDC-ACM) for the Mini-VCI.
  std::sort(nodes.begin(), nodes.end(), [](const std::string& a, const std::string& b) {
    const bool am = a.find("usbmodem") != std::string::npos;
    const bool bm = b.find("usbmodem") != std::string::npos;
    if (am != bm) return !am;
    return a < b;
  });
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  return nodes;
}

class MacOsDispatchTransport final : public ITransport {
public:
  Status open(const std::string& deviceName) override {
    close();

    const bool explicitSerial = !deviceName.empty() &&
                                (deviceName.rfind("serial:", 0) == 0 || deviceName[0] == '/');

    if (explicitSerial) {
      inner_ = createSerialTransport();
      return inner_ ? inner_->open(deviceName) : ERR_FAILED;
    }

    if (deviceName.empty()) {
      const auto nodes = findUsbSerialNodes();
      for (const auto& node : nodes) {
        macDiscoveryLog(("trying serial node " + node).c_str());
        auto serial = createSerialTransport();
        if (serial && serial->open(node) == STATUS_NOERROR) {
          macDiscoveryLog(("selected serial node " + node).c_str());
          inner_ = std::move(serial);
          return STATUS_NOERROR;
        }
        macDiscoveryLog(("serial node open failed: " + node).c_str());
      }
      macDiscoveryLog("no usable /dev/{tty,cu}.usb{serial,modem}-* node; falling back to libusb path");
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