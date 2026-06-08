#include "mvci/platform/usb_vci.hpp"

#include "mvci/platform/frame_resync.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string_view>

#include <libusb.h>

namespace mvci {
namespace {

constexpr std::uint16_t kDefaultVid = 0x0403U;
constexpr std::uint16_t kDefaultPid = 0x6001U;
constexpr std::uint16_t kFtdiVid = 0x0403U;
constexpr std::uint32_t kUsbTimeoutMs = 1000U;
constexpr std::string_view kKeywordMvcI = "mvci";
constexpr std::string_view kKeywordToyota = "toyota";
constexpr std::string_view kKeywordTechstream = "techstream";

bool verboseUsbEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_VERBOSE_USB");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

template <typename... Args>
void usbLog(const char* fmt, Args... args) {
  if (!verboseUsbEnabled()) {
    return;
  }
  std::fprintf(stderr, "[mvci usb] ");
  std::fprintf(stderr, fmt, args...);
  std::fprintf(stderr, "\n");
}

void usbLogPlain(const char* msg) {
  if (!verboseUsbEnabled()) {
    return;
  }
  std::fprintf(stderr, "[mvci usb] %s\n", msg);
}

struct VidPid {
  std::uint16_t vid;
  std::uint16_t pid;
};

constexpr std::initializer_list<VidPid> kKnownToyotaMvcis = {
    {0x0403U, 0x6001U}, // Common FTDI-based Mini-VCI clones.
    {0x0403U, 0x6010U},
    {0x0584U, 0xB020U},
    {0x0E8DU, 0x0003U},
};

std::string toLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return text;
}

bool containsKeyword(const std::string& text) {
  const auto lowered = toLower(text);
  return lowered.find(kKeywordMvcI) != std::string::npos ||
         lowered.find(kKeywordToyota) != std::string::npos ||
         lowered.find(kKeywordTechstream) != std::string::npos;
}

std::uint16_t parseHex16(const std::string& text) {
  return static_cast<std::uint16_t>(std::stoul(text, nullptr, 16));
}

bool isMiniVciCandidate(std::uint16_t vid, std::uint16_t pid) {
  if (vid != 0x0403U) {
    return false;
  }
  return pid == 0x6001U || pid == 0x6010U;
}

bool miniBootstrapStrictEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_BOOTSTRAP_STRICT");
    if (!value || value[0] == '\0') {
      return false;
    }
    return value[0] != '0';
  }();
  return enabled;
}

std::uint32_t nowMs() {
  return static_cast<std::uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::string hexString(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2U);
  for (const auto b : bytes) {
    out.push_back(kHex[(b >> 4U) & 0x0FU]);
    out.push_back(kHex[b & 0x0FU]);
  }
  return out;
}

bool containsSequence(const std::vector<std::uint8_t>& haystack,
                      const std::vector<std::uint8_t>& needle) {
  if (needle.empty() || haystack.size() < needle.size()) {
    return false;
  }
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

} // namespace

UsbVciInterface::UsbVciInterface() = default;

UsbVciInterface::~UsbVciInterface() {
  close();
}

bool UsbVciInterface::parseDeviceString(const std::string& deviceName,
                                        std::uint16_t& vid,
                                        std::uint16_t& pid,
                                        std::string& serial) {
  serial.clear();
  if (deviceName.empty()) {
    return false;
  }

  const auto firstSep = deviceName.find(':');
  if (firstSep == std::string::npos) {
    return false;
  }

  const auto secondSep = deviceName.find(':', firstSep + 1);
  try {
    vid = parseHex16(deviceName.substr(0, firstSep));
    if (secondSep == std::string::npos) {
      pid = parseHex16(deviceName.substr(firstSep + 1));
      return true;
    }

    pid = parseHex16(deviceName.substr(firstSep + 1, secondSep - firstSep - 1));
    serial = deviceName.substr(secondSep + 1);
    return true;
  } catch (...) {
    return false;
  }
}

bool UsbVciInterface::miniBootstrapEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_BOOTSTRAP");
    if (!value || value[0] == '\0') {
      return true;
    }
    return value[0] != '0';
  }();
  return enabled;
}

Status UsbVciInterface::open(const std::string& deviceName) {
  close();

  return openUsb(deviceName);
}

