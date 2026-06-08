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
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <IOKit/serial/ioss.h>
#endif

namespace mvci {
namespace {

bool boolEnv(const char* name, bool fallback) {
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') {
    return fallback;
  }
  return value[0] != '0';
}

unsigned long ulongEnv(const char* name, unsigned long fallback, unsigned long minimum = 0) {
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const auto parsed = std::strtoul(value, &end, 10);
  if (end == value || parsed == 0UL) {
    return fallback;
  }
  return parsed < minimum ? minimum : parsed;
}

bool verbose() {
  static const bool enabled = boolEnv("MVCI_VERBOSE_SERIAL", false);
  return enabled;
}

template <typename... Args>
void slog(const char* fmt, Args... args) {
  if (!verbose()) {
    return;
  }
  std::fprintf(stderr, "[mvci serial] ");
  std::fprintf(stderr, fmt, args...);
  std::fputc('\n', stderr);
}

void slog(const char* msg) {
  if (!verbose()) {
    return;
  }
  std::fprintf(stderr, "[mvci serial] %s\n", msg);
}

// Always-on info logging for key lifecycle (bootstrap complete) and protocol
// events (MVCI-framed J2534 data writes/reads for ISO15765 etc.). This is
// intentionally *not* gated so that `./read_dtcs` (and similar) show what is
// happening against real hardware. Verbose-only logs (full hex dumps, most
// bootstrap steps) remain behind MVCI_VERBOSE_SERIAL.
template <typename... Args>
void serialInfo(const char* fmt, Args... args) {
  std::fprintf(stderr, "[mvci serial] ");
  std::fprintf(stderr, fmt, args...);
  std::fputc('\n', stderr);
}

void serialInfo(const char* msg) {
  std::fprintf(stderr, "[mvci serial] %s\n", msg);
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
  return static_cast<unsigned int>(ulongEnv("MVCI_SERIAL_BAUD", 500000UL));
}

std::uint32_t openRetryMs() {
  static const auto value = static_cast<std::uint32_t>(ulongEnv("MVCI_SERIAL_OPEN_RETRY_MS", 3000UL, 150UL));
  return value;
}

enum class CtrlMode { Assert, None, Pulse };

CtrlMode ctrlMode() {
  const char* value = std::getenv("MVCI_SERIAL_CTRL_MODE");
  if (!value || value[0] == '\0') {
    return boolEnv("MVCI_SERIAL_ASSERT_CTRL", true) ? CtrlMode::Assert : CtrlMode::None;
  }
  if (std::strcmp(value, "none") == 0) return CtrlMode::None;
  if (std::strcmp(value, "pulse") == 0) return CtrlMode::Pulse;
  return CtrlMode::Assert;
}

bool ctrlModeUserConfigured() {
  const char* value = std::getenv("MVCI_SERIAL_CTRL_MODE");
  return value && value[0] != '\0';
}

void setModemBits(int fd, bool dtr, bool rts) {
  int modem = 0;
  if (::ioctl(fd, TIOCMGET, &modem) != 0) {
    return;
  }
  if (dtr) modem |= TIOCM_DTR; else modem &= ~TIOCM_DTR;
  if (rts) modem |= TIOCM_RTS; else modem &= ~TIOCM_RTS;
  (void)::ioctl(fd, TIOCMSET, &modem);
}

void applyCtrlMode(int fd, CtrlMode mode) {
  switch (mode) {
    case CtrlMode::Assert:
      setModemBits(fd, true, true);
      return;
    case CtrlMode::None:
      setModemBits(fd, false, false);
      return;
    case CtrlMode::Pulse:
      setModemBits(fd, false, false);
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      setModemBits(fd, true, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      return;
  }
}

std::string hexString(const std::vector<std::uint8_t>& bytes, std::size_t maxBytes = 48U) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  const auto count = std::min(bytes.size(), maxBytes);
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0) oss << ' ';
    oss << std::setw(2) << static_cast<int>(bytes[i]);
  }
  if (bytes.size() > maxBytes) {
    oss << " ...";
  }
  return oss.str();
}

bool contains(const std::vector<std::uint8_t>& haystack,
              const std::vector<std::uint8_t>& needle) {
  if (needle.empty() || haystack.size() < needle.size()) {
    return false;
  }
  return std::search(haystack.begin(), haystack.end(),
                     needle.begin(), needle.end()) != haystack.end();
}

// Some Mini-VCI replies are XOR-obfuscated with a 3-byte repeating key after
// the first byte; detect by partial header match against `ICVM`/`ICWM`.
bool tryDecodeObfuscatedIcvm(std::vector<std::uint8_t>& bytes) {
  if (bytes.size() < 4U || (bytes[0] & 0x7FU) != 0x49U) {
    return false;
  }
  static constexpr std::uint8_t kKey[3] = {0x88U, 0xFAU, 0x78U};

  std::vector<std::uint8_t> decoded(bytes);
  decoded[0] = 0x49U;
  for (std::size_t i = 1; i < decoded.size(); ++i) {
    decoded[i] ^= kKey[(i - 1U) % 3U];
  }

  const bool looksLikeHeader = decoded[1] == 0x43U && decoded[3] == 0x4DU &&
                               (decoded[2] == 0x56U || decoded[2] == 0x57U);
  if (!looksLikeHeader) {
    return false;
  }
  bytes = std::move(decoded);
  return true;
}

