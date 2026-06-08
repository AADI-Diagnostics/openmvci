#include "mvci/uds.hpp"
#include "mvci/api.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace mvci {
namespace {

// Always-on logging for the UDS/OBD request path (VIN, DTCs). Visible on
// plain `./read_dtcs` runs so users (and developers) can see the high-level
// requests and response counts without MVCI_VERBOSE_*.
template <typename... Args>
void udsInfo(const char* fmt, Args... args) {
  std::fprintf(stderr, "[mvci uds] ");
  std::fprintf(stderr, fmt, args...);
  std::fputc('\n', stderr);
}

void udsInfo(const char* msg) {
  std::fprintf(stderr, "[mvci uds] %s\n", msg);
}

constexpr std::uint32_t kObdBroadcastCanId = 0x000007DFU;
constexpr std::size_t kCanIdPrefixLen = 4U;

std::uint32_t resolveRequestCanId() {
  if (const char* env = std::getenv("MVCI_OBD_CAN_ID")) {
    try {
      return static_cast<std::uint32_t>(std::stoul(env, nullptr, 0));
    } catch (...) {
      // fall through to default
    }
  }
  return kObdBroadcastCanId;
}

std::vector<std::uint8_t> withCanIdPrefix(const std::vector<std::uint8_t>& payload,
                                          std::uint32_t canId = resolveRequestCanId()) {
  std::vector<std::uint8_t> wrapped;
  wrapped.reserve(kCanIdPrefixLen + payload.size());
  wrapped.push_back(static_cast<std::uint8_t>((canId >> 24U) & 0xFFU));
  wrapped.push_back(static_cast<std::uint8_t>((canId >> 16U) & 0xFFU));
  wrapped.push_back(static_cast<std::uint8_t>((canId >> 8U) & 0xFFU));
  wrapped.push_back(static_cast<std::uint8_t>(canId & 0xFFU));
  wrapped.insert(wrapped.end(), payload.begin(), payload.end());
  return wrapped;
}

std::uint32_t extractCanId(const std::vector<std::uint8_t>& frame) {
  if (frame.size() < kCanIdPrefixLen) {
    return 0U;
  }
  // Only decode as a CAN ID prefix when the frame looks like our OBD embedding
  // (11-bit IDs carried as 00 00 07 Ex in big-endian 32-bit word). Bare UDS
  // payloads (e.g. starting with 0x59/0x49/0x7F from unit tests or direct callers)
  // must be left alone; their first byte is never 0 in this protocol.
  if (frame[0] != 0U || frame[1] != 0U) {
    return 0U;
  }
  return (static_cast<std::uint32_t>(frame[0]) << 24U) |
         (static_cast<std::uint32_t>(frame[1]) << 16U) |
         (static_cast<std::uint32_t>(frame[2]) << 8U) |
         static_cast<std::uint32_t>(frame[3]);
}

bool isPositiveResponse(const std::vector<std::uint8_t>& response, std::uint8_t serviceId) {
  return !response.empty() && response.front() == static_cast<std::uint8_t>(serviceId + 0x40U);
}

std::uint32_t readDtcCode(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 16U) |
         (static_cast<std::uint32_t>(data[1]) << 8U) |
         static_cast<std::uint32_t>(data[2]);
}

Status writeSingleFrame(ChannelHandle channelId, const std::vector<std::uint8_t>& request, std::uint32_t canIdOverride) {
  const auto canId = (canIdOverride != 0U) ? canIdOverride : resolveRequestCanId();
  const auto framed = withCanIdPrefix(request, canId);
  PassThruMsg msg{};
  msg.protocolId = PROTOCOL_ISO15765;
  msg.txFlags = ISO15765_FRAME_PAD;
  msg.dataSize = static_cast<std::uint32_t>(std::min<std::size_t>(framed.size(), sizeof(msg.data)));
  std::copy_n(framed.begin(), msg.dataSize, msg.data);

  std::uint32_t count = 1;
  return PassThruWriteMsgs(channelId, &msg, &count, 1000);
}