Status UsbVciInterface::openUsb(const std::string& deviceName) {
  if (libusb_init(&context_) != LIBUSB_SUCCESS) {
    usbLogPlain("libusb_init failed");
    return ERR_FAILED;
  }

  libusb_set_option(context_, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);

  std::uint16_t vid = kDefaultVid;
  std::uint16_t pid = kDefaultPid;
  std::string serial;
  const bool explicitMatch = parseDeviceString(deviceName, vid, pid, serial);
  usbLog("openUsb selector='%s' explicit=%d target=%04x:%04x serial='%s'",
         deviceName.c_str(), explicitMatch ? 1 : 0, vid, pid, serial.c_str());

  if (explicitMatch) {
    const auto status = discoverDevice(vid, pid, serial);
    usbLog("discoverDevice(explicit %04x:%04x) -> %d", vid, pid, static_cast<int>(status));
    if (status == STATUS_NOERROR) {
      return claimInterface();
    }
    close();
    return status;
  }

  const auto exactStatus = discoverDevice(vid, pid, serial);
  usbLog("discoverDevice(default %04x:%04x) -> %d", vid, pid, static_cast<int>(exactStatus));
  if (exactStatus == STATUS_NOERROR) {
    return claimInterface();
  }

  for (const auto& candidate : kKnownToyotaMvcis) {
    if (candidate.vid == vid && candidate.pid == pid) {
      continue;
    }

    const auto knownStatus = discoverDevice(candidate.vid, candidate.pid, serial);
    usbLog("discoverDevice(known %04x:%04x) -> %d", candidate.vid, candidate.pid,
           static_cast<int>(knownStatus));
    if (knownStatus == STATUS_NOERROR) {
      return claimInterface();
    }
  }

  const auto scanStatus = discoverAnyMatchingDevice();
  usbLog("discoverAnyMatchingDevice -> %d", static_cast<int>(scanStatus));
  if (scanStatus != STATUS_NOERROR) {
    close();
    return scanStatus;
  }

  return claimInterface();
}

Status UsbVciInterface::discoverDevice(std::uint16_t vid, std::uint16_t pid, const std::string& serial) {
  libusb_device** list = nullptr;
  const auto count = libusb_get_device_list(context_, &list);
  if (count < 0) {
    return ERR_FAILED;
  }

  Status result = ERR_FAILED;
  for (ssize_t index = 0; index < count; ++index) {
    libusb_device* device = list[index];
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(device, &descriptor) != LIBUSB_SUCCESS) {
      continue;
    }

    if (descriptor.idVendor != vid || descriptor.idProduct != pid) {
      continue;
    }

    libusb_device_handle* candidateHandle = nullptr;
    if (libusb_open(device, &candidateHandle) != LIBUSB_SUCCESS) {
      continue;
    }

    if (!serial.empty()) {
      unsigned char serialBuffer[256]{};
      if (libusb_get_string_descriptor_ascii(candidateHandle, descriptor.iSerialNumber, serialBuffer, sizeof(serialBuffer)) <= 0 ||
          serial != reinterpret_cast<char*>(serialBuffer)) {
        libusb_close(candidateHandle);
        continue;
      }
    }

    handle_ = candidateHandle;
    detectedVid_ = descriptor.idVendor;
    detectedPid_ = descriptor.idProduct;
    result = findEndpoints();
    if (result == STATUS_NOERROR) {
      break;
    }

    libusb_close(handle_);
    handle_ = nullptr;
  }

  libusb_free_device_list(list, 1);
  return result;
}