std::vector<std::string> findSerialNodes() {
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
  const auto nodes = findSerialNodes();
  if (nodes.empty()) {
    return {};
  }
  if (std::find(nodes.begin(), nodes.end(), originalPath) != nodes.end()) {
    return originalPath;
  }
  const bool wantSerial = originalPath.find("usbserial") != std::string::npos;
  const bool wantModem = originalPath.find("usbmodem") != std::string::npos;
  for (const auto& node : nodes) {
    if (wantSerial && node.find("usbserial") != std::string::npos) return node;
    if (wantModem && node.find("usbmodem") != std::string::npos) return node;
  }
  return nodes.front();
}

int openSerialNode(const std::string& initialPath, std::string& openedPath) {
  std::string currentPath = initialPath;
  int lastErrno = 0;
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    const int fd = ::open(currentPath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd >= 0) {
      openedPath = currentPath;
      return fd;
    }
    lastErrno = errno;
    if (lastErrno == ENOENT || lastErrno == ETIMEDOUT || lastErrno == ENXIO || lastErrno == EIO) {
      const auto alt = pickReenumeratedNode(initialPath);
      if (!alt.empty() && alt != currentPath) {
        currentPath = alt;
      }
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    if (static_cast<std::uint32_t>(elapsedMs) >= openRetryMs()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }
  errno = lastErrno;
  openedPath.clear();
  return -1;
}

Status configurePort(int fd, unsigned int baud) {
  termios tty{};
  if (::tcgetattr(fd, &tty) != 0) {
    slog("tcgetattr failed: %s", std::strerror(errno));
    return ERR_FAILED;
  }

  cfmakeraw(&tty);
  tty.c_cflag |= (CLOCAL | CREAD);
  tty.c_cflag &= ~CSIZE;
  tty.c_cflag |= CS8;
  tty.c_cflag &= ~PARENB;
  tty.c_cflag &= ~CSTOPB;
  if (boolEnv("MVCI_SERIAL_RTSCTS", false)) {
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
    slog("tcsetattr failed: %s", std::strerror(errno));
    return ERR_FAILED;
  }

#if defined(__APPLE__)
  if (standard == 0) {
    const speed_t custom = static_cast<speed_t>(baud);
    if (::ioctl(fd, IOSSIOSPEED, &custom) < 0) {
      slog("IOSSIOSPEED %u failed: %s", baud, std::strerror(errno));
      return ERR_FAILED;
    }
  }
#else
  if (standard == 0) {
    slog("non-standard baud %u not supported on this platform", baud);
  }
#endif

  ::tcflush(fd, TCIOFLUSH);
  return STATUS_NOERROR;
}

// Mini-VCI handshake sequences (validated against MVCI32.dll USB capture):
const std::vector<std::uint8_t> kStartStage1{0x03, 0x00, 0x03};
const std::vector<std::uint8_t> kStartStage2{
    0x0c, 0x00, 0x07, 0x00, 0x01,
    0x4d, 0x56, 0x43, 0x49, 0x2d, 0x54, 0x62}; // "MVCI-Tb"
const std::vector<std::uint8_t> kStartStage3{
    0x13, 0x00, 0xd0, 0x4d, 0x01, 0xf7, 0x76, 0x39, 0x07, 0x6b,
    0x27, 0x40, 0xea, 0x48, 0xfd, 0x6e, 0xa4, 0xa9, 0x00};

const std::vector<std::uint8_t> kAck1FtdiStatus{0x01, 0x60};
const std::vector<std::uint8_t> kAck2WithStatus{
    0x01, 0x60, 0x0e, 0x00, 0x09, 0x00, 0x01, 0xb0,
    0xcb, 0x49, 0x68, 0x07, 0x45, 0xc8, 0x7f, 0xa9};
const std::vector<std::uint8_t> kAck2NoStatus{
    0x0e, 0x00, 0x09, 0x00, 0x01, 0xb0,
    0xcb, 0x49, 0x68, 0x07, 0x45, 0xc8, 0x7f, 0xa9};
const std::vector<std::uint8_t> kAck3WithStatus{
    0x01, 0x60, 0x0b, 0x00, 0x71, 0x08, 0x8e,
    0x8d, 0x8d, 0xa6, 0xaa, 0xdf, 0x2f};
const std::vector<std::uint8_t> kAck3NoStatus{
    0x0b, 0x00, 0x71, 0x08, 0x8e,
    0x8d, 0x8d, 0xa6, 0xaa, 0xdf, 0x2f};

const std::vector<std::uint8_t> kKeepaliveOut{
    0x0b, 0x00, 0x31, 0x18, 0x19, 0x2b, 0x97, 0x53, 0x24, 0xce, 0x3e};
const std::vector<std::uint8_t> kKeepaliveAckWithStatus{
    0x01, 0x60, 0x0b, 0x00, 0x64, 0x3b, 0x58, 0x62, 0x53, 0xa7, 0xd6, 0x65, 0x29};
const std::vector<std::uint8_t> kKeepaliveAckNoStatus{
    0x0b, 0x00, 0x64, 0x3b, 0x58, 0x62, 0x53, 0xa7, 0xd6, 0x65, 0x29};

const std::vector<std::uint8_t> kTickleQueryOut{
    0x0b, 0x00, 0x71, 0xa1, 0xe8, 0x84, 0xc4, 0xa2, 0x9c, 0xe0, 0xad};
const std::vector<std::uint8_t> kTickleAckWithStatus{
    0x01, 0x60, 0x0b, 0x00, 0xd2, 0xb9, 0x82, 0x0d, 0x1c, 0x58, 0x7c, 0xb4, 0x63};
const std::vector<std::uint8_t> kTickleAckNoStatus{
    0x0b, 0x00, 0xd2, 0xb9, 0x82, 0x0d, 0x1c, 0x58, 0x7c, 0xb4, 0x63};

// The 12-step post-bootstrap "unlock" sequence captured from MVCI32.dll.
// The two-byte (0x01, 0x60) prefix on each expected response is the FTDI bulk
// status byte — VCP drivers usually strip it, so both forms are accepted.
struct ReplayStep {
  std::vector<std::uint8_t> request;
  std::vector<std::vector<std::uint8_t>> expected;
};

const std::vector<ReplayStep>& replaySteps() {
  static const auto* steps = new std::vector<ReplayStep>{
    {{0x0b, 0x00, 0x25, 0x8a, 0x95, 0x1b, 0xe3, 0x6d, 0xfa, 0x9e, 0xc0},
     {{0x01, 0x60, 0x0b, 0x00, 0xa3, 0xc7, 0xc2, 0x27, 0xd0, 0x0b, 0x16, 0x50, 0x17},
      {0x0b, 0x00, 0xa3, 0xc7, 0xc2, 0x27, 0xd0, 0x0b, 0x16, 0x50, 0x17}}},
    {{0x0b, 0x00, 0x71, 0xa1, 0xe8, 0x84, 0xc4, 0xa2, 0x9c, 0xe0, 0xad},
     {{0x01, 0x60, 0x0b, 0x00, 0xd2, 0xb9, 0x82, 0x0d, 0x1c, 0x58, 0x7c, 0xb4, 0x63},
      {0x0b, 0x00, 0xd2, 0xb9, 0x82, 0x0d, 0x1c, 0x58, 0x7c, 0xb4, 0x63}}},
    {{0x23, 0x00, 0xfb, 0xb3, 0xd4, 0x3c, 0xbb, 0x46, 0x84, 0xb2, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
      0x4d, 0x98, 0x44, 0xdb, 0xf5, 0x35, 0x3a, 0x31, 0xed, 0x0c, 0x3a, 0x3e, 0x04, 0xf8, 0xc1, 0x6b,
      0x73, 0x90, 0xc6},
     {{0x01, 0x60, 0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70},
      {0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70}}},
    {{0x23, 0x00, 0x6a, 0x85, 0xd0, 0x98, 0x32, 0xea, 0x3d, 0x1e, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
      0x4d, 0x98, 0x9a, 0x28, 0x9a, 0xd9, 0x38, 0x5d, 0x0b, 0x6f, 0x00, 0xb4, 0xee, 0x06, 0x45, 0xdd,
      0xf5, 0xb3, 0x87},
     {{0x01, 0x60, 0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70},
      {0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70}}},
    {{0x23, 0x00, 0x78, 0xa4, 0x9f, 0x19, 0x4b, 0xcd, 0x31, 0xaa, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
      0x4d, 0x98, 0xe5, 0x1a, 0x46, 0x7a, 0x60, 0x37, 0xe2, 0x64, 0x8d, 0x55, 0xee, 0x6f, 0x97, 0xf3,
      0x36, 0xc0, 0x37},
     {{0x01, 0x60, 0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70},
      {0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70}}},
    {{0x23, 0x00, 0x0d, 0xc4, 0x7f, 0x17, 0xbd, 0x49, 0x42, 0x01, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
      0x4d, 0x98, 0x43, 0x4f, 0x23, 0xad, 0x5d, 0x7a, 0xcb, 0xaf, 0x5e, 0xdf, 0xea, 0xb2, 0x31, 0xca,
      0x79, 0xb6, 0x93},
     {{0x01, 0x60, 0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70},
      {0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70}}},
    {{0x23, 0x00, 0xdc, 0xb5, 0xa5, 0x84, 0x50, 0xc3, 0xea, 0x72, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
      0x4d, 0x98, 0xca, 0xb6, 0x74, 0x6b, 0xc1, 0x6b, 0xa9, 0x9c, 0xd8, 0x54, 0x2d, 0x0c, 0xf8, 0x2c,
      0x6c, 0xd7, 0xd4},
     {{0x01, 0x60, 0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70},
      {0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70}}},
    {{0x23, 0x00, 0xc1, 0xdc, 0x74, 0x4e, 0xb4, 0xda, 0x05, 0x60, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
      0x4d, 0x98, 0x31, 0xd0, 0x0a, 0xcb, 0x0d, 0x37, 0xf9, 0x0a, 0x81, 0x1d, 0xc6, 0x5d, 0x8d, 0xad,
      0x33, 0xa0, 0xd8},
     {{0x01, 0x60, 0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70},
      {0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70}}},
    {{0x23, 0x00, 0x4c, 0xc0, 0x0d, 0xc4, 0x1d, 0x6e, 0x14, 0x9a, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
      0x4d, 0x98, 0x8a, 0xaa, 0xdf, 0xe0, 0x00, 0x42, 0x34, 0x15, 0xe1, 0x6f, 0xa9, 0x96, 0xf0, 0x95,
      0xfc, 0x69, 0x2c},
     {{0x01, 0x60, 0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70},
      {0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70}}},
    {{0x23, 0x00, 0x89, 0x87, 0x3c, 0x4f, 0x1d, 0x6b, 0xef, 0x52, 0x7c, 0xbc, 0x8d, 0x47, 0x05, 0x50,
      0x4d, 0x98, 0xa3, 0x3c, 0xef, 0xc3, 0x31, 0x82, 0x62, 0xf2, 0x74, 0x9b, 0x98, 0xf2, 0x25, 0x20,
      0x5b, 0x4b, 0x1f},
     {{0x01, 0x60, 0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70},
      {0x0b, 0x00, 0x1a, 0x3f, 0xb2, 0x62, 0x6f, 0x47, 0xde, 0x78, 0x70}}},
    {{0x0b, 0x00, 0x71, 0xa1, 0xe8, 0x84, 0xc4, 0xa2, 0x9c, 0xe0, 0xad},
     {{0x01, 0x60, 0x0b, 0x00, 0xd2, 0xb9, 0x82, 0x0d, 0x1c, 0x58, 0x7c, 0xb4, 0x63},
      {0x0b, 0x00, 0xd2, 0xb9, 0x82, 0x0d, 0x1c, 0x58, 0x7c, 0xb4, 0x63}}},
    {{0x1b, 0x00, 0xff, 0xe0, 0xff, 0x28, 0x6b, 0xdf, 0xfa, 0x7c, 0x27, 0x28, 0xfb, 0x14, 0x1f, 0xca,
      0xab, 0xfe, 0xbf, 0x98, 0x58, 0x87, 0xe6, 0x29, 0xc4, 0x5e, 0x2c},
     {{0x01, 0x60, 0x0b, 0x00, 0xb6, 0x19, 0xc7, 0xf0, 0xb5, 0xc3, 0xbd, 0xf8, 0xa0},
      {0x0b, 0x00, 0xb6, 0x19, 0xc7, 0xf0, 0xb5, 0xc3, 0xbd, 0xf8, 0xa0}}},
    {{0x0b, 0x00, 0x31, 0x18, 0x19, 0x2b, 0x97, 0x53, 0x24, 0xce, 0x3e},
     {{0x01, 0x60, 0x0b, 0x00, 0x64, 0x3b, 0x58, 0x62, 0x53, 0xa7, 0xd6, 0x65, 0x29},
      {0x0b, 0x00, 0x64, 0x3b, 0x58, 0x62, 0x53, 0xa7, 0xd6, 0x65, 0x29}}},
  };
  return *steps;
}

class SerialTransport final : public ITransport {
public:
  Status open(const std::string& deviceName) override {
    close();

    std::string path = deviceName;
    if (constexpr const char* kPrefix = "serial:"; path.rfind(kPrefix, 0) == 0) {
      path.erase(0, std::strlen(kPrefix));
    }
    if (path.empty() || path[0] != '/') {
      slog("invalid serial path '%s'", path.c_str());
      return ERR_FAILED;
    }

    std::string openedPath;
    fd_ = openSerialNode(path, openedPath);
    if (fd_ < 0) {
      slog("open('%s') failed: %s", path.c_str(), std::strerror(errno));
      return ERR_FAILED;
    }
    if (!openedPath.empty() && openedPath != path) {
      slog("serial node moved from '%s' to '%s'", path.c_str(), openedPath.c_str());
      path = openedPath;
    }
    serialPath_ = path;

    // Clear O_NONBLOCK so subsequent reads honor VMIN/VTIME via select.
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags >= 0) {
      ::fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);
    }
    if (::ioctl(fd_, TIOCEXCL) < 0) {
      slog("TIOCEXCL failed on '%s': %s (continuing)", path.c_str(), std::strerror(errno));
    }

    const auto baud = desiredBaud();
    if (configurePort(fd_, baud) != STATUS_NOERROR) {
      closeFd();
      return ERR_FAILED;
    }
    currentBaud_ = baud;
    open_ = true;
    slog("opened '%s' at %u baud", path.c_str(), baud);

    if (boolEnv("MVCI_MINIVCI_BOOTSTRAP", true)) {
      auto bootstrap = bootstrapWithFallback(baud, "open");

      // If the very first bootstrap closed the fd (e.g. transient I/O error
      // during a port re-enumeration), reopen and retry once.
      if (bootstrap != STATUS_NOERROR && fd_ < 0 && !serialPath_.empty()) {
        slog("bootstrap aborted with closed fd; reopening '%s' and retrying",
             serialPath_.c_str());
        if (reopenFd()) {
          miniReady_ = false;
          bootstrap = bootstrapWithFallback(baud, "open-retry");
        }
      }
      if (bootstrap != STATUS_NOERROR && boolEnv("MVCI_MINIVCI_BOOTSTRAP_STRICT", false)) {
        slog("mini bootstrap failed; rejecting serial node");
        close();
        return ERR_FAILED;
      }
    }
    return STATUS_NOERROR;
  }

  void close() override {
    closeFd();
    open_ = false;
    rxBuffer_.clear();
    miniReady_ = false;
    currentBaud_ = 0U;
    serialPath_.clear();
  }

  Status write(const std::vector<std::uint8_t>& packet) override {
    if (!open_ || fd_ < 0) {
      return ERR_NOT_INITIALIZED;
    }

    const bool icvm = isIcvmPacket(packet);
    if (icvm && !miniReady_) {
      if (!boolEnv("MVCI_MINIVCI_BOOTSTRAP", true)) {
        slog("rejecting icvm write: mini transport not ready");
        return ERR_NOT_INITIALIZED;
      }
      const auto baud = currentBaud_ ? currentBaud_ : desiredBaud();
      slog("icvm write while mini not ready; attempting bootstrap");
      if (bootstrapWithFallback(baud, "write") != STATUS_NOERROR) {
        slog("rejecting icvm write: mini transport bootstrap failed");
        return ERR_NOT_INITIALIZED;
      }
      slog("icvm write bootstrap completed");
    }

    if (miniReady_) {
      // Refresh the Mini-VCI session/keepalive bridge before *any* post-bootstrap
      // traffic (both the special ICVM control packets and regular MVCI-framed
      // J2534 data such as ISO15765 writes). Some clones appear to need recent
      // keepalives for subsequent CAN commands to be accepted.
      if (boolEnv("MVCI_MINIVCI_SESSION_TICKLE", true)) {
        (void)sendSessionTickle();
      }
      if (boolEnv("MVCI_MINIVCI_KEEPALIVE_BRIDGE", true)) {
        (void)sendKeepalive();
      }
    }

    if (packet.size() >= 4) {
      const bool isMvci = (packet[0] == 0x4D && packet[1] == 0x56 && packet[2] == 0x43 && packet[3] == 0x49);
      if (isMvci && packet.size() >= 24) {
        const auto ch = static_cast<unsigned>(packet[4]) |
                        (static_cast<unsigned>(packet[5]) << 8) |
                        (static_cast<unsigned>(packet[6]) << 16) |
                        (static_cast<unsigned>(packet[7]) << 24);
        const auto proto = static_cast<unsigned>(packet[8]) |
                           (static_cast<unsigned>(packet[9]) << 8) |
                           (static_cast<unsigned>(packet[10]) << 16) |
                           (static_cast<unsigned>(packet[11]) << 24);
        const auto flags = static_cast<unsigned>(packet[12]) |
                           (static_cast<unsigned>(packet[13]) << 8) |
                           (static_cast<unsigned>(packet[14]) << 16) |
                           (static_cast<unsigned>(packet[15]) << 24);
        const auto psz = static_cast<unsigned>(packet[20]) |
                         (static_cast<unsigned>(packet[21]) << 8) |
                         (static_cast<unsigned>(packet[22]) << 16) |
                         (static_cast<unsigned>(packet[23]) << 24);
        serialInfo("MVCI write ch=%u proto=%u flags=0x%08x psz=%u firstData=%02x",
                   ch, proto, flags, psz, (packet.size() > 24 ? packet[24] : 0));
      } else if (icvm) {
        // ICVM data/command carrying a (usually CAN) frame toward the vehicle.
        // Observed layout for data writes: [0..3]=ICVM magic, later a length field
        // followed by 4-byte CAN ID (big-endian as produced by withCanIdPrefix) then
        // the UDS/ISO-TP payload. We make a best-effort decode so that UDS requests
        // (VIN/DTC) are visible at the unconditional [mvci serial] level.
        std::uint32_t maybeId = 0;
        std::size_t payloadOff = 0;
        if (packet.size() > 32) {
          // Common observed offset after the variable header + 08 00 00 00 length word.
          maybeId = (static_cast<std::uint32_t>(packet[28]) << 0) |
                    (static_cast<std::uint32_t>(packet[29]) << 8) |
                    (static_cast<std::uint32_t>(packet[30]) << 16) |
                    (static_cast<std::uint32_t>(packet[31]) << 24);
          payloadOff = 32;
        }
        if (maybeId == 0U) {
          // Fallback: hunt for 00 00 07 xx (typical 11-bit OBD ID with our prefix style).
          for (std::size_t i = 4; i + 4 < packet.size(); ++i) {
            if (packet[i] == 0U && packet[i + 1] == 0U && (packet[i + 2] & 0xF8U) == 0x07U) {
              maybeId = (static_cast<std::uint32_t>(packet[i + 2]) << 0) |
                        (static_cast<std::uint32_t>(packet[i + 3]) << 8);
              payloadOff = i + 4;
              break;
            }
          }
        }
        if (maybeId != 0U) {
          char first[48] = {};
          std::snprintf(first, sizeof(first), "%02x %02x %02x %02x",
                        (payloadOff + 0 < packet.size() ? packet[payloadOff + 0] : 0),
                        (payloadOff + 1 < packet.size() ? packet[payloadOff + 1] : 0),
                        (payloadOff + 2 < packet.size() ? packet[payloadOff + 2] : 0),
                        (payloadOff + 3 < packet.size() ? packet[payloadOff + 3] : 0));
          serialInfo("ICVM data write id=0x%03x first=[%s]", static_cast<unsigned>(maybeId), first);
        } else if (verbose()) {
          slog("ICVM write (no CAN id decoded) %zu bytes", packet.size());
        }
      } else if (!icvm && verbose()) {
        slog("write non-ICVM/non-MVCI packet (%zu bytes) first=%02x", packet.size(), packet[0]);
      }
    }

    return writeBytes(packet);
  }

  Status read(std::vector<std::uint8_t>& packet, std::uint32_t timeoutMs) override {
    if (!open_ || fd_ < 0) {
      return ERR_NOT_INITIALIZED;
    }
    packet.clear();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);

    for (;;) {
      if (tryExtractMvcIFrame(rxBuffer_, packet)) {
        if (packet.size() >= 24) {
          const bool isMvci = (packet[0] == 0x4D && packet[1] == 0x56 && packet[2] == 0x43 && packet[3] == 0x49);
          if (isMvci) {
            const auto ch = static_cast<unsigned>(packet[4]) |
                            (static_cast<unsigned>(packet[5]) << 8) |
                            (static_cast<unsigned>(packet[6]) << 16) |
                            (static_cast<unsigned>(packet[7]) << 24);
            const auto proto = static_cast<unsigned>(packet[8]) |
                               (static_cast<unsigned>(packet[9]) << 8) |
                               (static_cast<unsigned>(packet[10]) << 16) |
                               (static_cast<unsigned>(packet[11]) << 24);
            const auto psz = static_cast<unsigned>(packet[20]) |
                             (static_cast<unsigned>(packet[21]) << 8) |
                             (static_cast<unsigned>(packet[22]) << 16) |
                             (static_cast<unsigned>(packet[23]) << 24);
            const unsigned first = (packet.size() > 24 ? packet[24] : 0);
            serialInfo("MVCI read ch=%u proto=%u psz=%u firstData=%02x (rxbuf left=%zu)",
                       ch, proto, psz, first, rxBuffer_.size());
          }
        }
        if (verbose() && packet.size() >= 24) {
          // verbose may add more context; the summary is already emitted above
        }
        return STATUS_NOERROR;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return ERR_TIMEOUT;
      }
      const auto remaining = static_cast<std::uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
      std::vector<std::uint8_t> chunk;
      const auto status = readChunk(chunk, std::min<std::uint32_t>(remaining, 120U));
      if (status == ERR_TIMEOUT) {
        if (miniReady_ && boolEnv("MVCI_MINIVCI_SESSION_TICKLE", true)) {
          maybeTickleSession();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          serialInfo("read timeout after %u ms (rx_buffer=%zu, mini_ready=%d)",
                     timeoutMs, rxBuffer_.size(), miniReady_ ? 1 : 0);
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

  Status controlTransfer(std::uint8_t, std::uint8_t,
                         std::uint16_t, std::uint16_t,
                         std::vector<std::uint8_t>&, std::uint32_t) override {
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

  Status writeBytes(const std::vector<std::uint8_t>& packet) {
    if (!open_ || fd_ < 0) {
      return ERR_NOT_INITIALIZED;
    }
    std::size_t offset = 0;
    while (offset < packet.size()) {
      const auto written = ::write(fd_, packet.data() + offset, packet.size() - offset);
      if (written < 0) {
        if (errno == EINTR) continue;
        if (tryRecoverDevice("write", errno)) continue;
        slog("write failed: %s", std::strerror(errno));
        return ERR_FAILED;
      }
      offset += static_cast<std::size_t>(written);
    }
    if (verbose() && !packet.empty()) {
      slog("tx[%zu]: %s", packet.size(), hexString(packet).c_str());
    }
    return STATUS_NOERROR;
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
        if (attempt == 0 && tryRecoverDevice("select", errno)) continue;
        slog("select failed: %s", std::strerror(errno));
        return ERR_FAILED;
      }
      if (ready == 0) {
        return ERR_TIMEOUT;
      }

      unsigned char buffer[512];
      const auto got = ::read(fd_, buffer, sizeof(buffer));
      if (got < 0) {
        if (errno == EINTR) return ERR_TIMEOUT;
        if (attempt == 0 && tryRecoverDevice("read", errno)) continue;
        slog("read failed: %s", std::strerror(errno));
        return ERR_FAILED;
      }
      if (got == 0) {
        return ERR_TIMEOUT;
      }

      out.assign(buffer, buffer + got);
      if (tryDecodeObfuscatedIcvm(out)) {
        slog("decoded mini obfuscated icvm frame");
      }
      if (verbose()) {
        slog("rx[%zu]: %s", out.size(), hexString(out).c_str());
      }
      return STATUS_NOERROR;
    }
    return ERR_FAILED;
  }

  static bool isIcvmPacket(const std::vector<std::uint8_t>& packet) {
    return packet.size() >= 4 &&
           packet[0] == 0x49 && packet[1] == 0x43 &&
           packet[2] == 0x56 && packet[3] == 0x4d;
  }

  bool reopenFd() {
    closeFd();
    std::string reopenedPath;
    const int fd = openSerialNode(serialPath_, reopenedPath);
    if (fd < 0) {
      slog("reopen('%s') failed: %s", serialPath_.c_str(), std::strerror(errno));
      return false;
    }
    if (!reopenedPath.empty() && reopenedPath != serialPath_) {
      slog("serial node moved from '%s' to '%s'", serialPath_.c_str(), reopenedPath.c_str());
      serialPath_ = reopenedPath;
    }
    fd_ = fd;
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags >= 0) {
      ::fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);
    }
    (void)::ioctl(fd_, TIOCEXCL);
    return true;
  }

  bool tryRecoverDevice(const char* source, int err) {
    if (err != ENODEV && err != ENXIO && err != EIO &&
        err != ENOTCONN && err != EBADF) {
      return false;
    }
    if (serialPath_.empty()) {
      return false;
    }
    slog("serial recover from %s after %s", source, std::strerror(err));
    if (!reopenFd()) {
      return false;
    }

    const auto baud = currentBaud_ ? currentBaud_ : desiredBaud();
    if (configurePort(fd_, baud) != STATUS_NOERROR) {
      slog("serial recover configure failed");
      closeFd();
      return false;
    }
    currentBaud_ = baud;
    miniReady_ = false;

    if (boolEnv("MVCI_MINIVCI_BOOTSTRAP", true) &&
        bootstrapWithFallback(baud, "recover") != STATUS_NOERROR) {
      slog("serial recover mini bootstrap failed");
      closeFd();
      return false;
    }
    slog("serial recovered on '%s' at %u baud", serialPath_.c_str(), currentBaud_);
    return true;
  }

  Status bootstrapWithFallback(unsigned int configuredBaud, const char* source) {
    if (fd_ < 0) {
      return ERR_NOT_INITIALIZED;
    }

    auto tryWithCtrl = [&](CtrlMode mode) -> Status {
      if (fd_ < 0) return ERR_NOT_INITIALIZED;
      applyCtrlMode(fd_, mode);
      slog("serial ctrl mode: %s",
           mode == CtrlMode::Assert ? "assert" :
           mode == CtrlMode::Pulse  ? "pulse"  : "none");

      if (bootstrap() == STATUS_NOERROR) {
        return STATUS_NOERROR;
      }
      if (fd_ < 0) return ERR_NOT_INITIALIZED;

      for (unsigned int alt : {500000U, 230400U, 115200U, 38400U}) {
        if (alt == currentBaud_ || fd_ < 0) continue;
        if (configurePort(fd_, alt) != STATUS_NOERROR) continue;
        currentBaud_ = alt;
        slog("%s retrying mini bootstrap at %u baud", source, alt);
        if (bootstrap() == STATUS_NOERROR) {
          slog("mini bootstrap succeeded at %u baud", alt);
          return STATUS_NOERROR;
        }
        if (fd_ < 0) return ERR_NOT_INITIALIZED;
      }
      return ERR_FAILED;
    };

    if (configurePort(fd_, configuredBaud) == STATUS_NOERROR) {
      currentBaud_ = configuredBaud;
    }

    const auto preferred = ctrlMode();
    auto status = tryWithCtrl(preferred);
    if (status != STATUS_NOERROR && fd_ < 0) {
      return status;
    }

    // If the user didn't pin a ctrl mode, try alternates as a last resort.
    if (status != STATUS_NOERROR &&
        !ctrlModeUserConfigured() &&
        boolEnv("MVCI_SERIAL_CTRL_AUTO", true)) {
      for (CtrlMode mode : {CtrlMode::None, CtrlMode::Pulse, CtrlMode::Assert}) {
        if (mode == preferred || fd_ < 0) continue;
        if (configurePort(fd_, configuredBaud) == STATUS_NOERROR) {
          currentBaud_ = configuredBaud;
        }
        slog("%s retrying bootstrap with alternate ctrl mode", source);
        status = tryWithCtrl(mode);
        if (status == STATUS_NOERROR) {
          return STATUS_NOERROR;
        }
        if (fd_ < 0) return status;
      }
    }

    if (fd_ >= 0 && configurePort(fd_, configuredBaud) == STATUS_NOERROR) {
      currentBaud_ = configuredBaud;
    }
    return status;
  }

  Status bootstrap() {
    for (int attempt = 1; attempt <= 3; ++attempt) {
      clearRx();
      if (writeBytes(kStartStage1) != STATUS_NOERROR) {
        slog("mini bootstrap stage1 attempt %d write failed", attempt);
        continue;
      }
      const bool stage1Acked = waitForReply({kAck1FtdiStatus}, 350);
      if (!stage1Acked && boolEnv("MVCI_MINIVCI_STAGE1_STRICT", false)) {
        slog("mini bootstrap stage1 attempt %d failed (strict)", attempt);
        continue;
      }
      if (!stage1Acked) {
        slog("mini bootstrap stage1 attempt %d: no explicit ack; continuing", attempt);
      }

      if (writeBytes(kStartStage2) != STATUS_NOERROR ||
          !waitForReply({kAck2WithStatus, kAck2NoStatus}, 900)) {
        slog("mini bootstrap stage2 attempt %d failed", attempt);
        continue;
      }

      if (writeBytes(kStartStage3) != STATUS_NOERROR ||
          !waitForReply({kAck3WithStatus, kAck3NoStatus}, 900)) {
        slog("mini bootstrap stage3 attempt %d failed", attempt);
        continue;
      }

      if (boolEnv("MVCI_MINIVCI_POST_BOOTSTRAP", true) &&
          !runPostBootstrap() &&
          boolEnv("MVCI_MINIVCI_POST_BOOTSTRAP_STRICT", false)) {
        continue;
      }

      miniReady_ = true;
      nextTickleAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(220);
      slog("mini bootstrap completed (attempt %d)", attempt);
      serialInfo("mini bootstrap completed (attempt %d)", attempt);
      return STATUS_NOERROR;
    }
    return ERR_FAILED;
  }

  bool runPostBootstrap() {
    if (fd_ >= 0) ::tcflush(fd_, TCIOFLUSH);
    clearRx();
    const auto& steps = replaySteps();
    for (std::size_t i = 0; i < steps.size(); ++i) {
      const auto& step = steps[i];
      if (writeBytes(step.request) != STATUS_NOERROR ||
          !waitForReply(step.expected, 250)) {
        slog("mini post-bootstrap step %zu failed", i + 1);
        return false;
      }
    }
    return true;
  }

  // Keepalive / tickle (writes-through-write to refresh state during reads):
  bool sendKeepalive() {
    if (writeBytes(kKeepaliveOut) != STATUS_NOERROR) {
      return false;
    }
    const bool ok = waitForReply({kKeepaliveAckWithStatus, kKeepaliveAckNoStatus}, 120);
    if (!ok) slog("mini keepalive bridge miss");
    return ok;
  }

  bool sendSessionTickle() {
    if (writeBytes(kTickleQueryOut) != STATUS_NOERROR ||
        !waitForReply({kTickleAckWithStatus, kTickleAckNoStatus}, 120)) {
      slog("mini session tickle query miss");
      return false;
    }
    return sendKeepalive();
  }

  void maybeTickleSession() {
    const auto now = std::chrono::steady_clock::now();
    if (now < nextTickleAt_) {
      return;
    }
    (void)sendSessionTickle();
    nextTickleAt_ = now + std::chrono::milliseconds(220);
  }

  bool waitForReply(const std::vector<std::vector<std::uint8_t>>& accepted,
                    std::uint32_t timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    std::vector<std::uint8_t> stream;
    while (std::chrono::steady_clock::now() < deadline) {
      std::vector<std::uint8_t> chunk;
      const auto status = readChunk(chunk, 50);
      if (status == ERR_TIMEOUT) continue;
      if (status != STATUS_NOERROR) return false;

      stream.insert(stream.end(), chunk.begin(), chunk.end());
      if (stream.size() > 4096U) {
        stream.erase(stream.begin(), stream.end() - 1024);
      }
      for (const auto& expected : accepted) {
        if (chunk == expected || contains(stream, expected)) {
          return true;
        }
      }
    }
    return false;
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
  bool miniReady_{false};
  std::string serialPath_;
  unsigned int currentBaud_{0U};
  std::chrono::steady_clock::time_point nextTickleAt_{};
};

} // namespace

std::unique_ptr<ITransport> createSerialTransport() {
  return std::make_unique<SerialTransport>();
}

} // namespace mvci
