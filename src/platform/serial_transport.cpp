#include "mvci/platform/transport.hpp"

#include "mvci/platform/frame_resync.hpp"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <dirent.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <string>
#include <thread>
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

std::uint32_t serialOpenRetryMs() {
  static const std::uint32_t timeoutMs = []() {
    const char* value = std::getenv("MVCI_SERIAL_OPEN_RETRY_MS");
    if (!value || value[0] == '\0') {
      return 3000U;
    }
    char* end = nullptr;
    const auto parsed = std::strtoul(value, &end, 10);
    if (end == value) {
      return 3000U;
    }
    return static_cast<std::uint32_t>(std::max<unsigned long>(parsed, 150UL));
  }();
  return timeoutMs;
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

bool miniPostBootstrapScriptEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_POST_BOOTSTRAP");
    if (!value || value[0] == '\0') {
      return true;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool miniPostBootstrapScriptStrictEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_POST_BOOTSTRAP_STRICT");
    if (!value || value[0] == '\0') {
      return false;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool miniKeepaliveBridgeEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_KEEPALIVE_BRIDGE");
    if (!value || value[0] == '\0') {
      return true;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool miniSessionTickleEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_SESSION_TICKLE");
    if (!value || value[0] == '\0') {
      return true;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool miniAltInitEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_ALT_INIT");
    if (!value || value[0] == '\0') {
      return false;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool miniIcvmPrimeSweepEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_ICVM_PRIME_SWEEP");
    if (!value || value[0] == '\0') {
      return false;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool miniIcvmFlushBeforeWriteEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_ICVM_FLUSH");
    if (!value || value[0] == '\0') {
      return false;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool miniReplayBeforeIcvmEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_REPLAY_BEFORE_ICVM");
    if (!value || value[0] == '\0') {
      return false;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool miniCaptureNonMvciEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_CAPTURE_NON_MVCI");
    if (!value || value[0] == '\0') {
      return false;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool miniReplayLooseEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_REPLAY_LOOSE");
    if (!value || value[0] == '\0') {
      return false;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool miniReplay20BeforeIcvmEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_REPLAY20_BEFORE_ICVM");
    if (!value || value[0] == '\0') {
      return false;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool miniTransitionProbesEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_TRANSITION_PROBES");
    if (!value || value[0] == '\0') {
      return false;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool miniTransitionProbesEachIcvmEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_TRANSITION_PROBES_EACH_ICVM");
    if (!value || value[0] == '\0') {
      return false;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool miniTransition7d25FollowupEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_TRANSITION_7D25_FOLLOWUP");
    if (!value || value[0] == '\0') {
      return false;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool miniTransition7d25ProtocolSweepEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_TRANSITION_7D25_PROTOCOL_SWEEP");
    if (!value || value[0] == '\0') {
      return false;
    }
    return value[0] != '0';
  }();
  return enabled;
}

bool miniTransition7d25FollowupEachIcvmEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_MINIVCI_TRANSITION_7D25_FOLLOWUP_EACH_ICVM");
    if (!value || value[0] == '\0') {
      return false;
    }
    return value[0] != '0';
  }();
  return enabled;
}

std::uint32_t miniReplay20Cycles() {
  static const std::uint32_t cycles = []() {
    const char* value = std::getenv("MVCI_MINIVCI_REPLAY20_CYCLES");
    if (!value || value[0] == '\0') {
      return 1U;
    }
    char* end = nullptr;
    const auto parsed = std::strtoul(value, &end, 10);
    if (end == value || parsed == 0U) {
      return 1U;
    }
    return static_cast<std::uint32_t>(parsed);
  }();
  return cycles;
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

enum class SerialCtrlMode {
  Assert,
  None,
  Pulse,
};

SerialCtrlMode serialCtrlMode() {
  static const SerialCtrlMode mode = []() {
    const char* value = std::getenv("MVCI_SERIAL_CTRL_MODE");
    if (!value || value[0] == '\0') {
      return serialAssertControlLinesEnabled() ? SerialCtrlMode::Assert : SerialCtrlMode::None;
    }
    if (std::strcmp(value, "none") == 0) {
      return SerialCtrlMode::None;
    }
    if (std::strcmp(value, "pulse") == 0) {
      return SerialCtrlMode::Pulse;
    }
    return SerialCtrlMode::Assert;
  }();
  return mode;
}

bool serialCtrlModeExplicitlyConfigured() {
  const char* value = std::getenv("MVCI_SERIAL_CTRL_MODE");
  return value && value[0] != '\0';
}

bool serialCtrlAutoFallbackEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MVCI_SERIAL_CTRL_AUTO");
    if (!value || value[0] == '\0') {
      return true;
    }
    return value[0] != '0';
  }();
  return enabled;
}

void setControlLineBits(int fd, bool dtr, bool rts) {
  int modem = 0;
  if (::ioctl(fd, TIOCMGET, &modem) != 0) {
    return;
  }
  if (dtr) modem |= TIOCM_DTR; else modem &= ~TIOCM_DTR;
  if (rts) modem |= TIOCM_RTS; else modem &= ~TIOCM_RTS;
  (void)::ioctl(fd, TIOCMSET, &modem);
}

void applySerialCtrlMode(int fd, SerialCtrlMode mode) {
  switch (mode) {
    case SerialCtrlMode::Assert:
      setControlLineBits(fd, true, true);
      break;
    case SerialCtrlMode::None:
      setControlLineBits(fd, false, false);
      break;
    case SerialCtrlMode::Pulse:
      setControlLineBits(fd, false, false);
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      setControlLineBits(fd, true, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      break;
  }
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

bool tryDecodeMiniObfuscatedIcvm(std::vector<std::uint8_t>& bytes) {
  if (bytes.size() < 4U || (bytes[0] & 0x7FU) != 0x49U) {
    return false;
  }

  // Captured Mini-VCI replies can be XOR-obfuscated with a repeating 3-byte key.
  static constexpr std::uint8_t kKey[3] = {0x88U, 0xFAU, 0x78U};

  std::vector<std::uint8_t> decoded(bytes);
  decoded[0] = 0x49U;
  for (std::size_t i = 1; i < decoded.size(); ++i) {
    decoded[i] ^= kKey[(i - 1U) % 3U];
  }

  if (decoded.size() >= 4U &&
      decoded[0] == 0x49U &&
      decoded[1] == 0x43U &&
      decoded[3] == 0x4DU &&
      (decoded[2] == 0x56U || decoded[2] == 0x57U)) {
    bytes = std::move(decoded);
    return true;
  }

  return false;
}

std::vector<std::string> findSerialNodeCandidates() {
  std::vector<std::string> nodes;
  DIR* dir = ::opendir("/dev");
  if (!dir) {
    return nodes;
  }

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

  std::sort(nodes.begin(), nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  return nodes;
}

std::string pickReenumeratedNode(const std::string& originalPath) {
  const auto nodes = findSerialNodeCandidates();
  if (nodes.empty()) {
    return {};
  }

  if (std::find(nodes.begin(), nodes.end(), originalPath) != nodes.end()) {
    return originalPath;
  }

  const bool wantsUsbSerial = originalPath.find("usbserial") != std::string::npos;
  const bool wantsUsbModem = originalPath.find("usbmodem") != std::string::npos;

  for (const auto& node : nodes) {
    if (wantsUsbSerial && node.find("usbserial") != std::string::npos) {
      return node;
    }
    if (wantsUsbModem && node.find("usbmodem") != std::string::npos) {
      return node;
    }
  }

  return nodes.front();
}

int openSerialNodeWithRetry(const std::string& initialPath, std::string& openedPath) {
  std::string currentPath = initialPath;
  int lastErrno = 0;
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    const int fd = ::open(currentPath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd >= 0) {
      openedPath = currentPath;
      return fd;
    }

    lastErrno = errno;
    if (lastErrno == ENOENT || lastErrno == ETIMEDOUT || lastErrno == ENXIO || lastErrno == EIO) {
      const std::string alternate = pickReenumeratedNode(initialPath);
      if (!alternate.empty() && alternate != currentPath) {
        currentPath = alternate;
      }
    }

    const auto elapsedMs = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
    if (elapsedMs >= serialOpenRetryMs()) {
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }

  errno = lastErrno;
  openedPath.clear();
  return -1;
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

    std::string openedPath;
    fd_ = openSerialNodeWithRetry(path, openedPath);
    if (fd_ < 0) {
      serialLog("open('%s') failed: %s", path.c_str(), std::strerror(errno));
      return ERR_FAILED;
    }

    if (!openedPath.empty() && openedPath != path) {
      serialLog("serial node moved from '%s' to '%s'", path.c_str(), openedPath.c_str());
      path = openedPath;
    }

    serialPath_ = path;

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
    currentSerialBaud_ = activeBaud;
    open_ = true;
    serialLog("opened '%s' at %u baud", path.c_str(), activeBaud);

    if (miniBootstrapEnabled()) {
      auto bootstrap = bootstrapMiniWithFallback(configuredBaud, "open");

      if (bootstrap != STATUS_NOERROR && fd_ < 0 && !serialPath_.empty()) {
        serialLog("bootstrap aborted with closed fd; reopening '%s' and retrying", serialPath_.c_str());
        std::string reopenedPath;
        const int reopenedFd = openSerialNodeWithRetry(serialPath_, reopenedPath);
        if (reopenedFd >= 0) {
          if (!reopenedPath.empty() && reopenedPath != serialPath_) {
            serialLog("serial node moved from '%s' to '%s'", serialPath_.c_str(), reopenedPath.c_str());
            serialPath_ = reopenedPath;
          }
          fd_ = reopenedFd;
          const int reflags = ::fcntl(fd_, F_GETFL, 0);
          if (reflags >= 0) {
            ::fcntl(fd_, F_SETFL, reflags & ~O_NONBLOCK);
          }
          (void)::ioctl(fd_, TIOCEXCL);
          miniVciReady_ = false;
          miniAltProfile_ = false;
          miniIcvmPrimeAttempted_ = false;
          miniReplayBeforeIcvmDone_ = false;
          transformedReplayStateSeen_ = false;
          transitionProbesDone_ = false;
          bootstrap = bootstrapMiniWithFallback(configuredBaud, "open-retry");
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
    miniAltProfile_ = false;
    transformedReplayStateSeen_ = false;
    transitionProbesDone_ = false;
    icvmWriteCount_ = 0U;
    transitionProbeRunCount_ = 0U;
    transition7d25FollowupCount_ = 0U;
    lastTransitionProbeCode_ = 0U;
    lastTransitionProbeCodeValid_ = false;
    currentSerialBaud_ = 0U;
    serialPath_.clear();
  }

  Status write(const std::vector<std::uint8_t>& packet) override {
    if (!open_ || fd_ < 0) {
      return ERR_NOT_INITIALIZED;
    }

    const bool isIcvm = isIcvmFramedPacket(packet);
    if (isIcvm && !miniVciReady_) {
      if (!miniBootstrapEnabled()) {
        serialLog("rejecting icvm write: mini transport not ready");
        return ERR_NOT_INITIALIZED;
      }

      const unsigned int configuredBaud = currentSerialBaud_ ? currentSerialBaud_ : desiredBaud();
      serialLog("icvm write while mini not ready; attempting bootstrap");
      if (bootstrapMiniWithFallback(configuredBaud, "write") != STATUS_NOERROR) {
        serialLog("rejecting icvm write: mini transport bootstrap failed");
        return ERR_NOT_INITIALIZED;
      }

      serialLog("icvm write bootstrap completed");
    }

    if (miniVciReady_ && isIcvm) {
      ++icvmWriteCount_;
      logMiniStateFingerprint("icvm pre");
      maybeReplayBeforeIcvm();
      maybeRunTransitionProbes();
      maybePrimeMiniIcvm(packet);
      logMiniStateFingerprint("icvm post");
      lastIcvmTx_ = packet;
      icvmAwaitingResponse_ = true;
      if (miniSessionTickleEnabled()) {
        (void)sendMiniSessionTickle();
      }
      if (miniKeepaliveBridgeEnabled()) {
        (void)sendMiniKeepalive();
      }
    }

    return writeBytes(packet);
  }

  Status writeBytes(const std::vector<std::uint8_t>& packet) {
    if (!open_ || fd_ < 0) {
      return ERR_NOT_INITIALIZED;
    }

    if (miniVciReady_ && isIcvmFramedPacket(packet) && miniIcvmFlushBeforeWriteEnabled()) {
      ::tcflush(fd_, TCIOFLUSH);
      serialLog("flushed serial buffers before icvm write");
    }

    std::size_t offset = 0;
    while (offset < packet.size()) {
      const auto written = ::write(fd_, packet.data() + offset, packet.size() - offset);
      if (written < 0) {
        if (errno == EINTR) continue;
        if (maybeRecoverSerialDevice("write", errno)) {
          continue;
        }
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
        icvmAwaitingResponse_ = false;
        return STATUS_NOERROR;
      }

      std::vector<std::uint8_t> chunk;
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return ERR_TIMEOUT;
      }
      const auto remaining = static_cast<std::uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

      const auto pollMs = std::min<std::uint32_t>(remaining, 120U);
      const auto status = readChunk(chunk, pollMs);
      if (status == ERR_TIMEOUT) {
        if (miniVciReady_ && miniSessionTickleEnabled()) {
          maybeTickleSession();
        }

        if (std::chrono::steady_clock::now() >= deadline) {
          serialLog("read timeout after %u ms (rx_buffer=%zu, mini_ready=%d)",
                    timeoutMs,
                    rxBuffer_.size(),
                    miniVciReady_ ? 1 : 0);
          logMiniStateFingerprint("icvm timeout");
          return ERR_TIMEOUT;
        }
        continue;
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

  Status miniRawRequest(const std::vector<std::uint8_t>& request,
                        std::vector<std::uint8_t>& response,
                        std::uint32_t timeoutMs) override {
    response.clear();
    if (!open_ || fd_ < 0) {
      return ERR_NOT_INITIALIZED;
    }
    if (!miniVciReady_) {
      return ERR_NOT_INITIALIZED;
    }
    if (request.empty()) {
      return ERR_INVALID_MSG;
    }

    if (write(request) != STATUS_NOERROR) {
      return ERR_FAILED;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
      std::vector<std::uint8_t> chunk;
      const auto now = std::chrono::steady_clock::now();
      const auto remaining = static_cast<std::uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
      const auto status = readChunk(chunk, std::min<std::uint32_t>(remaining, 120U));
      if (status == ERR_TIMEOUT) {
        if (miniSessionTickleEnabled()) {
          maybeTickleSession();
        }
        continue;
      }
      if (status != STATUS_NOERROR) {
        return status;
      }

      if (chunk.size() >= 2U && chunk[0] == 0x01U && chunk[1] == 0x60U) {
        chunk.erase(chunk.begin(), chunk.begin() + 2);
      }
      if (!chunk.empty()) {
        response = std::move(chunk);
        return STATUS_NOERROR;
      }
    }

    return ERR_TIMEOUT;
  }

  void clearRx() override {
    if (fd_ >= 0) ::tcflush(fd_, TCIFLUSH);
    rxBuffer_.clear();
  }

  void clearTx() override {
    if (fd_ >= 0) ::tcflush(fd_, TCOFLUSH);
  }

private:
  void logCtrlMode(SerialCtrlMode mode) {
    switch (mode) {
      case SerialCtrlMode::Assert:
        serialLog("serial ctrl mode: assert");
        break;
      case SerialCtrlMode::None:
        serialLog("serial ctrl mode: none");
        break;
      case SerialCtrlMode::Pulse:
        serialLog("serial ctrl mode: pulse");
        break;
    }
  }

  Status bootstrapMiniWithFallback(unsigned int configuredBaud, const char* source) {
    if (fd_ < 0) {
      return ERR_NOT_INITIALIZED;
    }

    auto tryBootstrapForMode = [&](SerialCtrlMode mode) {
      if (fd_ < 0) {
        return ERR_NOT_INITIALIZED;
      }
      applySerialCtrlMode(fd_, mode);
      logCtrlMode(mode);

      auto bootstrap = bootstrapMiniVci();
      if (bootstrap == STATUS_NOERROR) {
        return STATUS_NOERROR;
      }
      if (fd_ < 0) {
        return ERR_NOT_INITIALIZED;
      }

      for (unsigned int fallbackBaud : {500000U, 230400U, 115200U, 38400U}) {
        if (fallbackBaud == currentSerialBaud_) {
          continue;
        }
        if (fd_ < 0) {
          return ERR_NOT_INITIALIZED;
        }

        if (configureSerialPort(fd_, fallbackBaud) != STATUS_NOERROR) {
          continue;
        }

        currentSerialBaud_ = fallbackBaud;
        serialLog("%s retrying mini bootstrap at %u baud", source, fallbackBaud);
        bootstrap = bootstrapMiniVci();
        if (bootstrap == STATUS_NOERROR) {
          serialLog("mini bootstrap succeeded at %u baud", fallbackBaud);
          return STATUS_NOERROR;
        }
        if (fd_ < 0) {
          return ERR_NOT_INITIALIZED;
        }
      }

      return ERR_FAILED;
    };

    if (configureSerialPort(fd_, configuredBaud) == STATUS_NOERROR) {
      currentSerialBaud_ = configuredBaud;
    }

    const auto configuredMode = serialCtrlMode();
    auto bootstrap = tryBootstrapForMode(configuredMode);

    if (bootstrap != STATUS_NOERROR && fd_ < 0) {
      return bootstrap;
    }

    if (bootstrap != STATUS_NOERROR &&
        !serialCtrlModeExplicitlyConfigured() &&
        serialCtrlAutoFallbackEnabled()) {
      for (SerialCtrlMode mode : {SerialCtrlMode::None, SerialCtrlMode::Pulse, SerialCtrlMode::Assert}) {
        if (mode == configuredMode) {
          continue;
        }
        if (fd_ < 0) {
          return bootstrap;
        }

        if (configureSerialPort(fd_, configuredBaud) == STATUS_NOERROR) {
          currentSerialBaud_ = configuredBaud;
        }

        serialLog("%s retrying bootstrap with alternate ctrl mode", source);
        bootstrap = tryBootstrapForMode(mode);
        if (bootstrap == STATUS_NOERROR) {
          return STATUS_NOERROR;
        }
        if (fd_ < 0) {
          return bootstrap;
        }
      }
    }

    if (fd_ >= 0 && configureSerialPort(fd_, configuredBaud) == STATUS_NOERROR) {
      currentSerialBaud_ = configuredBaud;
    }

    return bootstrap;
  }

  bool maybeRecoverSerialDevice(const char* source, int err) {
    if (!(err == ENODEV || err == ENXIO || err == EIO || err == ENOTCONN || err == EBADF)) {
      return false;
    }

    if (serialPath_.empty()) {
      return false;
    }

    serialLog("serial recover from %s after %s", source, std::strerror(err));
    closeFd();

    std::string reopenedPath;
    const int reopenedFd = openSerialNodeWithRetry(serialPath_, reopenedPath);
    if (reopenedFd < 0) {
      serialLog("serial recover open('%s') failed: %s", serialPath_.c_str(), std::strerror(errno));
      return false;
    }

    if (!reopenedPath.empty() && reopenedPath != serialPath_) {
      serialLog("serial recovered node moved from '%s' to '%s'", serialPath_.c_str(), reopenedPath.c_str());
      serialPath_ = reopenedPath;
    }

    fd_ = reopenedFd;
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags >= 0) {
      ::fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);
    }

    if (::ioctl(fd_, TIOCEXCL) < 0) {
      serialLog("serial recover TIOCEXCL failed: %s (continuing)", std::strerror(errno));
    }

    const unsigned int configuredBaud = currentSerialBaud_ ? currentSerialBaud_ : desiredBaud();
    if (configureSerialPort(fd_, configuredBaud) != STATUS_NOERROR) {
      serialLog("serial recover configure failed");
      closeFd();
      return false;
    }

    currentSerialBaud_ = configuredBaud;

    // Recovery can reset adapter runtime state; force Mini bootstrap again.
    miniVciReady_ = false;
    miniAltProfile_ = false;
    miniIcvmPrimeAttempted_ = false;
    miniReplayBeforeIcvmDone_ = false;
    transformedReplayStateSeen_ = false;
    transitionProbesDone_ = false;

    if (miniBootstrapEnabled()) {
      const auto bootstrap = bootstrapMiniWithFallback(configuredBaud, "recover");
      if (bootstrap != STATUS_NOERROR) {
        serialLog("serial recover mini bootstrap failed");
        closeFd();
        return false;
      }
      serialLog("serial recover mini bootstrap completed");
    }

    serialLog("serial recovered on '%s' at %u baud", serialPath_.c_str(), currentSerialBaud_);
    return true;
  }

  Status readChunk(std::vector<std::uint8_t>& out, std::uint32_t timeoutMs) {
    out.clear();
    if (fd_ < 0) {
      return ERR_NOT_INITIALIZED;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
      timeval tv{};
      tv.tv_sec = static_cast<long>(timeoutMs / 1000U);
      tv.tv_usec = static_cast<int>((timeoutMs % 1000U) * 1000U);

      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(fd_, &rfds);

      const int ready = ::select(fd_ + 1, &rfds, nullptr, nullptr, &tv);
      if (ready < 0) {
        if (errno == EINTR) return ERR_TIMEOUT;
        if (attempt == 0 && maybeRecoverSerialDevice("select", errno)) {
          continue;
        }
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
        if (attempt == 0 && maybeRecoverSerialDevice("read", errno)) {
          continue;
        }
        serialLog("read failed: %s", std::strerror(errno));
        return ERR_FAILED;
      }
      if (got == 0) {
        return ERR_TIMEOUT;
      }

      out.assign(buffer, buffer + got);
      if (tryDecodeMiniObfuscatedIcvm(out)) {
        serialLog("decoded mini obfuscated icvm frame");
      }
      if (serialVerboseEnabled()) {
        serialLog("rx[%zu]: %s", out.size(), hexString(out).c_str());
      }
      return STATUS_NOERROR;
    }

    return ERR_FAILED;
  }

  bool extractFramedPacket(std::vector<std::uint8_t>& packet) {
    if (miniCaptureNonMvciEnabled() && icvmAwaitingResponse_ && rxBuffer_.size() >= 24U) {
      const std::uint32_t maybeMvcI = static_cast<std::uint32_t>(rxBuffer_[0]) |
                                      (static_cast<std::uint32_t>(rxBuffer_[1]) << 8U) |
                                      (static_cast<std::uint32_t>(rxBuffer_[2]) << 16U) |
                                      (static_cast<std::uint32_t>(rxBuffer_[3]) << 24U);
      if (maybeMvcI != kMvciPacketMagic) {
        serialLog("non-mvci rx while awaiting icvm response (%zu bytes): %s",
                  rxBuffer_.size(),
                  hexString(rxBuffer_).c_str());
        serialLog("last icvm tx[%zu]: %s", lastIcvmTx_.size(), hexString(lastIcvmTx_).c_str());
      }
    }

    return tryExtractMvcIFrame(rxBuffer_, packet);
  }

  bool isIcvmFramedPacket(const std::vector<std::uint8_t>& packet) const {
    return packet.size() >= 4 &&
           packet[0] == 0x49 &&
           packet[1] == 0x43 &&
           packet[2] == 0x56 &&
           packet[3] == 0x4d;
  }

  void maybePrimeMiniIcvm(const std::vector<std::uint8_t>& packet) {
    if (miniIcvmPrimeAttempted_ || !miniIcvmPrimeSweepEnabled() || packet.size() < 12U) {
      return;
    }

    miniIcvmPrimeAttempted_ = true;
    std::vector<std::uint32_t> protocols{3U, 5U, 7U};
    const bool transformed7d25 = lastTransitionProbeCodeValid_ && lastTransitionProbeCode_ == 0x7dU;
    if (transformed7d25 && miniTransition7d25ProtocolSweepEnabled()) {
      protocols = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 16U};
      serialLog("running mini icvm prime sweep (expanded 0x7d mode)");
    } else {
      serialLog("running mini icvm prime sweep");
    }

    for (std::uint32_t protocol : protocols) {
      std::vector<std::uint8_t> probe(packet);
      probe[8] = static_cast<std::uint8_t>(protocol & 0xffU);
      probe[9] = static_cast<std::uint8_t>((protocol >> 8U) & 0xffU);
      probe[10] = static_cast<std::uint8_t>((protocol >> 16U) & 0xffU);
      probe[11] = static_cast<std::uint8_t>((protocol >> 24U) & 0xffU);

      if (writeBytes(probe) != STATUS_NOERROR) {
        serialLog("mini icvm prime sweep write failed at protocol %u", protocol);
        return;
      }

      std::vector<std::uint8_t> response;
      const auto status = readChunk(response, 220);
      if (status == STATUS_NOERROR && !response.empty()) {
        serialLog("mini icvm prime protocol %u got rx[%zu]: %s",
                  protocol,
                  response.size(),
                  hexString(response).c_str());
      }
    }
  }

  void maybeReplayBeforeIcvm() {
    if (miniReplayBeforeIcvmDone_ || !miniReplayBeforeIcvmEnabled()) {
      return;
    }

    miniReplayBeforeIcvmDone_ = true;
    serialLog("running mini replay before icvm");
    const bool ok = miniReplay20BeforeIcvmEnabled()
      ? runMiniReplayProfile20BeforeIcvm()
      : runMiniPostBootstrapReplay(true, miniReplayLooseEnabled());
    if (!ok) {
      serialLog("mini replay before icvm failed");
    }
  }

  bool looksLikeTransformedReplayReply(const std::vector<std::uint8_t>& resp) const {
    if (resp.size() < 11U || resp[0] != 0x0bU || resp[1] != 0x00U) {
      return false;
    }

    // Known stable runtime reply family observed in standard state.
    for (std::uint8_t code : {0x71U, 0xa3U, 0xd2U, 0x1aU, 0xb6U, 0x64U}) {
      if (resp[2] == code) {
        return false;
      }
    }
    return true;
  }

  void logMiniStateFingerprint(const char* stage) const {
    if (!serialVerboseEnabled()) {
      return;
    }

    if (lastTransitionProbeCodeValid_) {
      serialLog("mini state %s: icvm=%u transformed=%d probe_runs=%u probe_last=0x%02x followups=%u prime=%d",
                stage,
                icvmWriteCount_,
                transformedReplayStateSeen_ ? 1 : 0,
                transitionProbeRunCount_,
                static_cast<unsigned int>(lastTransitionProbeCode_),
                transition7d25FollowupCount_,
                miniIcvmPrimeAttempted_ ? 1 : 0);
      return;
    }

    serialLog("mini state %s: icvm=%u transformed=%d probe_runs=%u probe_last=none followups=%u prime=%d",
              stage,
              icvmWriteCount_,
              transformedReplayStateSeen_ ? 1 : 0,
              transitionProbeRunCount_,
              transition7d25FollowupCount_,
              miniIcvmPrimeAttempted_ ? 1 : 0);
  }

  void maybeRunTransition7d25Followup() {
    if (!miniTransition7d25FollowupEnabled()) {
      return;
    }

    ++transition7d25FollowupCount_;
    serialLog("running mini transition 0x7d followup #%u", transition7d25FollowupCount_);

    const std::vector<std::vector<std::uint8_t>> followup{
      {0x0b, 0x00, 0x71, 0xa1, 0xe8, 0x84, 0xc4, 0xa2, 0x9c, 0xe0, 0xad},
      {0x1b, 0x00, 0xff, 0xe0, 0xff, 0x28, 0x6b, 0xdf, 0xfa, 0x7c, 0x27, 0x28, 0xfb, 0x14, 0x1f, 0xca,
       0xab, 0xfe, 0xbf, 0x98, 0x58, 0x87, 0xe6, 0x29, 0xc4, 0x5e, 0x2c},
      {0x0b, 0x00, 0x31, 0x18, 0x19, 0x2b, 0x97, 0x53, 0x24, 0xce, 0x3e},
      {0x0b, 0x00, 0x31, 0x18, 0x19, 0x2b, 0x97, 0x53, 0x24, 0xce, 0x3e},
    };

    for (std::size_t i = 0; i < followup.size(); ++i) {
      if (writeBytes(followup[i]) != STATUS_NOERROR) {
        serialLog("mini transition 0x7d followup step %zu write failed", i + 1);
        return;
      }

      const auto resp = readWindow(180);
      if (!resp.empty()) {
        serialLog("mini transition 0x7d followup step %zu rx[%zu]: %s",
                  i + 1,
                  resp.size(),
                  hexString(resp).c_str());
      }
    }
  }

  void maybeRunTransitionProbes() {
    if (!miniTransitionProbesEnabled() || !transformedReplayStateSeen_) {
      return;
    }

    const bool repeatEachIcvm = miniTransitionProbesEachIcvmEnabled();
    if (transitionProbesDone_ && !repeatEachIcvm) {
      return;
    }

    transitionProbesDone_ = true;
    ++transitionProbeRunCount_;
    serialLog("running mini transition probes");

    const std::vector<std::vector<std::uint8_t>> probes{
      {0x0b, 0x00, 0x71, 0xa1, 0xe8, 0x84, 0xc4, 0xa2, 0x9c, 0xe0, 0xad},
      {0x0b, 0x00, 0x31, 0x18, 0x19, 0x2b, 0x97, 0x53, 0x24, 0xce, 0x3e},
      {0x0b, 0x00, 0x25, 0x8a, 0x95, 0x1b, 0xe3, 0x6d, 0xfa, 0x9e, 0xc0},
      {0x0b, 0x00, 0x96, 0x72, 0x51, 0x88, 0x2a, 0xaa, 0xba, 0x9b, 0x97},
      {0x0b, 0x00, 0x51, 0x32, 0x0a, 0xc0, 0x2f, 0xd8, 0x97, 0x9c, 0x5e},
      {0x0b, 0x00, 0x69, 0x06, 0x67, 0xdf, 0xf4, 0x14, 0xdf, 0x59, 0xba},
      {0x1b, 0x00, 0xff, 0xe0, 0xff, 0x28, 0x6b, 0xdf, 0xfa, 0x7c, 0x27, 0x28, 0xfb, 0x14, 0x1f, 0xca,
       0xab, 0xfe, 0xbf, 0x98, 0x58, 0x87, 0xe6, 0x29, 0xc4, 0x5e, 0x2c},
    };

    bool saw7d25Family = false;

    for (std::size_t i = 0; i < probes.size(); ++i) {
      if (writeBytes(probes[i]) != STATUS_NOERROR) {
        serialLog("mini transition probe %zu write failed", i + 1);
        return;
      }
      const auto resp = readWindow(180);
      if (!resp.empty()) {
        serialLog("mini transition probe %zu rx[%zu]: %s", i + 1, resp.size(), hexString(resp).c_str());
        if (resp.size() >= 3U && resp[0] == 0x0bU && resp[1] == 0x00U) {
          lastTransitionProbeCodeValid_ = true;
          lastTransitionProbeCode_ = resp[2];
          if (resp[2] == 0x7dU) {
            saw7d25Family = true;
          }
        }
      }
    }

    const bool force7d25Followup = miniTransition7d25FollowupEachIcvmEnabled() &&
                                   lastTransitionProbeCodeValid_ &&
                                   lastTransitionProbeCode_ == 0x7dU;
    if (saw7d25Family || force7d25Followup) {
      maybeRunTransition7d25Followup();
    }
  }

  std::vector<std::uint8_t> readWindow(std::uint32_t windowMs) {
    std::vector<std::uint8_t> out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(windowMs);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto now = std::chrono::steady_clock::now();
      const auto remaining = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
      if (remaining == 0U) {
        break;
      }

      std::vector<std::uint8_t> chunk;
      const auto status = readChunk(chunk, std::min<std::uint32_t>(remaining, 45U));
      if (status == ERR_TIMEOUT) {
        continue;
      }
      if (status != STATUS_NOERROR) {
        break;
      }
      out.insert(out.end(), chunk.begin(), chunk.end());
    }
    return out;
  }

  bool runMiniReplayProfile20BeforeIcvm() {
    const std::vector<std::uint8_t> start1{0x03, 0x00, 0x03};
    const std::vector<std::uint8_t> start2{0x0c, 0x00, 0x07, 0x00, 0x01, 0x4d, 0x56, 0x43, 0x49, 0x2d, 0x54, 0x62};
    const std::vector<std::uint8_t> start3{0x13, 0x00, 0xd0, 0x4d, 0x01, 0xf7, 0x76, 0x39, 0x07, 0x6b,
                                          0x27, 0x40, 0xea, 0x48, 0xfd, 0x6e, 0xa4, 0xa9, 0x00};
    const std::vector<std::uint8_t> keepaliveOut{0x0b, 0x00, 0x31, 0x18, 0x19, 0x2b, 0x97, 0x53, 0x24, 0xce, 0x3e};

    const std::vector<std::vector<std::uint8_t>> outSteps{
      start1,
      start2,
      start3,
      {0x0b, 0x00, 0x25, 0x8a, 0x95, 0x1b, 0xe3, 0x6d, 0xfa, 0x9e, 0xc0},
      {0x0b, 0x00, 0x71, 0xa1, 0xe8, 0x84, 0xc4, 0xa2, 0x9c, 0xe0, 0xad},
      {0x23, 0x00, 0xfb, 0xb3, 0xd4, 0x3c, 0xbb, 0x46, 0x84, 0xb2, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
       0x4d, 0x98, 0x44, 0xdb, 0xf5, 0x35, 0x3a, 0x31, 0xed, 0x0c, 0x3a, 0x3e, 0x04, 0xf8, 0xc1, 0x6b,
       0x73, 0x90, 0xc6},
      {0x23, 0x00, 0x6a, 0x85, 0xd0, 0x98, 0x32, 0xea, 0x3d, 0x1e, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
       0x4d, 0x98, 0x9a, 0x28, 0x9a, 0xd9, 0x38, 0x5d, 0x0b, 0x6f, 0x00, 0xb4, 0xee, 0x06, 0x45, 0xdd,
       0xf5, 0xb3, 0x87},
      {0x23, 0x00, 0x78, 0xa4, 0x9f, 0x19, 0x4b, 0xcd, 0x31, 0xaa, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
       0x4d, 0x98, 0xe5, 0x1a, 0x46, 0x7a, 0x60, 0x37, 0xe2, 0x64, 0x8d, 0x55, 0xee, 0x6f, 0x97, 0xf3,
       0x36, 0xc0, 0x37},
      {0x23, 0x00, 0x0d, 0xc4, 0x7f, 0x17, 0xbd, 0x49, 0x42, 0x01, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
       0x4d, 0x98, 0x43, 0x4f, 0x23, 0xad, 0x5d, 0x7a, 0xcb, 0xaf, 0x5e, 0xdf, 0xea, 0xb2, 0x31, 0xca,
       0x79, 0xb6, 0x93},
      {0x23, 0x00, 0xdc, 0xb5, 0xa5, 0x84, 0x50, 0xc3, 0xea, 0x72, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
       0x4d, 0x98, 0xca, 0xb6, 0x74, 0x6b, 0xc1, 0x6b, 0xa9, 0x9c, 0xd8, 0x54, 0x2d, 0x0c, 0xf8, 0x2c,
       0x6c, 0xd7, 0xd4},
      {0x23, 0x00, 0xc1, 0xdc, 0x74, 0x4e, 0xb4, 0xda, 0x05, 0x60, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
       0x4d, 0x98, 0x31, 0xd0, 0x0a, 0xcb, 0x0d, 0x37, 0xf9, 0x0a, 0x81, 0x1d, 0xc6, 0x5d, 0x8d, 0xad,
       0x33, 0xa0, 0xd8},
      {0x23, 0x00, 0x4c, 0xc0, 0x0d, 0xc4, 0x1d, 0x6e, 0x14, 0x9a, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
       0x4d, 0x98, 0x8a, 0xaa, 0xdf, 0xe0, 0x00, 0x42, 0x34, 0x15, 0xe1, 0x6f, 0xa9, 0x96, 0xf0, 0x95,
       0xfc, 0x69, 0x2c},
      {0x23, 0x00, 0x89, 0x87, 0x3c, 0x4f, 0x1d, 0x6b, 0xef, 0x52, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
       0x4d, 0x98, 0xa3, 0x3c, 0xef, 0xc3, 0x31, 0x82, 0x62, 0xf2, 0x74, 0x9b, 0x98, 0xf2, 0x25, 0x20,
       0x5b, 0x4b, 0x1f},
      {0x0b, 0x00, 0x71, 0xa1, 0xe8, 0x84, 0xc4, 0xa2, 0x9c, 0xe0, 0xad},
      {0x1b, 0x00, 0xff, 0xe0, 0xff, 0x28, 0x6b, 0xdf, 0xfa, 0x7c, 0x27, 0x28, 0xfb, 0x14, 0x1f, 0xca,
       0xab, 0xfe, 0xbf, 0x98, 0x58, 0x87, 0xe6, 0x29, 0xc4, 0x5e, 0x2c},
      keepaliveOut,
      keepaliveOut,
      keepaliveOut,
      keepaliveOut,
      keepaliveOut,
    };

    const std::vector<std::uint32_t> readWindowsMs{
      117U, 17U, 24U, 25U, 22U,
      24U, 23U, 24U, 24U, 23U,
      24U, 23U, 23U, 23U, 24U,
      24U, 23U, 24U, 23U, 23U,
    };

    if (fd_ >= 0) {
      ::tcflush(fd_, TCIOFLUSH);
    }
    clearRx();

    serialLog("mini replay profile20 before icvm");
    const auto cycles = miniReplay20Cycles();
    for (std::uint32_t cycle = 0; cycle < cycles; ++cycle) {
      if (cycle > 0U) {
        clearRx();
      }
      serialLog("mini replay20 cycle %u/%u", cycle + 1U, cycles);
      for (std::size_t i = 0; i < outSteps.size(); ++i) {
        if (writeBytes(outSteps[i]) != STATUS_NOERROR) {
          serialLog("mini replay20 step %zu write failed", i + 1);
          return false;
        }
        const auto resp = readWindow(readWindowsMs[i]);
        if (!resp.empty()) {
          serialLog("mini replay20 step %zu rx[%zu]: %s", i + 1, resp.size(), hexString(resp).c_str());
          if (looksLikeTransformedReplayReply(resp)) {
            transformedReplayStateSeen_ = true;
          }
        }
      }
    }
    return true;
  }

  bool sendMiniKeepalive() {
    const std::vector<std::uint8_t> keepaliveOut{0x0b, 0x00, 0x31, 0x18, 0x19, 0x2b, 0x97, 0x53, 0x24, 0xce, 0x3e};
    const std::vector<std::uint8_t> keepaliveWithStatus{0x01, 0x60, 0x0b, 0x00, 0x64, 0x3b, 0x58, 0x62, 0x53, 0xa7, 0xd6, 0x65, 0x29};
    const std::vector<std::uint8_t> keepaliveNoStatus{0x0b, 0x00, 0x64, 0x3b, 0x58, 0x62, 0x53, 0xa7, 0xd6, 0x65, 0x29};
    const std::vector<std::uint8_t> altRuntimeAck{0x07, 0x00, 0x02, 0x00, 0x00, 0x01, 0x04};

    if (write(keepaliveOut) != STATUS_NOERROR) {
      return false;
    }
    std::vector<std::vector<std::uint8_t>> accepted{keepaliveWithStatus, keepaliveNoStatus};
    if (miniAltProfile_) {
      accepted.push_back(altRuntimeAck);
    }
    const bool ok = waitForReply(accepted, 120);
    if (!ok) {
      serialLog("mini keepalive bridge miss");
    }
    return ok;
  }

  bool sendMiniSessionTickle() {
    const std::vector<std::uint8_t> queryOut{0x0b, 0x00, 0x71, 0xa1, 0xe8, 0x84, 0xc4, 0xa2, 0x9c, 0xe0, 0xad};
    const std::vector<std::uint8_t> queryWithStatus{0x01, 0x60, 0x0b, 0x00, 0xd2, 0xb9, 0x82, 0x0d, 0x1c, 0x58, 0x7c, 0xb4, 0x63};
    const std::vector<std::uint8_t> queryNoStatus{0x0b, 0x00, 0xd2, 0xb9, 0x82, 0x0d, 0x1c, 0x58, 0x7c, 0xb4, 0x63};
    const std::vector<std::uint8_t> altRuntimeAck{0x07, 0x00, 0x02, 0x00, 0x00, 0x01, 0x04};

    std::vector<std::vector<std::uint8_t>> accepted{queryWithStatus, queryNoStatus};
    if (miniAltProfile_) {
      accepted.push_back(altRuntimeAck);
    }

    if (write(queryOut) != STATUS_NOERROR || !waitForReply(accepted, 120)) {
      serialLog("mini session tickle query miss");
      return false;
    }

    return sendMiniKeepalive();
  }

  void maybeTickleSession() {
    const auto now = std::chrono::steady_clock::now();
    if (now < nextMiniTickleAt_) {
      return;
    }

    (void)sendMiniSessionTickle();
    nextMiniTickleAt_ = now + std::chrono::milliseconds(220);
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

  bool runMiniPostBootstrapReplay(bool flushBeforeStart = false,
                                  bool looseMode = false) {
    const std::vector<std::vector<std::uint8_t>> postOut{
      {0x0b, 0x00, 0x25, 0x8a, 0x95, 0x1b, 0xe3, 0x6d, 0xfa, 0x9e, 0xc0},
      {0x0b, 0x00, 0x71, 0xa1, 0xe8, 0x84, 0xc4, 0xa2, 0x9c, 0xe0, 0xad},
      {0x23, 0x00, 0xfb, 0xb3, 0xd4, 0x3c, 0xbb, 0x46, 0x84, 0xb2, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
       0x4d, 0x98, 0x44, 0xdb, 0xf5, 0x35, 0x3a, 0x31, 0xed, 0x0c, 0x3a, 0x3e, 0x04, 0xf8, 0xc1, 0x6b,
       0x73, 0x90, 0xc6},
      {0x23, 0x00, 0x6a, 0x85, 0xd0, 0x98, 0x32, 0xea, 0x3d, 0x1e, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
       0x4d, 0x98, 0x9a, 0x28, 0x9a, 0xd9, 0x38, 0x5d, 0x0b, 0x6f, 0x00, 0xb4, 0xee, 0x06, 0x45, 0xdd,
       0xf5, 0xb3, 0x87},
      {0x23, 0x00, 0x78, 0xa4, 0x9f, 0x19, 0x4b, 0xcd, 0x31, 0xaa, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
       0x4d, 0x98, 0xe5, 0x1a, 0x46, 0x7a, 0x60, 0x37, 0xe2, 0x64, 0x8d, 0x55, 0xee, 0x6f, 0x97, 0xf3,
       0x36, 0xc0, 0x37},
      {0x23, 0x00, 0x0d, 0xc4, 0x7f, 0x17, 0xbd, 0x49, 0x42, 0x01, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
       0x4d, 0x98, 0x43, 0x4f, 0x23, 0xad, 0x5d, 0x7a, 0xcb, 0xaf, 0x5e, 0xdf, 0xea, 0xb2, 0x31, 0xca,
       0x79, 0xb6, 0x93},
      {0x23, 0x00, 0xdc, 0xb5, 0xa5, 0x84, 0x50, 0xc3, 0xea, 0x72, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
       0x4d, 0x98, 0xca, 0xb6, 0x74, 0x6b, 0xc1, 0x6b, 0xa9, 0x9c, 0xd8, 0x54, 0x2d, 0x0c, 0xf8, 0x2c,
       0x6c, 0xd7, 0xd4},
      {0x23, 0x00, 0xc1, 0xdc, 0x74, 0x4e, 0xb4, 0xda, 0x05, 0x60, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
       0x4d, 0x98, 0x31, 0xd0, 0x0a, 0xcb, 0x0d, 0x37, 0xf9, 0x0a, 0x81, 0x1d, 0xc6, 0x5d, 0x8d, 0xad,
       0x33, 0xa0, 0xd8},
      {0x23, 0x00, 0x4c, 0xc0, 0x0d, 0xc4, 0x1d, 0x6e, 0x14, 0x9a, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
       0x4d, 0x98, 0x8a, 0xaa, 0xdf, 0xe0, 0x00, 0x42, 0x34, 0x15, 0xe1, 0x6f, 0xa9, 0x96, 0xf0, 0x95,
       0xfc, 0x69, 0x2c},
      {0x23, 0x00, 0x89, 0x87, 0x3c, 0x4f, 0x1d, 0x6b, 0xef, 0x52, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
       0x4d, 0x98, 0xa3, 0x3c, 0xef, 0xc3, 0x31, 0x82, 0x62, 0xf2, 0x74, 0x9b, 0x98, 0xf2, 0x25, 0x20,
       0x5b, 0x4b, 0x1f},
      {0x0b, 0x00, 0x71, 0xa1, 0xe8, 0x84, 0xc4, 0xa2, 0x9c, 0xe0, 0xad},
      {0x1b, 0x00, 0xff, 0xe0, 0xff, 0x28, 0x6b, 0xdf, 0xfa, 0x7c, 0x27, 0x28, 0xfb, 0x14, 0x1f, 0xca,
       0xab, 0xfe, 0xbf, 0x98, 0x58, 0x87, 0xe6, 0x29, 0xc4, 0x5e, 0x2c},
      {0x0b, 0x00, 0x31, 0x18, 0x19, 0x2b, 0x97, 0x53, 0x24, 0xce, 0x3e},
    };

    const std::vector<std::vector<std::vector<std::uint8_t>>> postExpect{
      {{0x01, 0x60, 0x0b, 0x00, 0xa3, 0xc7, 0xc2, 0x27, 0xd0, 0x0b, 0x16, 0x50, 0x17},
       {0x0b, 0x00, 0xa3, 0xc7, 0xc2, 0x27, 0xd0, 0x0b, 0x16, 0x50, 0x17}},
      {{0x01, 0x60, 0x0b, 0x00, 0xd2, 0xb9, 0x82, 0x0d, 0x1c, 0x58, 0x7c, 0xb4, 0x63},
       {0x0b, 0x00, 0xd2, 0xb9, 0x82, 0x0d, 0x1c, 0x58, 0x7c, 0xb4, 0x63}},
      {{0x01, 0x60, 0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70},
       {0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70}},
      {{0x01, 0x60, 0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70},
       {0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70}},
      {{0x01, 0x60, 0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70},
       {0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70}},
      {{0x01, 0x60, 0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70},
       {0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70}},
      {{0x01, 0x60, 0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70},
       {0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70}},
      {{0x01, 0x60, 0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70},
       {0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70}},
      {{0x01, 0x60, 0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70},
       {0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70}},
      {{0x01, 0x60, 0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70},
       {0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70}},
      {{0x01, 0x60, 0x0b, 0x00, 0xd2, 0xb9, 0x82, 0x0d, 0x1c, 0x58, 0x7c, 0xb4, 0x63},
       {0x0b, 0x00, 0xd2, 0xb9, 0x82, 0x0d, 0x1c, 0x58, 0x7c, 0xb4, 0x63}},
      {{0x01, 0x60, 0x0b, 0x00, 0xb6, 0x19, 0xc7, 0xf0, 0xb5, 0xc3, 0xbd, 0xf8, 0xa0},
       {0x0b, 0x00, 0xb6, 0x19, 0xc7, 0xf0, 0xb5, 0xc3, 0xbd, 0xf8, 0xa0}},
      {{0x01, 0x60, 0x0b, 0x00, 0x64, 0x3b, 0x58, 0x62, 0x53, 0xa7, 0xd6, 0x65, 0x29},
       {0x0b, 0x00, 0x64, 0x3b, 0x58, 0x62, 0x53, 0xa7, 0xd6, 0x65, 0x29}},
    };

    if (flushBeforeStart && fd_ >= 0) {
      ::tcflush(fd_, TCIOFLUSH);
    }
    clearRx();

    if (looseMode) {
      serialLog("mini replay using loose timing mode");
      for (std::size_t i = 0; i < postOut.size(); ++i) {
        if (writeBytes(postOut[i]) != STATUS_NOERROR) {
          serialLog("mini post-bootstrap step %zu write failed (loose)", i + 1);
          return false;
        }
        std::vector<std::uint8_t> chunk;
        (void)readChunk(chunk, 45);
      }
      return true;
    }

    for (std::size_t i = 0; i < postOut.size(); ++i) {
      if (writeBytes(postOut[i]) != STATUS_NOERROR || !waitForReply(postExpect[i], 250)) {
        serialLog("mini post-bootstrap step %zu failed", i + 1);
        return false;
      }
    }

    return true;
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

    const std::vector<std::uint8_t> alt1{0x0b, 0x00, 0x96, 0x72, 0x51, 0x88, 0x2a, 0xaa, 0xba, 0x9b, 0x97};
    const std::vector<std::uint8_t> alt2{0x0b, 0x00, 0x51, 0x32, 0x0a, 0xc0, 0x2f, 0xd8, 0x97, 0x9c, 0x5e};
    const std::vector<std::uint8_t> alt3{0x0b, 0x00, 0x69, 0x06, 0x67, 0xdf, 0xf4, 0x14, 0xdf, 0x59, 0xba};

    const std::vector<std::uint8_t> altAck1WithStatus{
      0x01, 0x60, 0x1b, 0x00, 0xd2, 0x22, 0x26, 0xed, 0xed, 0x5f, 0xdd, 0x76, 0x26,
      0x53, 0x1a, 0xbc, 0x60, 0x06, 0x5f, 0x23, 0x4c, 0x9c, 0xac, 0xed, 0x9a, 0xc3,
      0xd6, 0xb2, 0x5c};
    const std::vector<std::uint8_t> altAck1NoStatus{
      0x1b, 0x00, 0xd2, 0x22, 0x26, 0xed, 0xed, 0x5f, 0xdd, 0x76, 0x26,
      0x53, 0x1a, 0xbc, 0x60, 0x06, 0x5f, 0x23, 0x4c, 0x9c, 0xac, 0xed, 0x9a, 0xc3,
      0xd6, 0xb2, 0x5c};
    const std::vector<std::uint8_t> altAck2WithStatus{0x01, 0x60, 0x0b, 0x00, 0x66, 0xe7, 0x74, 0x80, 0x8a, 0xae, 0x28, 0xd8, 0xaa};
    const std::vector<std::uint8_t> altAck2NoStatus{0x0b, 0x00, 0x66, 0xe7, 0x74, 0x80, 0x8a, 0xae, 0x28, 0xd8, 0xaa};
    const std::vector<std::uint8_t> altAck3WithStatus{0x01, 0x60, 0x07, 0x00, 0x02, 0x00, 0x02, 0x00, 0x07};
    const std::vector<std::uint8_t> altAck3NoStatus{0x07, 0x00, 0x02, 0x00, 0x02, 0x00, 0x07};

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

      miniAltProfile_ = false;
      if (miniAltInitEnabled()) {
        if (write(alt1) != STATUS_NOERROR || !waitForReply({altAck1WithStatus, altAck1NoStatus}, 900) ||
            write(alt2) != STATUS_NOERROR || !waitForReply({altAck2WithStatus, altAck2NoStatus}, 400) ||
            write(alt3) != STATUS_NOERROR || !waitForReply({altAck3WithStatus, altAck3NoStatus}, 400)) {
          serialLog("mini alt-init attempt %d failed", attempt);
          continue;
        }

        miniVciReady_ = true;
        miniAltProfile_ = true;
        nextMiniTickleAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(220);
        serialLog("mini bootstrap completed via alt-init (attempt %d)", attempt);
        return STATUS_NOERROR;
      }

      if (write(start3) != STATUS_NOERROR ||
          !waitForReply({ack3WithStatus, ack3NoStatus}, 900)) {
        serialLog("mini bootstrap stage3 attempt %d failed", attempt);
        continue;
      }

      if (miniPostBootstrapScriptEnabled()) {
        const bool postOk = runMiniPostBootstrapReplay();
        if (!postOk && miniPostBootstrapScriptStrictEnabled()) {
          continue;
        }
      }

      miniVciReady_ = true;
      miniAltProfile_ = false;
      nextMiniTickleAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(220);
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
  bool miniAltProfile_{false};
  bool transformedReplayStateSeen_{false};
  bool transitionProbesDone_{false};
  std::uint32_t icvmWriteCount_{0};
  std::uint32_t transitionProbeRunCount_{0};
  std::uint32_t transition7d25FollowupCount_{0};
  std::uint8_t lastTransitionProbeCode_{0};
  bool lastTransitionProbeCodeValid_{false};
  bool miniIcvmPrimeAttempted_{false};
  bool miniReplayBeforeIcvmDone_{false};
  bool icvmAwaitingResponse_{false};
  std::vector<std::uint8_t> lastIcvmTx_;
  std::string serialPath_;
  unsigned int currentSerialBaud_{0U};
  std::chrono::steady_clock::time_point nextMiniTickleAt_{};
};

} // namespace

std::unique_ptr<ITransport> createSerialTransport() {
  return std::make_unique<SerialTransport>();
}

} // namespace mvci