void buildCanIdMsg(PassThruMsg& msg, std::uint32_t canId) {
  msg.protocolId = PROTOCOL_ISO15765;
  msg.txFlags = ISO15765_FRAME_PAD;
  msg.dataSize = 4U;
  msg.data[0] = static_cast<std::uint8_t>((canId >> 24U) & 0xFFU);
  msg.data[1] = static_cast<std::uint8_t>((canId >> 16U) & 0xFFU);
  msg.data[2] = static_cast<std::uint8_t>((canId >> 8U) & 0xFFU);
  msg.data[3] = static_cast<std::uint8_t>(canId & 0xFFU);
}

// Install ISO-15765 flow-control filter pairs for the standard 11-bit OBD-II
// ECU range (tx 0x7E0..0x7E7 -> rx 0x7E8..0x7EF) plus the functional broadcast
// (tx 0x7DF -> rx 0x7E8) on first use of a channel. Without these filters the
// adapter has no rules to accept incoming response frames, so reads time out
// even when the ECU is replying on the bus.
void ensureObdFlowControlFilters(ChannelHandle channelId) {
  static std::set<ChannelHandle> initialised;
  if (initialised.count(channelId) != 0U) {
    return;
  }
  initialised.insert(channelId);

  const std::uint32_t mask = 0xFFFFFFFFU;
  for (std::uint32_t ecu = 0U; ecu < 8U; ++ecu) {
    const std::uint32_t txId = 0x7E0U + ecu;
    const std::uint32_t rxId = 0x7E8U + ecu;

    PassThruMsg maskMsg{};
    buildCanIdMsg(maskMsg, mask);
    PassThruMsg patternMsg{};
    buildCanIdMsg(patternMsg, rxId);
    PassThruMsg fcMsg{};
    buildCanIdMsg(fcMsg, txId);

    std::uint32_t filterId = 0;
    PassThruStartMsgFilter(channelId, FILTER_FLOW_CONTROL, &maskMsg, &patternMsg, &fcMsg, &filterId);
  }
}

} // namespace

// Public implementation (definition must be in mvci:: not the anonymous namespace
// so that it satisfies the declaration in the header and is exported from the library).
std::vector<std::uint8_t> stripCanIdPrefix(const std::vector<std::uint8_t>& frame) {
  constexpr std::size_t prefixLen = 4U;
  if (frame.size() <= prefixLen) {
    return frame;
  }
  // Only strip when the frame looks like it carries our CAN ID prefix (leading
  // 00 00 for the OBD 11-bit IDs). Otherwise the payload is already a bare UDS
  // response (as in unit tests and for any callers that invoke the parsers directly).
  if (frame[0] != 0U || frame[1] != 0U) {
    return frame;
  }
  return std::vector<std::uint8_t>(frame.begin() + prefixLen, frame.end());
}

std::vector<std::uint8_t> buildReadDtcRequest(std::uint8_t statusMask) {
  return {0x19U, 0x02U, statusMask};
}

std::vector<std::uint8_t> buildClearDtcRequest() {
  return {0x14U, 0xFFU, 0xFFU, 0xFFU};
}

std::vector<std::uint8_t> buildReadVinRequest() {
  return {0x22U, 0xF1U, 0x90U};
}

std::vector<std::uint8_t> buildReadVinOBDRequest() {
  return {0x09U, 0x02U};
}

// Forward declaration so the session helper (placed early for readability) can
// call the internal WithId sender before its definition appears in this TU.
static Status sendUdsRequestWithId(ChannelHandle channelId,
                                   const std::vector<std::uint8_t>& request,
                                   std::vector<std::vector<std::uint8_t>>& responses,
                                   std::uint32_t timeoutMs,
                                   std::uint32_t canIdOverride);

