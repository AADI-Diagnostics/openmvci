#include "mvci/driver.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "mvci/packet.hpp"
#include "mvci/platform/transport.hpp"

namespace mvci {
namespace {

void writeVersionString(char* destination, const char* value) {
  if (!destination) {
    return;
  }

  constexpr std::size_t kMaxChars = 79;
  std::strncpy(destination, value, kMaxChars);
  destination[kMaxChars] = '\0';
}

} // namespace

Driver::Driver() = default;

Driver::~Driver() {
  close();
}

Status Driver::open(const std::string& deviceName) {
  if (open_) {
    return STATUS_NOERROR;
  }

  transport_ = createPlatformTransport();
  if (!transport_) {
    return ERR_FAILED;
  }

  const auto status = transport_->open(deviceName);
  if (status != STATUS_NOERROR) {
    transport_.reset();
    return status;
  }

  open_ = true;
  return STATUS_NOERROR;
}

void Driver::close() {
  channels_.clear();
  periodicMessages_.clear();
  filters_.clear();
  if (transport_) {
    transport_->close();
    transport_.reset();
  }
  open_ = false;
}

Status Driver::connect(std::uint32_t protocolId,
                       std::uint32_t flags,
                       std::uint32_t baudRate,
                       ChannelHandle& channelId) {
  if (!open_ || !transport_) {
    return ERR_NOT_INITIALIZED;
  }

  channelId = nextChannel_++;
  channels_[channelId] = ChannelState{protocolId, flags, baudRate};
  periodicMessages_[channelId] = {};
  filters_[channelId] = {};
  return STATUS_NOERROR;
}

Status Driver::disconnect(ChannelHandle channelId) {
  const auto it = channels_.find(channelId);
  if (it == channels_.end()) {
    return ERR_INVALID_CHANNEL_ID;
  }

  channels_.erase(it);
  periodicMessages_.erase(channelId);
  filters_.erase(channelId);
  return STATUS_NOERROR;
}

Status Driver::write(ChannelHandle channelId,
                     const PassThruMsg* msgs,
                     std::uint32_t& numMsgs,
                     std::uint32_t timeoutMs) {
  if (!open_ || !transport_) {
    return ERR_NOT_INITIALIZED;
  }

  if (channels_.find(channelId) == channels_.end()) {
    return ERR_INVALID_CHANNEL_ID;
  }

  for (std::uint32_t index = 0; index < numMsgs; ++index) {
    const auto& msg = msgs[index];
    if (msg.dataSize > sizeof(msg.data)) {
      numMsgs = index;
      return ERR_INVALID_MSG;
    }

    const auto packet = encodePacket(msg, channelId);
    const auto status = transport_->write(packet);
    if (status != STATUS_NOERROR) {
      numMsgs = index;
      return status;
    }
  }

  (void)timeoutMs;
  return STATUS_NOERROR;
}

Status Driver::read(ChannelHandle channelId,
                    PassThruMsg* msgs,
                    std::uint32_t& numMsgs,
                    std::uint32_t timeoutMs) {
  if (!open_ || !transport_) {
    return ERR_NOT_INITIALIZED;
  }

  if (channels_.find(channelId) == channels_.end()) {
    return ERR_INVALID_CHANNEL_ID;
  }

  std::uint32_t delivered = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

  while (delivered < numMsgs) {
    std::vector<std::uint8_t> packet;
    const auto now = std::chrono::steady_clock::now();
    const auto remaining = now >= deadline ? 0U : static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

    const auto status = transport_->read(packet, remaining);
    if (status == ERR_TIMEOUT) {
      break;
    }
    if (status != STATUS_NOERROR) {
      numMsgs = delivered;
      return status;
    }

    PacketFrame frame;
    const auto decodeStatus = decodePacket(packet, frame);
    if (decodeStatus != STATUS_NOERROR) {
      numMsgs = delivered;
      return decodeStatus;
    }

    if (frame.channel != channelId) {
      continue;
    }

    auto& out = msgs[delivered];
    out.protocolId = frame.protocolId;
    out.rxStatus = 0;
    out.txFlags = frame.flags;
    out.timestamp = frame.timestamp;
    out.dataSize = static_cast<std::uint32_t>(std::min<std::size_t>(frame.payload.size(), sizeof(out.data)));
    out.extraDataIndex = out.dataSize;
    std::copy_n(frame.payload.begin(), out.dataSize, out.data);
    ++delivered;
  }

  numMsgs = delivered;
  return delivered > 0 ? STATUS_NOERROR : ERR_TIMEOUT;
}

Status Driver::startPeriodic(ChannelHandle channelId,
                             const PassThruMsg* msg,
                             std::uint32_t& msgId,
                             std::uint32_t timeIntervalMs) {
  if (!open_ || !transport_) {
    return ERR_NOT_INITIALIZED;
  }

  if (!msg || timeIntervalMs == 0U || timeIntervalMs > 60000U) {
    return ERR_INVALID_MSG;
  }

  if (channels_.find(channelId) == channels_.end()) {
    return ERR_INVALID_CHANNEL_ID;
  }

  msgId = nextPeriodicId_++;
  periodicMessages_[channelId][msgId] = *msg;
  return STATUS_NOERROR;
}

Status Driver::stopPeriodic(ChannelHandle channelId, std::uint32_t msgId) {
  if (!open_ || !transport_) {
    return ERR_NOT_INITIALIZED;
  }

  const auto channelIt = periodicMessages_.find(channelId);
  if (channelIt == periodicMessages_.end()) {
    return ERR_INVALID_CHANNEL_ID;
  }

  const auto erased = channelIt->second.erase(msgId);
  return erased > 0 ? STATUS_NOERROR : ERR_INVALID_MSG;
}

Status Driver::startFilter(ChannelHandle channelId,
                           std::uint32_t filterType,
                           const PassThruMsg* maskMsg,
                           const PassThruMsg* patternMsg,
                           const PassThruMsg* flowControlMsg,
                           std::uint32_t& filterId) {
  if (!open_ || !transport_) {
    return ERR_NOT_INITIALIZED;
  }

  if (channels_.find(channelId) == channels_.end()) {
    return ERR_INVALID_CHANNEL_ID;
  }

  if (!maskMsg || !patternMsg) {
    return ERR_INVALID_MSG;
  }

  if (filterType == FILTER_FLOW_CONTROL && !flowControlMsg) {
    return ERR_INVALID_MSG;
  }

  if (filterType != FILTER_PASS && filterType != FILTER_BLOCK && filterType != FILTER_FLOW_CONTROL) {
    return ERR_NOT_SUPPORTED;
  }

  filterId = nextFilterId_++;
  filters_[channelId][filterId] = filterType;
  return STATUS_NOERROR;
}

Status Driver::stopFilter(ChannelHandle channelId, std::uint32_t filterId) {
  if (!open_ || !transport_) {
    return ERR_NOT_INITIALIZED;
  }

  const auto channelIt = filters_.find(channelId);
  if (channelIt == filters_.end()) {
    return ERR_INVALID_CHANNEL_ID;
  }

  const auto erased = channelIt->second.erase(filterId);
  return erased > 0 ? STATUS_NOERROR : ERR_INVALID_MSG;
}

Status Driver::setProgrammingVoltage(std::uint32_t pin, std::uint32_t voltage) {
  if (!open_ || !transport_) {
    return ERR_NOT_INITIALIZED;
  }

  (void)pin;
  return voltage == 0U ? STATUS_NOERROR : ERR_NOT_SUPPORTED;
}

Status Driver::readVersion(char* firmwareVersion, char* dllVersion, char* apiVersion) {
  if (!open_ || !transport_) {
    return ERR_NOT_INITIALIZED;
  }

  writeVersionString(firmwareVersion, "openmvci-fw-0.1");
  writeVersionString(dllVersion, "openmvci-dll-0.1");
  writeVersionString(apiVersion, "J2534-04.04");
  return STATUS_NOERROR;
}

Status Driver::ioctl(ChannelHandle channelId,
                     std::uint32_t ioctlId,
                     void* input,
                     void* output) {
  if (!open_ || !transport_) {
    return ERR_NOT_INITIALIZED;
  }

  if (channels_.find(channelId) == channels_.end()) {
    return ERR_INVALID_CHANNEL_ID;
  }

  switch (ioctlId) {
  case IOCTL_CLEAR_RX_BUFFER:
    transport_->clearRx();
    return STATUS_NOERROR;
  case IOCTL_CLEAR_TX_BUFFER:
    transport_->clearTx();
    return STATUS_NOERROR;
  case IOCTL_READ_BATT_VOLTAGE:
    if (!output) {
      return ERR_INVALID_MSG;
    }
    *static_cast<std::uint32_t*>(output) = 12200;
    return STATUS_NOERROR;
  case IOCTL_SET_CONFIG:
    if (!input) {
      return ERR_INVALID_MSG;
    }
    {
      auto& channel = channels_.at(channelId);
      const auto list = static_cast<ConfigList*>(input);
      if (list->configPtr && list->numOfParams > 0U && list->numOfParams <= 64U) {
        for (std::uint32_t index = 0; index < list->numOfParams; ++index) {
          const auto& config = list->configPtr[index];
          if (config.parameter == CONFIG_BAUDRATE) {
            channel.baudRate = config.value;
            return STATUS_NOERROR;
          }
        }
        return ERR_NOT_SUPPORTED;
      }

      const auto config = static_cast<ConfigValue*>(input);
      if (config->parameter == CONFIG_BAUDRATE) {
        channel.baudRate = config->value;
        return STATUS_NOERROR;
      }
    }
    return ERR_NOT_SUPPORTED;
  case IOCTL_GET_CONFIG:
    if (!output) {
      return ERR_INVALID_MSG;
    }
    {
      const auto& channel = channels_.at(channelId);
      const auto list = static_cast<ConfigList*>(output);
      if (list->configPtr && list->numOfParams > 0U && list->numOfParams <= 64U) {
        for (std::uint32_t index = 0; index < list->numOfParams; ++index) {
          auto& config = list->configPtr[index];
          if (config.parameter == CONFIG_BAUDRATE) {
            config.value = channel.baudRate;
            return STATUS_NOERROR;
          }
        }
        return ERR_NOT_SUPPORTED;
      }

      const auto config = static_cast<ConfigValue*>(output);
      if (config->parameter == CONFIG_BAUDRATE) {
        config->value = channel.baudRate;
        return STATUS_NOERROR;
      }
    }
    return ERR_NOT_SUPPORTED;
  default:
    return ERR_NOT_SUPPORTED;
  }
}

bool Driver::isOpen() const {
  return open_;
}

} // namespace mvci