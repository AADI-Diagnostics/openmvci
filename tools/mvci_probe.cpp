// Standalone serial probe for Mini-VCI adapters (FTDI VCP path).
//
// Modes:
// 1) Generic probe (default): sends short candidate payloads.
// 2) PCAP profile probe (--pcap): parses USBPcap EPB records and replays
//    early OUT commands from the dominant bulk stream, then checks whether
//    expected IN signatures are observed on serial.

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <dirent.h>

#if defined(__APPLE__)
#include <IOKit/serial/ioss.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Probe {
  const char* label;
  std::vector<std::uint8_t> bytes;
};

enum class CtrlMode {
  Pulse,
  High,
  None,
};

struct TraceEvent {
  std::uint16_t dev{0};
  std::uint64_t ts{0};
  bool out{false};
  std::vector<std::uint8_t> data;
};

struct ReplayStep {
  std::vector<std::uint8_t> out;
  std::vector<std::uint8_t> expectedIn;
  int gapToNextOutMs{0};
};

std::uint16_t readLe16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0]) |
         static_cast<std::uint16_t>(p[1] << 8);
}

std::uint32_t readLe32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) |
         static_cast<std::uint32_t>(p[1] << 8) |
         static_cast<std::uint32_t>(p[2] << 16) |
         static_cast<std::uint32_t>(p[3] << 24);
}

std::uint64_t readLe64(const std::uint8_t* p) {
  return static_cast<std::uint64_t>(readLe32(p)) |
         (static_cast<std::uint64_t>(readLe32(p + 4)) << 32);
}

std::string findSerial() {
  DIR* dir = ::opendir("/dev");
  if (!dir) return {};
  std::string best;
  while (auto* e = ::readdir(dir)) {
    std::string n = e->d_name;
    if (n.rfind("cu.usbserial", 0) == 0) {
      best = "/dev/" + n;
      break;
    }
  }
  ::closedir(dir);
  return best;
}

int openPort(const std::string& path, bool exclusive) {
  std::string activePath = path;
  int fd = -1;
  int lastErrno = 0;
  for (int attempt = 1; attempt <= 20; ++attempt) {
    fd = ::open(activePath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd >= 0) {
      break;
    }
    lastErrno = errno;

    if (lastErrno == ENOENT || lastErrno == ETIMEDOUT || lastErrno == ENXIO || lastErrno == EIO) {
      const std::string discovered = findSerial();
      if (!discovered.empty()) {
        activePath = discovered;
      }
    }

    // Mini-VCI nodes can flap briefly during line-state transitions.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }
  if (fd < 0) {
    errno = lastErrno;
    return -1;
  }
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
  if (exclusive) {
    ::ioctl(fd, TIOCEXCL);
  }
  return fd;
}

bool setBaud(int fd, unsigned int baud) {
  termios tty{};
  if (::tcgetattr(fd, &tty) != 0) return false;
  cfmakeraw(&tty);
  tty.c_cflag |= (CLOCAL | CREAD);
  tty.c_cflag &= ~CSIZE; tty.c_cflag |= CS8;
  tty.c_cflag &= ~PARENB; tty.c_cflag &= ~CSTOPB; tty.c_cflag &= ~CRTSCTS;
  tty.c_cc[VMIN] = 0; tty.c_cc[VTIME] = 0;

  speed_t s = B9600;
  bool useIoss = false;
  switch (baud) {
    case 9600: s = B9600; break;
    case 19200: s = B19200; break;
    case 38400: s = B38400; break;
    case 57600: s = B57600; break;
    case 115200: s = B115200; break;
    case 230400: s = B230400; break;
    default: useIoss = true; break;
  }
  if (!useIoss) {
    cfsetispeed(&tty, s);
    cfsetospeed(&tty, s);
  } else {
    cfsetispeed(&tty, B9600);
    cfsetospeed(&tty, B9600);
  }
  if (::tcsetattr(fd, TCSANOW, &tty) != 0) return false;
#if defined(__APPLE__)
  if (useIoss) {
    speed_t custom = static_cast<speed_t>(baud);
    if (::ioctl(fd, IOSSIOSPEED, &custom) < 0) return false;
  }
#endif
  ::tcflush(fd, TCIOFLUSH);
  return true;
}

void hex(const std::vector<std::uint8_t>& b) {
  for (auto x : b) std::printf("%02X ", x);
}

bool containsSequence(const std::vector<std::uint8_t>& haystack,
                      const std::vector<std::uint8_t>& needle) {
  if (needle.empty() || haystack.size() < needle.size()) {
    return false;
  }
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

std::vector<std::uint8_t> readFor(int fd, int ms) {
  std::vector<std::uint8_t> out;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  unsigned char buf[256];
  while (std::chrono::steady_clock::now() < deadline) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) break;
    timeval tv{}; tv.tv_sec = remaining / 1000; tv.tv_usec = (remaining % 1000) * 1000;
    fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
    int r = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
    if (r <= 0) break;
    auto got = ::read(fd, buf, sizeof(buf));
    if (got <= 0) break;
    out.insert(out.end(), buf, buf + got);
  }
  return out;
}