// Best-effort attempt to enter a default diagnostic session (e.g. extended 0x03).
// Many ECUs and gateways require an active session before UDS DID reads (22 F190)
// or DTC retrieval (19 02) are permitted; others will respond to data requests
// without it. We fire the request with a short timeout and ignore the result so
// that higher-level VIN/DTC code can proceed unconditionally.
static Status tryEnterDefaultDiagnosticSession(ChannelHandle channelId,
                                               std::uint32_t shortTimeoutMs) {
  const std::vector<std::uint8_t> req{0x10U, 0x03U};
  std::vector<std::vector<std::uint8_t>> replies;
  (void)sendUdsRequestWithId(channelId, req, replies, shortTimeoutMs, /*canIdOverride*/ 0U);
  // Swallow any status; a 50 03 positive, 7F 10 xx negative, or even a pure timeout
  // are all acceptable here — we just want to have "woken" the session state if needed.
  return STATUS_NOERROR;
}

// Internal implementation supporting a one-shot CAN ID override for this request
// (enables automatic fallback from 7DF functional to 7E0 physical for ECUs/gateways
// that ignore broadcast, without affecting other callers or requiring env changes).
static Status sendUdsRequestWithId(ChannelHandle channelId,
                                   const std::vector<std::uint8_t>& request,
                                   std::vector<std::vector<std::uint8_t>>& responses,
                                   std::uint32_t timeoutMs,
                                   std::uint32_t canIdOverride /* 0 = resolveRequestCanId() */) {
  responses.clear();

  if (request.empty()) {
    return ERR_INVALID_MSG;
  }

  ensureObdFlowControlFilters(channelId);

  const auto canId = (canIdOverride != 0U) ? canIdOverride : resolveRequestCanId();
  {
    std::string reqHex;
    for (auto b : request) {
      char buf[4];
      std::snprintf(buf, sizeof(buf), "%02X ", b);
      reqHex += buf;
    }
    udsInfo("send ch=%u canId=0x%08X req=[%s] timeout=%ums", channelId, canId, reqHex.c_str(), timeoutMs);
  }

  const auto writeStatus = writeSingleFrame(channelId, request, canIdOverride);
  if (writeStatus != STATUS_NOERROR) {
    udsInfo("writeSingleFrame failed: %d", static_cast<int>(writeStatus));
    return writeStatus;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (true) {
    PassThruMsg msg{};
    std::uint32_t count = 1;

    const auto now = std::chrono::steady_clock::now();
    const auto remaining = now >= deadline ? 0U : static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    const auto readStatus = PassThruReadMsgs(channelId, &msg, &count, remaining);
    if (readStatus == ERR_TIMEOUT) {
      break;
    }
    if (readStatus != STATUS_NOERROR) {
      return readStatus;
    }

    std::vector<std::uint8_t> payload(msg.data, msg.data + msg.dataSize);
    // Keep the raw payload (includes 4-byte CAN ID prefix for RX frames). The prefix
    // carries the ECU source address (e.g. 0x7E8) so callers and parsers can associate
    // responses/DTCs with specific ECUs. Parsers below use stripCanIdPrefix as needed.
    responses.emplace_back(std::move(payload));
    if (remaining == 0U) {
      break;
    }
  }

  udsInfo("received %zu response frame(s); status=%s", responses.size(),
          responses.empty() ? "ERR_TIMEOUT" : "OK");
  return responses.empty() ? ERR_TIMEOUT : STATUS_NOERROR;
}

Status sendUdsRequest(ChannelHandle channelId,
                      const std::vector<std::uint8_t>& request,
                      std::vector<std::vector<std::uint8_t>>& responses,
                      std::uint32_t timeoutMs) {
  return sendUdsRequestWithId(channelId, request, responses, timeoutMs, /*override*/ 0U);
}

Status parseActiveDtcResponses(const std::vector<std::vector<std::uint8_t>>& responses,
                               std::vector<DtcRecord>& dtcs,
                               std::uint8_t statusMask) {
  dtcs.clear();

  for (const auto& response : responses) {
    const auto uds = stripCanIdPrefix(response);
    const std::uint32_t ecu = extractCanId(response);

    if (uds.size() < 4) {
      continue;
    }

    if (uds[0] == 0x7FU) {
      return ERR_FAILED;
    }

    if (!isPositiveResponse(uds, 0x19U)) {
      continue;
    }

    for (std::size_t index = 3; index + 3 < uds.size(); index += 4) {
      const auto code = readDtcCode(&uds[index]);
      const auto recordStatus = uds[index + 3];
      if ((recordStatus & statusMask) == 0U) {
        continue;
      }
      dtcs.push_back(DtcRecord{code, recordStatus, ecu});
    }
  }

  return STATUS_NOERROR;
}

Status readActiveDtcs(ChannelHandle channelId,
                      std::vector<DtcRecord>& dtcs,
                      std::uint32_t timeoutMs,
                      std::uint8_t statusMask) {
  (void)tryEnterDefaultDiagnosticSession(channelId, std::min<std::uint32_t>(timeoutMs, 1200U));

  const auto request = buildReadDtcRequest(statusMask);
  const auto primaryId = resolveRequestCanId();

  std::vector<std::vector<std::uint8_t>> responses;
  auto status = sendUdsRequestWithId(channelId, request, responses, timeoutMs, primaryId);
  if (status == STATUS_NOERROR) {
    status = parseActiveDtcResponses(responses, dtcs, statusMask);
    if (status == STATUS_NOERROR || !responses.empty()) {
      return status;
    }
  }

  // Broadcast produced no frames at all — retry with physical addressing (GM ECMs etc.).
  if (responses.empty() && primaryId == kObdBroadcastCanId) {
    udsInfo("DTC read no response on broadcast; retrying with physical 0x7E0");
    responses.clear();
    status = sendUdsRequestWithId(channelId, request, responses, timeoutMs, 0x000007E0U);
    if (status == STATUS_NOERROR) {
      status = parseActiveDtcResponses(responses, dtcs, statusMask);
      if (status == STATUS_NOERROR || !responses.empty()) {
        return status;
      }
    }
  }

  return status;
}

Status parseVinResponses(const std::vector<std::vector<std::uint8_t>>& responses,
                         std::string& vin) {
  vin.clear();

  for (const auto& response : responses) {
    const auto uds = stripCanIdPrefix(response);
    if (uds.size() < 4) {
      continue;
    }

    if (uds[0] == 0x7FU) {
      return ERR_FAILED;
    }

    if (uds[0] != 0x62U || uds[1] != 0xF1U || uds[2] != 0x90U) {
      continue;
    }

    for (std::size_t i = 3; i < uds.size(); ++i) {
      const auto c = static_cast<char>(uds[i]);
      if (std::isprint(static_cast<unsigned char>(c)) != 0 && c != '\0') {
        vin.push_back(c);
      }
    }
    break;
  }

  return vin.empty() ? ERR_FAILED : STATUS_NOERROR;
}

Status parseOBDVinResponses(const std::vector<std::vector<std::uint8_t>>& responses,
                            std::string& vin) {
  vin.clear();
  std::map<std::uint8_t, std::string> orderedFrames;

  for (const auto& response : responses) {
    const auto uds = stripCanIdPrefix(response);
    if (uds.size() < 4) {
      continue;
    }

    if (uds[0] == 0x7FU) {
      return ERR_FAILED;
    }

    if (uds[0] != 0x49U || uds[1] != 0x02U) {
      continue;
    }

    const std::uint8_t frameIndex = uds[2];
    std::string frameData;
    for (std::size_t i = 3; i < uds.size(); ++i) {
      const auto c = static_cast<char>(uds[i]);
      if (std::isprint(static_cast<unsigned char>(c)) != 0 && c != '\0') {
        frameData.push_back(c);
      }
    }

    if (!frameData.empty()) {
      orderedFrames[frameIndex] = std::move(frameData);
    }
  }

  for (const auto& [_, part] : orderedFrames) {
    vin += part;
  }

  if (vin.size() > 17) {
    vin.resize(17);
  }
  return vin.empty() ? ERR_FAILED : STATUS_NOERROR;
}

Status readVehicleVin(ChannelHandle channelId,
                      std::string& vin,
                      std::uint32_t timeoutMs) {
  (void)tryEnterDefaultDiagnosticSession(channelId, std::min<std::uint32_t>(timeoutMs, 1200U));

  const auto primaryId = resolveRequestCanId();

  // Try UDS VIN DID (22 F190) first with the configured (or default broadcast) ID.
  {
    const auto udsRequest = buildReadVinRequest();
    std::vector<std::vector<std::uint8_t>> responses;
    auto status = sendUdsRequestWithId(channelId, udsRequest, responses, timeoutMs, primaryId);
    if (status == STATUS_NOERROR) {
      status = parseVinResponses(responses, vin);
      if (status == STATUS_NOERROR) {
        return STATUS_NOERROR;
      }
    }

    // If the vehicle/gateway ignored the functional broadcast (common on GM),
    // retry the exact same UDS request using physical addressing to the ECM (7E0 -> 7E8).
    if (responses.empty() && primaryId == kObdBroadcastCanId) {
      udsInfo("VIN UDS no response on broadcast; retrying with physical 0x7E0");
      responses.clear();
      status = sendUdsRequestWithId(channelId, udsRequest, responses, timeoutMs, 0x000007E0U);
      if (status == STATUS_NOERROR) {
        status = parseVinResponses(responses, vin);
        if (status == STATUS_NOERROR) {
          return STATUS_NOERROR;
        }
      }
    }
  }

  // OBD mode 09 02 fallback (also try physical on broadcast miss).
  {
    const auto obdRequest = buildReadVinOBDRequest();
    std::vector<std::vector<std::uint8_t>> responses;
    auto status = sendUdsRequestWithId(channelId, obdRequest, responses, timeoutMs, primaryId);
    if (status == STATUS_NOERROR) {
      status = parseOBDVinResponses(responses, vin);
      if (status == STATUS_NOERROR) {
        return STATUS_NOERROR;
      }
    }

    if (responses.empty() && primaryId == kObdBroadcastCanId) {
      udsInfo("VIN OBD no response on broadcast; retrying with physical 0x7E0");
      responses.clear();
      status = sendUdsRequestWithId(channelId, obdRequest, responses, timeoutMs, 0x000007E0U);
      if (status == STATUS_NOERROR) {
        status = parseOBDVinResponses(responses, vin);
        if (status == STATUS_NOERROR) {
          return STATUS_NOERROR;
        }
      }
    }
    return status;
  }
}

Status clearDtcs(ChannelHandle channelId, std::uint32_t timeoutMs) {
  (void)tryEnterDefaultDiagnosticSession(channelId, std::min<std::uint32_t>(timeoutMs, 1200U));

  const auto request = buildClearDtcRequest();
  std::vector<std::vector<std::uint8_t>> responses;
  const auto status = sendUdsRequest(channelId, request, responses, timeoutMs);
  if (status != STATUS_NOERROR) {
    return status;
  }

  for (const auto& response : responses) {
    const auto uds = stripCanIdPrefix(response);
    if (uds.empty()) {
      continue;
    }
    if (uds[0] == 0x7FU) {
      return ERR_FAILED;
    }
    if (uds[0] == 0x54U) {
      return STATUS_NOERROR;
    }
  }

  return ERR_FAILED;
}

std::string formatDtc(std::uint32_t code) {
  std::ostringstream stream;
  stream << "0x" << std::uppercase << std::hex << std::setw(6) << std::setfill('0') << code;
  return stream.str();
}

std::string statusToString(Status status) {
  switch (status) {
  case STATUS_NOERROR:
    return "OK";
  case ERR_FAILED:
    return "failed";
  case ERR_NOT_INITIALIZED:
    return "not initialized";
  case ERR_INVALID_DEVICE_ID:
    return "invalid device";
  case ERR_INVALID_CHANNEL_ID:
    return "invalid channel";
  case ERR_INVALID_MSG:
    return "invalid message";
  case ERR_TIMEOUT:
    return "timeout";
  case ERR_NOT_SUPPORTED:
    return "not supported";
  default:
    return "unknown";
  }
}

std::string formatFrame(const std::vector<std::uint8_t>& frame) {
  std::ostringstream stream;
  stream << std::uppercase << std::hex;
  for (std::size_t index = 0; index < frame.size(); ++index) {
    if (index != 0) {
      stream << ' ';
    }
    stream << std::setw(2) << std::setfill('0') << static_cast<unsigned>(frame[index]);
  }
  return stream.str();
}

} // namespace mvci