Status UsbVciInterface::discoverAnyMatchingDevice() {
  libusb_device** list = nullptr;
  const auto count = libusb_get_device_list(context_, &list);
  if (count < 0) {
    return ERR_FAILED;
  }

  Status result = ERR_FAILED;
  for (ssize_t index = 0; index < count; ++index) {
    libusb_device* device = list[index];
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(device, &descriptor) != LIBUSB_SUCCESS) {
      continue;
    }

    libusb_device_handle* candidateHandle = nullptr;
    if (libusb_open(device, &candidateHandle) != LIBUSB_SUCCESS) {
      continue;
    }

    unsigned char textBuffer[256]{};
    bool matched = false;
    if (descriptor.iManufacturer > 0 &&
        libusb_get_string_descriptor_ascii(candidateHandle, descriptor.iManufacturer, textBuffer, sizeof(textBuffer)) > 0 &&
        containsKeyword(reinterpret_cast<char*>(textBuffer))) {
      matched = true;
    }
    if (!matched && descriptor.iProduct > 0 &&
        libusb_get_string_descriptor_ascii(candidateHandle, descriptor.iProduct, textBuffer, sizeof(textBuffer)) > 0 &&
        containsKeyword(reinterpret_cast<char*>(textBuffer))) {
      matched = true;
    }

    if (!matched) {
      libusb_close(candidateHandle);
      continue;
    }

    handle_ = candidateHandle;
    detectedVid_ = descriptor.idVendor;
    detectedPid_ = descriptor.idProduct;
    result = findEndpoints();
    if (result == STATUS_NOERROR) {
      break;
    }

    libusb_close(handle_);
    handle_ = nullptr;
  }

  libusb_free_device_list(list, 1);
  return result;
}

Status UsbVciInterface::findEndpoints() {
  if (!handle_) {
    return ERR_FAILED;
  }

  libusb_device* device = libusb_get_device(handle_);
  libusb_config_descriptor* config = nullptr;
  if (libusb_get_active_config_descriptor(device, &config) != LIBUSB_SUCCESS) {
    const auto descStatus = libusb_get_config_descriptor(device, 0, &config);
    if (descStatus != LIBUSB_SUCCESS) {
      return ERR_FAILED;
    }
  }

  Status result = ERR_FAILED;
  for (int interfaceIndex = 0; interfaceIndex < config->bNumInterfaces && result != STATUS_NOERROR; ++interfaceIndex) {
    const auto& interface = config->interface[interfaceIndex];
    for (int altIndex = 0; altIndex < interface.num_altsetting; ++altIndex) {
      const auto& alt = interface.altsetting[altIndex];
      std::uint8_t inEndpoint = 0;
      std::uint8_t outEndpoint = 0;
      for (int endpointIndex = 0; endpointIndex < alt.bNumEndpoints; ++endpointIndex) {
        const auto& endpoint = alt.endpoint[endpointIndex];
        if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN &&
            (endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
          inEndpoint = endpoint.bEndpointAddress;
        } else if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT &&
                   (endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
          outEndpoint = endpoint.bEndpointAddress;
        }
      }

      if (inEndpoint != 0 && outEndpoint != 0) {
        endpoints_.in = inEndpoint;
        endpoints_.out = outEndpoint;
        endpoints_.interfaceNumber = alt.bInterfaceNumber;
        endpoints_.altSetting = alt.bAlternateSetting;
        result = STATUS_NOERROR;
        break;
      }
    }
  }

  libusb_free_config_descriptor(config);
  return result;
}

