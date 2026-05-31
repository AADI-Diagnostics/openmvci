#include "mvci/uds.hpp"
#include "mvci/api.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace mvci {
namespace {

constexpr std::uint32_t kObdBroadcastCanId = 0x000007DFU;
constexpr std::size_t kCanIdPrefixLen = 4U;

std::vector<std::uint8_t> withCanIdPrefix(const std::vector<std::uint8_t>& payload,
                                          std::uint32_t canId = kObdBroadcastCanId) {
  std::vector<std::uint8_t> wrapped;
  wrapped.reserve(kCanIdPrefixLen + payload.size());
  wrapped.push_back(static_cast<std::uint8_t>((canId >> 24U) & 0xFFU));
  wrapped.push_back(static_cast<std::uint8_t>((canId >> 16U) & 0xFFU));
  wrapped.push_back(static_cast<std::uint8_t>((canId >> 8U) & 0xFFU));
  wrapped.push_back(static_cast<std::uint8_t>(canId & 0xFFU));
  wrapped.insert(wrapped.end(), payload.begin(), payload.end());
  return wrapped;
}

std::vector<std::uint8_t> stripCanIdPrefix(const std::vector<std::uint8_t>& frame) {
  if (frame.size() <= kCanIdPrefixLen) {
    return frame;
  }
  return std::vector<std::uint8_t>(frame.begin() + kCanIdPrefixLen, frame.end());
}

bool isPositiveResponse(const std::vector<std::uint8_t>& response, std::uint8_t serviceId) {
  return !response.empty() && response.front() == static_cast<std::uint8_t>(serviceId + 0x40U);
}

std::uint32_t readDtcCode(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 16U) |
         (static_cast<std::uint32_t>(data[1]) << 8U) |
         static_cast<std::uint32_t>(data[2]);
}

Status writeSingleFrame(ChannelHandle channelId, const std::vector<std::uint8_t>& request) {
  const auto framed = withCanIdPrefix(request);
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

Status sendUdsRequest(ChannelHandle channelId,
                      const std::vector<std::uint8_t>& request,
                      std::vector<std::vector<std::uint8_t>>& responses,
                      std::uint32_t timeoutMs) {
  responses.clear();

  if (request.empty()) {
    return ERR_INVALID_MSG;
  }

  ensureObdFlowControlFilters(channelId);

  const auto writeStatus = writeSingleFrame(channelId, request);
  if (writeStatus != STATUS_NOERROR) {
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
    responses.emplace_back(stripCanIdPrefix(payload));
    if (remaining == 0U) {
      break;
    }
  }

  return responses.empty() ? ERR_TIMEOUT : STATUS_NOERROR;
}

Status parseActiveDtcResponses(const std::vector<std::vector<std::uint8_t>>& responses,
                               std::vector<DtcRecord>& dtcs,
                               std::uint8_t statusMask) {
  dtcs.clear();

  for (const auto& response : responses) {
    if (response.size() < 4) {
      continue;
    }

    if (response[0] == 0x7FU) {
      return ERR_FAILED;
    }

    if (!isPositiveResponse(response, 0x19U)) {
      continue;
    }

    for (std::size_t index = 3; index + 3 < response.size(); index += 4) {
      const auto code = readDtcCode(&response[index]);
      const auto recordStatus = response[index + 3];
      if ((recordStatus & statusMask) == 0U) {
        continue;
      }
      dtcs.push_back(DtcRecord{code, recordStatus});
    }
  }

  return STATUS_NOERROR;
}

Status readActiveDtcs(ChannelHandle channelId,
                      std::vector<DtcRecord>& dtcs,
                      std::uint32_t timeoutMs,
                      std::uint8_t statusMask) {
  const auto request = buildReadDtcRequest(statusMask);
  std::vector<std::vector<std::uint8_t>> responses;
  const auto status = sendUdsRequest(channelId, request, responses, timeoutMs);
  if (status != STATUS_NOERROR) {
    return status;
  }

  return parseActiveDtcResponses(responses, dtcs, statusMask);
}

Status parseVinResponses(const std::vector<std::vector<std::uint8_t>>& responses,
                         std::string& vin) {
  vin.clear();

  for (const auto& response : responses) {
    if (response.size() < 4) {
      continue;
    }

    if (response[0] == 0x7FU) {
      return ERR_FAILED;
    }

    if (response[0] != 0x62U || response[1] != 0xF1U || response[2] != 0x90U) {
      continue;
    }

    for (std::size_t i = 3; i < response.size(); ++i) {
      const auto c = static_cast<char>(response[i]);
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
    if (response.size() < 4) {
      continue;
    }

    if (response[0] == 0x7FU) {
      return ERR_FAILED;
    }

    if (response[0] != 0x49U || response[1] != 0x02U) {
      continue;
    }

    const std::uint8_t frameIndex = response[2];
    std::string frameData;
    for (std::size_t i = 3; i < response.size(); ++i) {
      const auto c = static_cast<char>(response[i]);
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
  const auto udsRequest = buildReadVinRequest();
  std::vector<std::vector<std::uint8_t>> responses;
  auto status = sendUdsRequest(channelId, udsRequest, responses, timeoutMs);
  if (status == STATUS_NOERROR) {
    status = parseVinResponses(responses, vin);
    if (status == STATUS_NOERROR) {
      return STATUS_NOERROR;
    }
  }

  const auto obdRequest = buildReadVinOBDRequest();
  responses.clear();
  status = sendUdsRequest(channelId, obdRequest, responses, timeoutMs);
  if (status != STATUS_NOERROR) {
    return status;
  }

  return parseOBDVinResponses(responses, vin);
}

Status clearDtcs(ChannelHandle channelId, std::uint32_t timeoutMs) {
  const auto request = buildClearDtcRequest();
  std::vector<std::vector<std::uint8_t>> responses;
  const auto status = sendUdsRequest(channelId, request, responses, timeoutMs);
  if (status != STATUS_NOERROR) {
    return status;
  }

  for (const auto& response : responses) {
    if (response.empty()) {
      continue;
    }
    if (response[0] == 0x7FU) {
      return ERR_FAILED;
    }
    if (response[0] == 0x54U) {
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