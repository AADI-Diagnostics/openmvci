#include "mvci/platform/transport.hpp"

#include "mvci/platform/frame_resync.hpp"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <IOKit/serial/ioss.h>
#endif

namespace mvci {
namespace {

bool serialVerboseEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_VERBOSE_SERIAL");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

void serialLog(const char* msg) {
  if (!serialVerboseEnabled()) {
    return;
  }
  std::fprintf(stderr, "[mvci serial] %s\n", msg);
}

template <typename... Args>
void serialLog(const char* fmt, Args... args) {
  if (!serialVerboseEnabled()) {
    return;
  }
  std::fprintf(stderr, "[mvci serial] ");
  std::fprintf(stderr, fmt, args...);
  std::fprintf(stderr, "\n");
}

speed_t standardBaud(unsigned int rate) {
  switch (rate) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default:     return 0;
  }
}

unsigned int desiredBaud() {
  if (const char* env = std::getenv("MVCI_SERIAL_BAUD")) {
    char* end = nullptr;
    const auto value = std::strtoul(env, &end, 10);
    if (end != env && value > 0) {
      return static_cast<unsigned int>(value);
    }
  }
  return 500000U; // Mini-VCI default
}

bool miniBootstrapEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_BOOTSTRAP");
    if (!value || value[0] == '\0') {
      return true;
    }
    return value[0] != '0';
  }();
  return enabled;
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

bool miniBootstrapStage1StrictEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_STAGE1_STRICT");
    if (!value || value[0] == '\0') {
      return false;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool serialRtsCtsEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_SERIAL_RTSCTS");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

bool serialAssertControlLinesEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_SERIAL_ASSERT_CTRL");
    if (!value || value[0] == '\0') {
      return true;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool serialRequireRxEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_SERIAL_REQUIRE_RX");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

std::uint32_t serialRequireRxTimeoutMs() {
  static const std::uint32_t timeoutMs = []() -> std::uint32_t {
    const char* value = std::getenv("MVCI_SERIAL_REQUIRE_RX_TIMEOUT_MS");
    if (!value || value[0] == '\0') {
      return 250U;
    }
    char* end = nullptr;
    const auto parsed = std::strtoul(value, &end, 10);
    if (end == value || parsed == 0) {
      return 250U;
    }
    return static_cast<std::uint32_t>(parsed);
  }();
  return timeoutMs;
}

std::string hexString(const std::vector<std::uint8_t>& bytes, std::size_t maxBytes = 48U) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  const auto count = std::min(bytes.size(), maxBytes);
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0) {
      oss << ' ';
    }
    oss << std::setw(2) << static_cast<int>(bytes[i]);
  }
  if (bytes.size() > maxBytes) {
    oss << " ...";
  }
  return oss.str();
}