Status UsbVciInterface::claimInterface() {
  if (!handle_) {
    return ERR_FAILED;
  }

  const int kernelActive = libusb_kernel_driver_active(handle_, endpoints_.interfaceNumber);
  if (kernelActive == 1) {
    const int detachResult = libusb_detach_kernel_driver(handle_, endpoints_.interfaceNumber);
    usbLog("kernel driver active on interface %u, detach result=%d (%s)",
           endpoints_.interfaceNumber, detachResult,
           detachResult == LIBUSB_SUCCESS ? "ok" : libusb_error_name(detachResult));

    if (detachResult != LIBUSB_SUCCESS) {
      libusb_device* device = libusb_get_device(handle_);
      libusb_device_descriptor descriptor{};
      const bool isFtdi = device && libusb_get_device_descriptor(device, &descriptor) == LIBUSB_SUCCESS &&
                          descriptor.idVendor == kFtdiVid;
#if defined(__APPLE__)
      if (isFtdi) {
        std::fprintf(stderr,
                     "[mvci usb] FTDI adapter %04x:%04x is owned by Apple's AppleUSBFTDI DriverKit extension and cannot be detached (%s).\n"
                     "[mvci usb] On modern macOS (Big Sur+, Apple Silicon), this dext is Apple-signed and cannot be unloaded on a stock system.\n"
                     "[mvci usb] The adapter is exposed as a serial node instead, typically /dev/cu.usbserial-<SERIAL>.\n"
                     "[mvci usb] Use the serial transport (see docs) rather than the raw USB path for FTDI-based VCIs on macOS.\n",
                     descriptor.idVendor, descriptor.idProduct,
                     libusb_error_name(detachResult));
      } else
#else
      (void)isFtdi;
#endif
      {
        std::fprintf(stderr,
                     "[mvci usb] Kernel driver holds the adapter and cannot be detached (%s).\n",
                     libusb_error_name(detachResult));
      }
      return ERR_FAILED;
    }
  }

  if (libusb_set_auto_detach_kernel_driver(handle_, 1) != LIBUSB_SUCCESS) {
    // ignore on platforms that do not support auto-detach
  }

  if (libusb_set_configuration(handle_, 1) == LIBUSB_SUCCESS) {
    activeConfiguration_ = 1;
  }

  const int claimResult = libusb_claim_interface(handle_, endpoints_.interfaceNumber);
  if (claimResult != LIBUSB_SUCCESS) {
    usbLog("libusb_claim_interface failed on interface %u: %s",
           endpoints_.interfaceNumber, libusb_error_name(claimResult));

    libusb_device* device = libusb_get_device(handle_);
    libusb_device_descriptor descriptor{};
    const bool isFtdi = device && libusb_get_device_descriptor(device, &descriptor) == LIBUSB_SUCCESS &&
                        descriptor.idVendor == kFtdiVid;
#if defined(__APPLE__)
    if (isFtdi && claimResult == LIBUSB_ERROR_ACCESS) {
      std::fprintf(stderr,
                   "[mvci usb] FTDI adapter detected but the macOS AppleUSBFTDI driver is holding it.\n"
                   "[mvci usb] Unload it once per boot with:\n"
                   "[mvci usb]   sudo kextunload -b com.apple.driver.AppleUSBFTDI\n"
                   "[mvci usb] (On SIP-enabled systems, also: sudo kextunload -b com.apple.driver.AppleUSBFTDI -v)\n");
    }
#else
    (void)isFtdi;
#endif
    return ERR_FAILED;
  }

  if (endpoints_.altSetting != 0) {
    (void)libusb_set_interface_alt_setting(handle_, endpoints_.interfaceNumber, endpoints_.altSetting);
  }

  (void)flushInput();

  miniVciMode_ = isMiniVciCandidate(detectedVid_, detectedPid_);
  miniVciReady_ = !miniVciMode_;
  if (miniVciMode_ && miniBootstrapEnabled()) {
    const auto initStatus = initializeMiniVci();
    if (initStatus != STATUS_NOERROR) {
      miniVciReady_ = false;
      if (miniBootstrapStrictEnabled()) {
        usbLogPlain("Mini-VCI bootstrap failed; rejecting adapter in strict mode");
        return ERR_FAILED;
      }
      usbLogPlain("Mini-VCI bootstrap did not complete; continuing in non-bootstrapped mode");
    }
  }

  return STATUS_NOERROR;
}

Status UsbVciInterface::writeRaw(const std::vector<std::uint8_t>& bytes) {
  if (!handle_ || endpoints_.out == 0) {
    return ERR_NOT_INITIALIZED;
  }

  int transferred = 0;
  const auto status = libusb_bulk_transfer(handle_, endpoints_.out,
                                           const_cast<unsigned char*>(bytes.data()),
                                           static_cast<int>(bytes.size()),
                                           &transferred,
                                           static_cast<unsigned int>(kUsbTimeoutMs));
  if (status == LIBUSB_ERROR_PIPE) {
    (void)recoverEndpoint(endpoints_.out);
    return ERR_FAILED;
  }
  if (status == LIBUSB_ERROR_NO_DEVICE) {
    return ERR_FAILED;
  }
  if (status != LIBUSB_SUCCESS || transferred != static_cast<int>(bytes.size())) {
    return ERR_FAILED;
  }
  return STATUS_NOERROR;
}