std::vector<TraceEvent> parseBulkTraceFromPcap(const std::string& pcapPath) {
  std::ifstream in(pcapPath, std::ios::binary);
  if (!in) {
    return {};
  }
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());

  std::vector<TraceEvent> all;
  std::size_t o = 0;
  while (o + 12 <= bytes.size()) {
    const auto* p = bytes.data() + o;
    const std::uint32_t blockType = readLe32(p + 0);
    const std::uint32_t blockLen = readLe32(p + 4);
    if (blockLen < 12 || o + blockLen > bytes.size()) {
      break;
    }

    if (blockType == 0x00000006U) {
      if (blockLen >= 28) {
        const std::uint32_t capLen = readLe32(p + 20);
        const std::size_t pktOff = o + 28;
        if (pktOff + capLen <= bytes.size() && capLen >= 28) {
          const auto* pkt = bytes.data() + pktOff;
          const std::uint16_t hdrLen = readLe16(pkt + 0);
          const std::uint16_t dev = readLe16(pkt + 19);
          const std::uint8_t info = pkt[16];
          const std::uint8_t ep = pkt[21];
          const std::uint8_t xfer = pkt[22];
          const std::uint32_t dataLen = readLe32(pkt + 23);
          const std::uint64_t ts = (static_cast<std::uint64_t>(readLe32(p + 12)) << 32) |
                                   static_cast<std::uint64_t>(readLe32(p + 16));
          if (xfer == 3 && dataLen > 0) {
            const bool isOut = (ep == 0x02 && info == 0x00);
            const bool isIn = (ep == 0x81 && info == 0x01);
            if ((isOut || isIn) && hdrLen + dataLen <= capLen) {
              TraceEvent ev;
              ev.dev = dev;
              ev.ts = ts;
              ev.out = isOut;
              ev.data.assign(pkt + hdrLen, pkt + hdrLen + dataLen);
              all.push_back(std::move(ev));
            }
          }
        }
      }
    }

    o += blockLen;
  }

  if (all.empty()) {
    return {};
  }

  std::map<std::uint16_t, std::size_t> byDev;
  for (const auto& ev : all) {
    byDev[ev.dev] += 1;
  }

  std::uint16_t dominantDev = 0;
  std::size_t dominantCount = 0;
  for (const auto& [dev, count] : byDev) {
    if (count > dominantCount) {
      dominantCount = count;
      dominantDev = dev;
    }
  }

  std::vector<TraceEvent> filtered;
  filtered.reserve(dominantCount);
  for (const auto& ev : all) {
    if (ev.dev == dominantDev) {
      filtered.push_back(ev);
    }
  }
  return filtered;
}