bool containsSequence(const std::vector<std::uint8_t>& haystack,
                      const std::vector<std::uint8_t>& needle) {
  if (needle.empty() || haystack.size() < needle.size()) {
    return false;
  }
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

Status configureSerialPort(int fd, unsigned int baud) {
  termios tty{};
  if (::tcgetattr(fd, &tty) != 0) {
    serialLog("tcgetattr failed: %s", std::strerror(errno));
    return ERR_FAILED;
  }

  cfmakeraw(&tty);
  tty.c_cflag |= (CLOCAL | CREAD);
  tty.c_cflag &= ~CSIZE;
  tty.c_cflag |= CS8;
  tty.c_cflag &= ~PARENB;
  tty.c_cflag &= ~CSTOPB;
  if (serialRtsCtsEnabled()) {
    tty.c_cflag |= CRTSCTS;
  } else {
    tty.c_cflag &= ~CRTSCTS;
  }
  tty.c_cflag &= ~HUPCL;
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 0;

  const speed_t standard = standardBaud(baud);
  if (standard != 0) {
    cfsetispeed(&tty, standard);
    cfsetospeed(&tty, standard);
  } else {
    cfsetispeed(&tty, B9600);
    cfsetospeed(&tty, B9600);
  }

  if (::tcsetattr(fd, TCSANOW, &tty) != 0) {
    serialLog("tcsetattr failed: %s", std::strerror(errno));
    return ERR_FAILED;
  }

#if defined(__APPLE__)
  if (standard == 0) {
    const speed_t custom = static_cast<speed_t>(baud);
    if (::ioctl(fd, IOSSIOSPEED, &custom) < 0) {
      serialLog("IOSSIOSPEED %u failed: %s", baud, std::strerror(errno));
      return ERR_FAILED;
    }
  }
#else
  if (standard == 0) {
    serialLog("non-standard baud %u not supported on this platform", baud);
  }
#endif

  ::tcflush(fd, TCIOFLUSH);
  return STATUS_NOERROR;
}

class SerialTransport final : public ITransport {
public:
  Status open(const std::string& deviceName) override {
    close();

    std::string path = deviceName;
    constexpr const char* kPrefix = "serial:";
    if (path.rfind(kPrefix, 0) == 0) {
      path.erase(0, std::strlen(kPrefix));
    }
    if (path.empty() || path[0] != '/') {
      serialLog("invalid serial path '%s'", path.c_str());
      return ERR_FAILED;
    }

    fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
      serialLog("open('%s') failed: %s", path.c_str(), std::strerror(errno));
      return ERR_FAILED;
    }

    // Clear O_NONBLOCK so subsequent reads honor VMIN/VTIME via select.
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags >= 0) {
      ::fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);
    }

    if (::ioctl(fd_, TIOCEXCL) < 0) {
      serialLog("TIOCEXCL failed on '%s': %s (continuing)", path.c_str(), std::strerror(errno));
    }

    const unsigned int configuredBaud = desiredBaud();
    if (configureSerialPort(fd_, configuredBaud) != STATUS_NOERROR) {
      closeFd();
      return ERR_FAILED;
    }

    unsigned int activeBaud = configuredBaud;
    open_ = true;
    serialLog("opened '%s' at %u baud", path.c_str(), activeBaud);

    if (serialAssertControlLinesEnabled()) {
      int modem = 0;
      if (::ioctl(fd_, TIOCMGET, &modem) == 0) {
        modem |= (TIOCM_DTR | TIOCM_RTS);
        if (::ioctl(fd_, TIOCMSET, &modem) == 0) {
          serialLog("asserted DTR/RTS");
        }
      }
    }

    if (miniBootstrapEnabled()) {
      auto bootstrap = bootstrapMiniVci();
      if (bootstrap != STATUS_NOERROR) {
        for (unsigned int fallbackBaud : {500000U, 230400U, 115200U, 38400U}) {
          if (fallbackBaud == activeBaud) {
            continue;
          }

          if (configureSerialPort(fd_, fallbackBaud) != STATUS_NOERROR) {
            continue;
          }

          activeBaud = fallbackBaud;
          serialLog("retrying mini bootstrap at %u baud", activeBaud);
          bootstrap = bootstrapMiniVci();
          if (bootstrap == STATUS_NOERROR) {
            serialLog("mini bootstrap succeeded at %u baud", activeBaud);
            break;
          }
        }
      }

      if (bootstrap != STATUS_NOERROR && miniBootstrapStrictEnabled()) {
        serialLog("mini bootstrap failed; rejecting serial node");
        close();
        return ERR_FAILED;
      }
    }

    if (serialRequireRxEnabled() && !waitForAnyIncoming(serialRequireRxTimeoutMs())) {
      serialLog("no serial RX observed within %u ms; rejecting serial node",
                serialRequireRxTimeoutMs());
      close();
      return ERR_FAILED;
    }

    return STATUS_NOERROR;
  }

  void close() override {
    closeFd();
    open_ = false;
    rxBuffer_.clear();
    miniVciReady_ = false;
  }

  Status write(const std::vector<std::uint8_t>& packet) override {
    if (!open_ || fd_ < 0) {
      return ERR_NOT_INITIALIZED;
    }

    std::size_t offset = 0;
    while (offset < packet.size()) {
      const auto written = ::write(fd_, packet.data() + offset, packet.size() - offset);
      if (written < 0) {
        if (errno == EINTR) continue;
        serialLog("write failed: %s", std::strerror(errno));
        return ERR_FAILED;
      }
      offset += static_cast<std::size_t>(written);
    }
    if (serialVerboseEnabled() && !packet.empty()) {
      serialLog("tx[%zu]: %s", packet.size(), hexString(packet).c_str());
    }
    return STATUS_NOERROR;
  }

  Status read(std::vector<std::uint8_t>& packet, std::uint32_t timeoutMs) override {
    if (!open_ || fd_ < 0) {
      return ERR_NOT_INITIALIZED;
    }

    packet.clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (true) {
      if (extractFramedPacket(packet)) {
        return STATUS_NOERROR;
      }

      std::vector<std::uint8_t> chunk;
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return ERR_TIMEOUT;
      }
      const auto remaining = static_cast<std::uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

      const auto status = readChunk(chunk, remaining);
      if (status == ERR_TIMEOUT) {
        serialLog("read timeout after %u ms (rx_buffer=%zu, mini_ready=%d)",
                  timeoutMs,
                  rxBuffer_.size(),
                  miniVciReady_ ? 1 : 0);
        return ERR_TIMEOUT;
      }
      if (status != STATUS_NOERROR) {
        return status;
      }

      rxBuffer_.insert(rxBuffer_.end(), chunk.begin(), chunk.end());
    }
  }

  Status controlTransfer(std::uint8_t,
                         std::uint8_t,
                         std::uint16_t,
                         std::uint16_t,
                         std::vector<std::uint8_t>&,
                         std::uint32_t) override {
    return ERR_NOT_SUPPORTED;
  }

  void clearRx() override {
    if (fd_ >= 0) ::tcflush(fd_, TCIFLUSH);
    rxBuffer_.clear();
  }

  void clearTx() override {
    if (fd_ >= 0) ::tcflush(fd_, TCOFLUSH);
  }

