#include "mvci/uds.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace mvci {
namespace {

bool isPositiveResponse(const std::vector<std::uint8_t>& response, std::uint8_t serviceId) {
  return !response.empty() && response.front() == static_cast<std::uint8_t>(serviceId + 0x40U);
}

std::uint32_t readDtcCode(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 16U) |
         (static_cast<std::uint32_t>(data[1]) << 8U) |
         static_cast<std::uint32_t>(data[2]);
}

Status writeSingleFrame(ChannelHandle channelId, const std::vector<std::uint8_t>& request) {
  PassThruMsg msg{};
  msg.protocolId = PROTOCOL_ISO15765;
  msg.dataSize = static_cast<std::uint32_t>(std::min<std::size_t>(request.size(), sizeof(msg.data)));
  std::copy_n(request.begin(), msg.dataSize, msg.data);

  std::uint32_t count = 1;
  return PassThruWriteMsgs(channelId, &msg, &count, 1000);
}

} // namespace

std::vector<std::uint8_t> buildReadDtcRequest(std::uint8_t statusMask) {
  return {0x19U, 0x02U, statusMask};
}

std::vector<std::uint8_t> buildClearDtcRequest() {
  return {0x14U, 0xFFU, 0xFFU, 0xFFU};
}

Status sendUdsRequest(ChannelHandle channelId,
                      const std::vector<std::uint8_t>& request,
                      std::vector<std::vector<std::uint8_t>>& responses,
                      std::uint32_t timeoutMs) {
  responses.clear();

  if (request.empty()) {
    return ERR_INVALID_MSG;
  }

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

    responses.emplace_back(msg.data, msg.data + msg.dataSize);
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