std::vector<ReplayStep> buildReplayProfile(const std::vector<TraceEvent>& events,
                                           std::size_t startOffset,
                                           std::size_t maxSteps) {
  std::vector<ReplayStep> steps;
  if (events.empty()) {
    return steps;
  }

  const std::vector<std::uint8_t> start1{0x03, 0x00, 0x03};
  const std::vector<std::uint8_t> start2{0x0c, 0x00, 0x07, 0x00, 0x01, 0x4d, 0x56, 0x43, 0x49, 0x2d, 0x54, 0x62};
  const std::vector<std::uint8_t> start3{0x13, 0x00, 0xd0, 0x4d, 0x01, 0xf7, 0x76, 0x39, 0x07, 0x6b,
                                         0x27, 0x40, 0xea, 0x48, 0xfd, 0x6e, 0xa4, 0xa9, 0x00};

  std::vector<std::size_t> outIdx;
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (events[i].out) {
      outIdx.push_back(i);
    }
  }
  if (outIdx.size() < 3) {
    return steps;
  }

  std::size_t startOutPos = 0;
  bool found = false;
  for (std::size_t k = 0; k + 2 < outIdx.size(); ++k) {
    if (events[outIdx[k]].data == start1 &&
        events[outIdx[k + 1]].data == start2 &&
        events[outIdx[k + 2]].data == start3) {
      startOutPos = k;
      found = true;
      break;
    }
  }
  if (!found) {
    return steps;
  }

  const std::size_t firstOutPos = std::min(outIdx.size(), startOutPos + startOffset);
  const std::size_t endOutPos = std::min(outIdx.size(), firstOutPos + maxSteps);
  for (std::size_t p = firstOutPos; p < endOutPos; ++p) {
    const std::size_t outEventIdx = outIdx[p];
    const std::size_t nextOutEventIdx = (p + 1 < outIdx.size()) ? outIdx[p + 1] : events.size();
    const std::size_t nextOutIdx = (p + 1 < outIdx.size()) ? outIdx[p + 1] : outEventIdx;

    ReplayStep step;
    step.out = events[outEventIdx].data;

    if (p + 1 < outIdx.size()) {
      const auto dtUs = (events[nextOutIdx].ts > events[outEventIdx].ts)
                        ? (events[nextOutIdx].ts - events[outEventIdx].ts)
                        : 0;
      step.gapToNextOutMs = static_cast<int>(dtUs / 1000ULL);
    }

    for (std::size_t j = outEventIdx + 1; j < nextOutEventIdx; ++j) {
      if (!events[j].out && !events[j].data.empty()) {
        step.expectedIn = events[j].data;
        break;
      }
    }

    steps.push_back(std::move(step));
  }

  return steps;
}

bool replyMatches(const std::vector<std::uint8_t>& got, const std::vector<std::uint8_t>& expected) {
  if (expected.empty()) {
    return !got.empty();
  }
  if (got == expected || containsSequence(got, expected)) {
    return true;
  }

  // Serial VCP may hide FTDI status bytes 01 60 seen on USB bulk-IN.
  if (expected.size() > 2 && expected[0] == 0x01 && expected[1] == 0x60) {
    std::vector<std::uint8_t> trimmed(expected.begin() + 2, expected.end());
    if (got == trimmed || containsSequence(got, trimmed)) {
      return true;
    }
  }

  return false;
}

void writeAll(int fd, const std::vector<std::uint8_t>& b) {
  std::size_t off = 0;
  while (off < b.size()) {
    auto w = ::write(fd, b.data() + off, b.size() - off);
    if (w <= 0) break;
    off += w;
  }
}

void setLine(int fd, int bit, bool on) {
  int bits = 0;
  ::ioctl(fd, TIOCMGET, &bits);
  if (on) bits |= bit; else bits &= ~bit;
  ::ioctl(fd, TIOCMSET, &bits);
}

// Pulse DTR low->high to reset MCUs that use DTR as reset (common on
// FTDI-based Mini-VCI clones).
void resetPulse(int fd) {
  setLine(fd, TIOCM_DTR, false);
  setLine(fd, TIOCM_RTS, false);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  setLine(fd, TIOCM_DTR, true);
  setLine(fd, TIOCM_RTS, true);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
}

const char* ctrlModeName(CtrlMode mode) {
  switch (mode) {
    case CtrlMode::Pulse: return "pulse";
    case CtrlMode::High: return "high";
    case CtrlMode::None: return "none";
  }
  return "unknown";
}