Status UsbVciInterface::readRaw(std::vector<std::uint8_t>& bytes, std::uint32_t timeoutMs) {
  bytes.clear();
  if (!handle_ || endpoints_.in == 0) {
    return ERR_NOT_INITIALIZED;
  }

  unsigned char buffer[512]{};
  int transferred = 0;
  const auto status = libusb_bulk_transfer(handle_, endpoints_.in, buffer, sizeof(buffer), &transferred, timeoutMs);
  if (status == LIBUSB_ERROR_TIMEOUT) {
    return ERR_TIMEOUT;
  }
  if (status == LIBUSB_ERROR_PIPE) {
    (void)recoverEndpoint(endpoints_.in);
    return ERR_FAILED;
  }
  if (status == LIBUSB_ERROR_NO_DEVICE || status != LIBUSB_SUCCESS) {
    return ERR_FAILED;
  }
  if (transferred <= 0) {
    return ERR_TIMEOUT;
  }

  bytes.assign(buffer, buffer + transferred);
  return STATUS_NOERROR;
}

bool UsbVciInterface::waitForMiniReply(const std::vector<std::vector<std::uint8_t>>& acceptedReplies,
                                       std::uint32_t timeoutMs) {
  const auto start = nowMs();
  std::vector<std::uint8_t> stream;
  while (nowMs() - start < timeoutMs) {
    std::vector<std::uint8_t> incoming;
    const auto status = readRaw(incoming, 50);
    if (status == ERR_TIMEOUT) {
      continue;
    }
    if (status != STATUS_NOERROR) {
      return false;
    }

    stream.insert(stream.end(), incoming.begin(), incoming.end());
    if (stream.size() > 4096U) {
      stream.erase(stream.begin(), stream.end() - 1024);
    }

    for (const auto& expected : acceptedReplies) {
      if (incoming == expected || containsSequence(stream, expected)) {
        usbLog("mini reply matched: %s", hexString(incoming).c_str());
        return true;
      }
    }
  }
  return false;
}

Status UsbVciInterface::initializeMiniVci() {
  usbLog("mini bootstrap start for %04x:%04x", detectedVid_, detectedPid_);

  const std::vector<std::uint8_t> start1{0x03, 0x00, 0x03};
  const std::vector<std::uint8_t> start2{0x0c, 0x00, 0x07, 0x00, 0x01, 0x4d, 0x56, 0x43, 0x49, 0x2d, 0x54, 0x62};
  const std::vector<std::uint8_t> start3{0x13, 0x00, 0xd0, 0x4d, 0x01, 0xf7, 0x76, 0x39, 0x07, 0x6b,
                                          0x27, 0x40, 0xea, 0x48, 0xfd, 0x6e, 0xa4, 0xa9, 0x00};

  const std::vector<std::uint8_t> ack1{0x01, 0x60};
  const std::vector<std::uint8_t> ack2WithStatus{0x01, 0x60, 0x0e, 0x00, 0x09, 0x00, 0x01, 0xb0,
                                                 0xcb, 0x49, 0x68, 0x07, 0x45, 0xc8, 0x7f, 0xa9};
  const std::vector<std::uint8_t> ack2NoStatus{0x0e, 0x00, 0x09, 0x00, 0x01, 0xb0,
                                               0xcb, 0x49, 0x68, 0x07, 0x45, 0xc8, 0x7f, 0xa9};
  const std::vector<std::uint8_t> ack3WithStatus{0x01, 0x60, 0x0b, 0x00, 0x71, 0x08, 0x8e,
                                                 0x8d, 0x8d, 0xa6, 0xaa, 0xdf, 0x2f};
  const std::vector<std::uint8_t> ack3NoStatus{0x0b, 0x00, 0x71, 0x08, 0x8e,
                                               0x8d, 0x8d, 0xa6, 0xaa, 0xdf, 0x2f};

  for (int attempt = 1; attempt <= 3; ++attempt) {
    (void)flushInput();

    if (writeRaw(start1) != STATUS_NOERROR || !waitForMiniReply({ack1}, 500)) {
      usbLog("mini bootstrap stage1 attempt %d failed", attempt);
      continue;
    }
    if (writeRaw(start2) != STATUS_NOERROR ||
        !waitForMiniReply({ack2WithStatus, ack2NoStatus}, 900)) {
      usbLog("mini bootstrap stage2 attempt %d failed", attempt);
      continue;
    }
    if (writeRaw(start3) != STATUS_NOERROR ||
        !waitForMiniReply({ack3WithStatus, ack3NoStatus}, 900)) {
      usbLog("mini bootstrap stage3 attempt %d failed", attempt);
      continue;
    }

    miniVciReady_ = true;
    usbLog("mini bootstrap completed on attempt %d", attempt);
    return STATUS_NOERROR;
  }

  return ERR_FAILED;
}