private:
  Status readChunk(std::vector<std::uint8_t>& out, std::uint32_t timeoutMs) {
    out.clear();
    if (fd_ < 0) {
      return ERR_NOT_INITIALIZED;
    }

    timeval tv{};
    tv.tv_sec = static_cast<long>(timeoutMs / 1000U);
    tv.tv_usec = static_cast<int>((timeoutMs % 1000U) * 1000U);

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd_, &rfds);

    const int ready = ::select(fd_ + 1, &rfds, nullptr, nullptr, &tv);
    if (ready < 0) {
      if (errno == EINTR) return ERR_TIMEOUT;
      serialLog("select failed: %s", std::strerror(errno));
      return ERR_FAILED;
    }
    if (ready == 0) {
      return ERR_TIMEOUT;
    }

    unsigned char buffer[512];
    const auto got = ::read(fd_, buffer, sizeof(buffer));
    if (got < 0) {
      if (errno == EINTR) return ERR_TIMEOUT;
      serialLog("read failed: %s", std::strerror(errno));
      return ERR_FAILED;
    }
    if (got == 0) {
      return ERR_TIMEOUT;
    }

    out.assign(buffer, buffer + got);
    if (serialVerboseEnabled()) {
      serialLog("rx[%zu]: %s", out.size(), hexString(out).c_str());
    }
    return STATUS_NOERROR;
  }

  bool extractFramedPacket(std::vector<std::uint8_t>& packet) {
    return tryExtractMvcIFrame(rxBuffer_, packet);
  }

  bool waitForReply(const std::vector<std::vector<std::uint8_t>>& acceptedReplies,
                    std::uint32_t timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::vector<std::uint8_t> stream;
    while (std::chrono::steady_clock::now() < deadline) {
      std::vector<std::uint8_t> chunk;
      const auto status = readChunk(chunk, 50);
      if (status == ERR_TIMEOUT) {
        continue;
      }
      if (status != STATUS_NOERROR) {
        return false;
      }

      stream.insert(stream.end(), chunk.begin(), chunk.end());
      if (stream.size() > 4096U) {
        stream.erase(stream.begin(), stream.end() - 1024);
      }

      for (const auto& expected : acceptedReplies) {
        if (chunk == expected || containsSequence(stream, expected)) {
          return true;
        }
      }
    }
    return false;
  }

  bool waitForAnyIncoming(std::uint32_t timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
      std::vector<std::uint8_t> chunk;
      const auto status = readChunk(chunk, 25);
      if (status == ERR_TIMEOUT) {
        continue;
      }
      if (status != STATUS_NOERROR) {
        return false;
      }

      rxBuffer_.insert(rxBuffer_.end(), chunk.begin(), chunk.end());
      return true;
    }
    return false;
  }

  Status bootstrapMiniVci() {
    const std::vector<std::uint8_t> start1{0x03, 0x00, 0x03};
    const std::vector<std::uint8_t> start2{0x0c, 0x00, 0x07, 0x00, 0x01, 0x4d, 0x56, 0x43, 0x49, 0x2d, 0x54, 0x62};
    const std::vector<std::uint8_t> start3{0x13, 0x00, 0xd0, 0x4d, 0x01, 0xf7, 0x76, 0x39, 0x07, 0x6b,
                                          0x27, 0x40, 0xea, 0x48, 0xfd, 0x6e, 0xa4, 0xa9, 0x00};

    // PCAP shows FTDI bulk-IN status bytes (01 60) on USB. Serial VCP often strips these.
    const std::vector<std::uint8_t> ack1FtdiStatus{0x01, 0x60};
    const std::vector<std::uint8_t> ack2WithStatus{0x01, 0x60, 0x0e, 0x00, 0x09, 0x00, 0x01, 0xb0,
                             0xcb, 0x49, 0x68, 0x07, 0x45, 0xc8, 0x7f, 0xa9};
    const std::vector<std::uint8_t> ack2NoStatus{0x0e, 0x00, 0x09, 0x00, 0x01, 0xb0,
                           0xcb, 0x49, 0x68, 0x07, 0x45, 0xc8, 0x7f, 0xa9};
    const std::vector<std::uint8_t> ack3WithStatus{0x01, 0x60, 0x0b, 0x00, 0x71, 0x08, 0x8e,
                             0x8d, 0x8d, 0xa6, 0xaa, 0xdf, 0x2f};
    const std::vector<std::uint8_t> ack3NoStatus{0x0b, 0x00, 0x71, 0x08, 0x8e,
                           0x8d, 0x8d, 0xa6, 0xaa, 0xdf, 0x2f};

    for (int attempt = 1; attempt <= 3; ++attempt) {
      clearRx();
      if (write(start1) != STATUS_NOERROR) {
        serialLog("mini bootstrap stage1 attempt %d write failed", attempt);
        continue;
      }

      const bool stage1Acked = waitForReply({ack1FtdiStatus}, 350);
      if (!stage1Acked && miniBootstrapStage1StrictEnabled()) {
        serialLog("mini bootstrap stage1 attempt %d failed (strict)", attempt);
        continue;
      }
      if (!stage1Acked) {
        serialLog("mini bootstrap stage1 attempt %d: no explicit ack; continuing", attempt);
      }

      if (write(start2) != STATUS_NOERROR ||
          !waitForReply({ack2WithStatus, ack2NoStatus}, 900)) {
        serialLog("mini bootstrap stage2 attempt %d failed", attempt);
        continue;
      }
      if (write(start3) != STATUS_NOERROR ||
          !waitForReply({ack3WithStatus, ack3NoStatus}, 900)) {
        serialLog("mini bootstrap stage3 attempt %d failed", attempt);
        continue;
      }

      miniVciReady_ = true;
      serialLog("mini bootstrap completed (attempt %d)", attempt);
      return STATUS_NOERROR;
    }

    return ERR_FAILED;
  }

  void closeFd() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  int fd_{-1};
  std::atomic<bool> open_{false};
  std::vector<std::uint8_t> rxBuffer_;
  bool miniVciReady_{false};
};

} // namespace

std::unique_ptr<ITransport> createSerialTransport() {
  return std::make_unique<SerialTransport>();
}

} // namespace mvci