void applyCtrlMode(int fd, CtrlMode mode) {
  switch (mode) {
    case CtrlMode::Pulse:
      resetPulse(fd);
      break;
    case CtrlMode::High:
      setLine(fd, TIOCM_DTR, true);
      setLine(fd, TIOCM_RTS, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      break;
    case CtrlMode::None:
      setLine(fd, TIOCM_DTR, false);
      setLine(fd, TIOCM_RTS, false);
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      break;
  }
}

CtrlMode parseCtrlMode(const std::string& mode) {
  if (mode == "high") return CtrlMode::High;
  if (mode == "none") return CtrlMode::None;
  return CtrlMode::Pulse;
}

bool parseHexBytes(const std::string& text, std::vector<std::uint8_t>& out) {
  out.clear();
  std::string cleaned;
  cleaned.reserve(text.size());
  for (char c : text) {
    if (std::isxdigit(static_cast<unsigned char>(c)) != 0) {
      cleaned.push_back(c);
    }
  }
  if (cleaned.empty() || (cleaned.size() % 2U) != 0U) {
    return false;
  }

  out.reserve(cleaned.size() / 2U);
  for (std::size_t i = 0; i < cleaned.size(); i += 2U) {
    const auto byteStr = cleaned.substr(i, 2U);
    char* end = nullptr;
    const auto value = std::strtoul(byteStr.c_str(), &end, 16);
    if (end == byteStr.c_str() || *end != '\0' || value > 0xFFU) {
      out.clear();
      return false;
    }
    out.push_back(static_cast<std::uint8_t>(value));
  }
  return true;
}

bool decodeMiniObfuscatedIcvm(const std::vector<std::uint8_t>& in,
                              std::vector<std::uint8_t>& out) {
  out.clear();
  if (in.size() < 4 || in[0] != 0x49) {
    return false;
  }

  static constexpr std::uint8_t key[3] = {0x88, 0xFA, 0x78};
  out = in;
  for (std::size_t i = 1; i < out.size(); ++i) {
    out[i] ^= key[(i - 1) % 3];
  }

  return out.size() >= 4 &&
         out[0] == 0x49 &&
         out[1] == 0x43 &&
         out[3] == 0x4D &&
         (out[2] == 0x56 || out[2] == 0x57);
}

} // namespace