Status UsbVciInterface::flushInput() {
  if (!handle_) {
    return STATUS_NOERROR;
  }

  unsigned char buffer[512]{};
  int transferred = 0;
  while (libusb_bulk_transfer(handle_, endpoints_.in, buffer, sizeof(buffer), &transferred, 1) == LIBUSB_SUCCESS && transferred > 0) {
    if (transferred < static_cast<int>(sizeof(buffer))) {
      break;
    }
  }

  return STATUS_NOERROR;
}

Status UsbVciInterface::recoverEndpoint(std::uint8_t endpointAddress) {
  if (!handle_) {
    return ERR_NOT_INITIALIZED;
  }

  if (libusb_clear_halt(handle_, endpointAddress) != LIBUSB_SUCCESS) {
    return ERR_FAILED;
  }

  return STATUS_NOERROR;
}

void UsbVciInterface::close() {
  if (handle_) {
    libusb_release_interface(handle_, endpoints_.interfaceNumber);
    libusb_close(handle_);
    handle_ = nullptr;
  }

  if (context_) {
    libusb_exit(context_);
    context_ = nullptr;
  }

  rxBuffer_.clear();
  endpoints_ = {};
  detectedVid_ = 0;
  detectedPid_ = 0;
  miniVciMode_ = false;
  miniVciReady_ = false;
  activeConfiguration_ = -1;
}

Status UsbVciInterface::write(const std::vector<std::uint8_t>& packet) {
  if (!handle_ || endpoints_.out == 0) {
    return ERR_NOT_INITIALIZED;
  }

  if (miniVciMode_ && !miniVciReady_ && miniBootstrapEnabled()) {
    const auto initStatus = initializeMiniVci();
    if (initStatus != STATUS_NOERROR && miniBootstrapStrictEnabled()) {
      return ERR_FAILED;
    }
  }

  return writeRaw(packet);
}

Status UsbVciInterface::read(std::vector<std::uint8_t>& packet, std::uint32_t timeoutMs) {
  if (!handle_ || endpoints_.in == 0) {
    return ERR_NOT_INITIALIZED;
  }

  while (true) {
    if (tryExtractMvcIFrame(rxBuffer_, packet)) {
      return STATUS_NOERROR;
    }

    std::vector<std::uint8_t> incoming;
    const auto status = readRaw(incoming, timeoutMs);
    if (status == ERR_TIMEOUT) {
      return ERR_TIMEOUT;
    }
    if (status != STATUS_NOERROR) {
      return status;
    }

    rxBuffer_.insert(rxBuffer_.end(), incoming.begin(), incoming.end());
  }
}

Status UsbVciInterface::controlTransfer(std::uint8_t requestType,
                                        std::uint8_t request,
                                        std::uint16_t value,
                                        std::uint16_t index,
                                        std::vector<std::uint8_t>& data,
                                        std::uint32_t timeoutMs) {
  if (!handle_) {
    return ERR_NOT_INITIALIZED;
  }

  const auto direction = static_cast<unsigned int>(requestType & LIBUSB_ENDPOINT_DIR_MASK);
  if (direction == LIBUSB_ENDPOINT_OUT) {
    const auto status = libusb_control_transfer(handle_, requestType, request, value, index,
                                                data.empty() ? nullptr : data.data(),
                                                static_cast<std::uint16_t>(data.size()),
                                                timeoutMs);
    return status < 0 ? ERR_FAILED : STATUS_NOERROR;
  }

  data.resize(256);
  const auto status = libusb_control_transfer(handle_, requestType, request, value, index,
                                              data.data(), static_cast<std::uint16_t>(data.size()),
                                              timeoutMs);
  if (status < 0) {
    data.clear();
    return ERR_FAILED;
  }

  data.resize(static_cast<std::size_t>(status));
  return STATUS_NOERROR;
}

void UsbVciInterface::clearRx() {
  rxBuffer_.clear();
  (void)flushInput();
}

void UsbVciInterface::clearTx() {
}

std::unique_ptr<ITransport> createUsbVciTransport() {
  return std::make_unique<UsbVciInterface>();
}

} // namespace mvci