int main(int argc, char** argv) {
  std::string path;
  std::string pcapPath;
  int pcapCycles = 1;
  int pcapGapScale = 1;
  int pcapMaxSteps = 12;
  int pcapStartOffset = 0;
  unsigned int baudOnly = 0;
  bool pcapShowRx = false;
  bool decodeIcvm = true;
  int sendReadMs = 250;
  std::vector<std::string> sendHexList;
  std::vector<std::uint32_t> sendIcvmProtocols;
  bool exclusive = true;
  bool ctrlMatrix = false;
  CtrlMode ctrlMode = CtrlMode::Pulse;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--pcap" && i + 1 < argc) {
      pcapPath = argv[++i];
      continue;
    }
    if (arg == "--pcap-cycles" && i + 1 < argc) {
      pcapCycles = std::max(1, std::atoi(argv[++i]));
      continue;
    }
    if (arg == "--pcap-gap-scale" && i + 1 < argc) {
      pcapGapScale = std::max(1, std::atoi(argv[++i]));
      continue;
    }
    if (arg == "--pcap-max-steps" && i + 1 < argc) {
      pcapMaxSteps = std::max(1, std::atoi(argv[++i]));
      continue;
    }
    if (arg == "--pcap-start-offset" && i + 1 < argc) {
      pcapStartOffset = std::max(0, std::atoi(argv[++i]));
      continue;
    }
    if (arg == "--baud" && i + 1 < argc) {
      baudOnly = static_cast<unsigned int>(std::strtoul(argv[++i], nullptr, 10));
      continue;
    }
    if (arg == "--pcap-show-rx") {
      pcapShowRx = true;
      continue;
    }
    if (arg == "--no-decode-icvm") {
      decodeIcvm = false;
      continue;
    }
    if (arg == "--send-hex" && i + 1 < argc) {
      sendHexList.push_back(argv[++i]);
      continue;
    }
    if (arg == "--send-icvm-protocol" && i + 1 < argc) {
      sendIcvmProtocols.push_back(static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0)));
      continue;
    }
    if (arg == "--send-read-ms" && i + 1 < argc) {
      sendReadMs = std::max(1, std::atoi(argv[++i]));
      continue;
    }
    if (arg == "--no-excl") {
      exclusive = false;
      continue;
    }
    if (arg == "--ctrl-matrix") {
      ctrlMatrix = true;
      continue;
    }
    if (arg == "--ctrl-mode" && i + 1 < argc) {
      ctrlMode = parseCtrlMode(argv[++i]);
      continue;
    }
    if (path.empty()) {
      path = arg;
    }
  }

  if (path.empty()) {
    path = findSerial();
  }
  if (path.empty()) { std::fprintf(stderr, "no /dev/cu.usbserial-* found\n"); return 1; }
  std::printf("probing port %s\n", path.c_str());

  // Candidate probe payloads — short, no harm before any J2534 Connect.
  const std::vector<Probe> probes = {
      {"listen",        {}},
      {"0x00",          {0x00}},
      {"0xFF",          {0xFF}},
      {"0x55 0xAA",     {0x55, 0xAA}},
      {"AA 55 (alt)",   {0xAA, 0x55}},
      {"ATZ\\r\\n",     {'A','T','Z','\r','\n'}},
      {"ATI\\r",        {'A','T','I','\r'}},
      {"AT@1\\r",       {'A','T','@','1','\r'}},
      {"AT WS\\r",      {'A','T',' ','W','S','\r'}},
      {"OPEN\\r",       {'O','P','E','N','\r'}},
      {"ver?\\r",       {'v','e','r','?','\r'}},
      // Generic J2534-style 4-byte length-prefixed PassThruReadVersion guess
      {"00 00 00 00",   {0x00,0x00,0x00,0x00}},
      {"01 00 00 00",   {0x01,0x00,0x00,0x00}},
      // Hartmann/XHorse Mini-VCI-style framed header guesses
      {"00 00 0C ?",    {0x00,0x00,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
      // Mongoose-style start-of-frame guesses
      {"61 (a)",        {0x61}},
      {"7E (b)",        {0x7E}},
  };

  std::vector<unsigned int> bauds;
  if (baudOnly != 0U) {
    bauds = {baudOnly};
  } else {
    bauds = {500000, 230400, 115200, 38400, 57600, 19200, 9600};
  }

  std::vector<ReplayStep> profile;
  if (!pcapPath.empty()) {
    auto trace = parseBulkTraceFromPcap(pcapPath);
    profile = buildReplayProfile(
      trace,
      static_cast<std::size_t>(pcapStartOffset),
      static_cast<std::size_t>(pcapMaxSteps));
    std::printf("pcap mode: %s\n", pcapPath.c_str());
    std::printf("pcap events: %zu, replay steps: %zu, cycles: %d, gap-scale: %d, start-offset: %d, max-steps: %d\n",
        trace.size(), profile.size(), pcapCycles, pcapGapScale, pcapStartOffset, pcapMaxSteps);
    if (profile.empty()) {
      std::printf("warning: failed to build replay profile from pcap, falling back to generic probes\n");
    }
  }

  int fd = openPort(path, exclusive);
  if (fd < 0) {
    std::fprintf(stderr, "open(%s) failed after retries: %s\n", path.c_str(), std::strerror(errno));
    return 1;
  }

  std::vector<CtrlMode> ctrlModes;
  if (ctrlMatrix) {
    ctrlModes = {CtrlMode::Pulse, CtrlMode::High, CtrlMode::None};
  } else {
    ctrlModes = {ctrlMode};
  }

  int totalRxSteps = 0;
  int totalMatched = 0;
  int totalSteps = 0;

  for (auto mode : ctrlModes) {
    std::printf("\n=== ctrl-mode %s ===\n", ctrlModeName(mode));
    for (auto baud : bauds) {
      if (!setBaud(fd, baud)) {
        std::printf("\n[baud=%u] setBaud failed\n", baud);
        continue;
      }
      std::printf("\n=== baud %u ===\n", baud);
      // small settle so the FTDI chip applies the rate
      std::this_thread::sleep_for(std::chrono::milliseconds(50));

      applyCtrlMode(fd, mode);
      auto banner = readFor(fd, 1500);
      if (!banner.empty()) {
        std::printf("  [line-mode banner] %zu bytes: ", banner.size());
        hex(banner);
        std::printf(" | \"");
        for (auto b : banner) std::printf("%c", (b >= 0x20 && b < 0x7F) ? char(b) : '.');
        std::printf("\"\n");
      }

      auto idle = readFor(fd, 100);
      if (!idle.empty()) { std::printf("  idle bytes (%zu): ", idle.size()); hex(idle); std::printf("\n"); }

      if (!profile.empty()) {
        std::printf("  [pcap replay]\n");
        int matched = 0;
        int seenAny = 0;
        ::tcflush(fd, TCIOFLUSH);

        for (int cycle = 0; cycle < pcapCycles; ++cycle) {
          std::printf("    cycle %d\n", cycle + 1);
          for (std::size_t i = 0; i < profile.size(); ++i) {
            const auto& step = profile[i];
            writeAll(fd, step.out);

            int readMs = 40;
            if (step.gapToNextOutMs > 0) {
              readMs = std::clamp(step.gapToNextOutMs * pcapGapScale + 8, 8, 350);
            }

            const auto resp = readFor(fd, readMs);
            if (!resp.empty()) {
              seenAny += 1;
            }
            const bool ok = replyMatches(resp, step.expectedIn);
            if (ok) {
              matched += 1;
            }

            std::printf("      step %02zu out[%zu]", i + 1, step.out.size());
            if (!step.expectedIn.empty()) {
              std::printf(" exp[%zu]", step.expectedIn.size());
            }
            std::printf(" wait=%dms -> ", readMs);
            if (resp.empty()) {
              std::printf("no-rx");
            } else {
              std::printf("rx[%zu] ", resp.size());
            }
            std::printf("%s\n", ok ? "MATCH" : "MISS");
            if (pcapShowRx && !resp.empty()) {
              std::printf("        rxhex: ");
              hex(resp);
              std::printf("\n");
              if (decodeIcvm) {
                std::vector<std::uint8_t> decoded;
                if (decodeMiniObfuscatedIcvm(resp, decoded)) {
                  std::printf("        rxdec: ");
                  hex(decoded);
                  std::printf("\n");
                }
              }
            }
          }
        }

        const auto baudTotalSteps = static_cast<int>(profile.size()) * pcapCycles;
        totalSteps += baudTotalSteps;
        totalMatched += matched;
        totalRxSteps += seenAny;
        std::printf("  [pcap replay summary] matched=%d/%d rx_steps=%d\n", matched, baudTotalSteps, seenAny);

        if (!sendHexList.empty()) {
          std::printf("  [custom send-hex probes]\n");
          for (std::size_t i = 0; i < sendHexList.size(); ++i) {
            std::vector<std::uint8_t> probe;
            if (!parseHexBytes(sendHexList[i], probe)) {
              std::printf("    custom %02zu parse error: %s\n", i + 1, sendHexList[i].c_str());
              continue;
            }
            if (i < sendIcvmProtocols.size() &&
                probe.size() >= 12 &&
                probe[0] == 0x49 &&
                probe[1] == 0x43 &&
                probe[2] == 0x56 &&
                probe[3] == 0x4D) {
              const auto protocol = sendIcvmProtocols[i];
              probe[8] = static_cast<std::uint8_t>(protocol & 0xffU);
              probe[9] = static_cast<std::uint8_t>((protocol >> 8) & 0xffU);
              probe[10] = static_cast<std::uint8_t>((protocol >> 16) & 0xffU);
              probe[11] = static_cast<std::uint8_t>((protocol >> 24) & 0xffU);
              std::printf("    custom %02zu protocol override -> %u\n", i + 1, protocol);
            }
            ::tcflush(fd, TCIOFLUSH);
            writeAll(fd, probe);
            const auto resp = readFor(fd, sendReadMs);
            std::printf("    custom %02zu tx[%zu] wait=%dms -> ", i + 1, probe.size(), sendReadMs);
            if (resp.empty()) {
              std::printf("no-rx\n");
            } else {
              std::printf("rx[%zu]: ", resp.size());
              hex(resp);
              std::printf("\n");
              if (decodeIcvm) {
                std::vector<std::uint8_t> decoded;
                if (decodeMiniObfuscatedIcvm(resp, decoded)) {
                  std::printf("      decoded: ");
                  hex(decoded);
                  std::printf("\n");
                }
              }
            }
          }
        }
        continue;
      }

      for (const auto& p : probes) {
        ::tcflush(fd, TCIOFLUSH);
        if (!p.bytes.empty()) writeAll(fd, p.bytes);
        auto resp = readFor(fd, 250);
        if (!resp.empty()) {
          std::printf("  [%s] -> %zu bytes: ", p.label, resp.size());
          hex(resp);
          // also print printable form
          std::printf(" | \"");
          for (auto b : resp) std::printf("%c", (b >= 0x20 && b < 0x7F) ? char(b) : '.');
          std::printf("\"\n");
        }
      }
    }
  }

  if (!profile.empty()) {
    std::printf("\n=== overall summary ===\n");
    std::printf("matched=%d/%d rx_steps=%d\n", totalMatched, totalSteps, totalRxSteps);
  }

  ::close(fd);
  return 0;
}
