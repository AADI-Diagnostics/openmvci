#include "mvci/api.hpp"

#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "mvci/driver.hpp"

namespace {

struct ChannelBinding {
  mvci::DeviceHandle deviceId{0};
  mvci::ChannelHandle internalChannelId{0};
};

class Registry {
public:
  mvci::Status open(const char* deviceName, mvci::DeviceHandle* deviceId) {
    if (!deviceId) {
      return mvci::ERR_INVALID_MSG;
    }

    auto driver = std::make_shared<mvci::Driver>();
    const auto status = driver->open(deviceName ? deviceName : "");
    if (status != mvci::STATUS_NOERROR) {
      return status;
    }

    const auto newDeviceId = nextDeviceId_++;
    devices_.emplace(newDeviceId, std::move(driver));
    *deviceId = newDeviceId;
    return mvci::STATUS_NOERROR;
  }

  mvci::Status close(mvci::DeviceHandle deviceId) {
    const auto deviceIt = devices_.find(deviceId);
    if (deviceIt == devices_.end()) {
      return mvci::ERR_INVALID_DEVICE_ID;
    }

    for (auto it = channels_.begin(); it != channels_.end();) {
      if (it->second.deviceId == deviceId) {
        deviceIt->second->disconnect(it->second.internalChannelId);
        it = channels_.erase(it);
      } else {
        ++it;
      }
    }

    deviceIt->second->close();
    devices_.erase(deviceIt);
    return mvci::STATUS_NOERROR;
  }

  mvci::Status connect(mvci::DeviceHandle deviceId,
                       std::uint32_t protocolId,
                       std::uint32_t flags,
                       std::uint32_t baudRate,
                       mvci::ChannelHandle* channelId) {
    if (!channelId) {
      return mvci::ERR_INVALID_MSG;
    }

    const auto deviceIt = devices_.find(deviceId);
    if (deviceIt == devices_.end()) {
      return mvci::ERR_INVALID_DEVICE_ID;
    }

    mvci::ChannelHandle internalChannelId = 0;
    const auto status = deviceIt->second->connect(protocolId, flags, baudRate, internalChannelId);
    if (status != mvci::STATUS_NOERROR) {
      return status;
    }

    const auto externalChannelId = nextChannelId_++;
    channels_.emplace(externalChannelId, ChannelBinding{deviceId, internalChannelId});
    *channelId = externalChannelId;
    return mvci::STATUS_NOERROR;
  }

  mvci::Status disconnect(mvci::ChannelHandle channelId) {
    const auto channelIt = channels_.find(channelId);
    if (channelIt == channels_.end()) {
      return mvci::ERR_INVALID_CHANNEL_ID;
    }

    const auto deviceIt = devices_.find(channelIt->second.deviceId);
    if (deviceIt != devices_.end()) {
      deviceIt->second->disconnect(channelIt->second.internalChannelId);
    }

    channels_.erase(channelIt);
    return mvci::STATUS_NOERROR;
  }

  mvci::Status write(mvci::ChannelHandle channelId,
                     const mvci::PassThruMsg* msgs,
                     std::uint32_t* numMsgs,
                     std::uint32_t timeoutMs) {
    if (!msgs || !numMsgs || *numMsgs == 0) {
      return mvci::ERR_INVALID_MSG;
    }

    const auto bindingIt = channels_.find(channelId);
    if (bindingIt == channels_.end()) {
      return mvci::ERR_INVALID_CHANNEL_ID;
    }

    const auto deviceIt = devices_.find(bindingIt->second.deviceId);
    if (deviceIt == devices_.end()) {
      return mvci::ERR_INVALID_DEVICE_ID;
    }

    return deviceIt->second->write(bindingIt->second.internalChannelId, msgs, *numMsgs, timeoutMs);
  }

  mvci::Status read(mvci::ChannelHandle channelId,
                    mvci::PassThruMsg* msgs,
                    std::uint32_t* numMsgs,
                    std::uint32_t timeoutMs) {
    if (!msgs || !numMsgs || *numMsgs == 0) {
      return mvci::ERR_INVALID_MSG;
    }

    const auto bindingIt = channels_.find(channelId);
    if (bindingIt == channels_.end()) {
      return mvci::ERR_INVALID_CHANNEL_ID;
    }

    const auto deviceIt = devices_.find(bindingIt->second.deviceId);
    if (deviceIt == devices_.end()) {
      return mvci::ERR_INVALID_DEVICE_ID;
    }

    return deviceIt->second->read(bindingIt->second.internalChannelId, msgs, *numMsgs, timeoutMs);
  }

  mvci::Status startPeriodic(mvci::ChannelHandle channelId,
                             const mvci::PassThruMsg* msg,
                             std::uint32_t* msgId,
                             std::uint32_t timeIntervalMs) {
    if (!msgId) {
      return mvci::ERR_INVALID_MSG;
    }

    const auto bindingIt = channels_.find(channelId);
    if (bindingIt == channels_.end()) {
      return mvci::ERR_INVALID_CHANNEL_ID;
    }

    const auto deviceIt = devices_.find(bindingIt->second.deviceId);
    if (deviceIt == devices_.end()) {
      return mvci::ERR_INVALID_DEVICE_ID;
    }

    return deviceIt->second->startPeriodic(bindingIt->second.internalChannelId, msg, *msgId, timeIntervalMs);
  }

  mvci::Status stopPeriodic(mvci::ChannelHandle channelId, std::uint32_t msgId) {
    const auto bindingIt = channels_.find(channelId);
    if (bindingIt == channels_.end()) {
      return mvci::ERR_INVALID_CHANNEL_ID;
    }

    const auto deviceIt = devices_.find(bindingIt->second.deviceId);
    if (deviceIt == devices_.end()) {
      return mvci::ERR_INVALID_DEVICE_ID;
    }

    return deviceIt->second->stopPeriodic(bindingIt->second.internalChannelId, msgId);
  }

  mvci::Status startFilter(mvci::ChannelHandle channelId,
                           std::uint32_t filterType,
                           const mvci::PassThruMsg* maskMsg,
                           const mvci::PassThruMsg* patternMsg,
                           const mvci::PassThruMsg* flowControlMsg,
                           std::uint32_t* filterId) {
    if (!filterId) {
      return mvci::ERR_INVALID_MSG;
    }

    const auto bindingIt = channels_.find(channelId);
    if (bindingIt == channels_.end()) {
      return mvci::ERR_INVALID_CHANNEL_ID;
    }

    const auto deviceIt = devices_.find(bindingIt->second.deviceId);
    if (deviceIt == devices_.end()) {
      return mvci::ERR_INVALID_DEVICE_ID;
    }

    return deviceIt->second->startFilter(bindingIt->second.internalChannelId,
                                         filterType,
                                         maskMsg,
                                         patternMsg,
                                         flowControlMsg,
                                         *filterId);
  }

  mvci::Status stopFilter(mvci::ChannelHandle channelId, std::uint32_t filterId) {
    const auto bindingIt = channels_.find(channelId);
    if (bindingIt == channels_.end()) {
      return mvci::ERR_INVALID_CHANNEL_ID;
    }

    const auto deviceIt = devices_.find(bindingIt->second.deviceId);
    if (deviceIt == devices_.end()) {
      return mvci::ERR_INVALID_DEVICE_ID;
    }

    return deviceIt->second->stopFilter(bindingIt->second.internalChannelId, filterId);
  }

  mvci::Status setProgrammingVoltage(mvci::DeviceHandle deviceId,
                                     std::uint32_t pin,
                                     std::uint32_t voltage) {
    const auto deviceIt = devices_.find(deviceId);
    if (deviceIt == devices_.end()) {
      return mvci::ERR_INVALID_DEVICE_ID;
    }

    return deviceIt->second->setProgrammingVoltage(pin, voltage);
  }

  mvci::Status readVersion(mvci::DeviceHandle deviceId,
                           char* firmwareVersion,
                           char* dllVersion,
                           char* apiVersion) {
    const auto deviceIt = devices_.find(deviceId);
    if (deviceIt == devices_.end()) {
      return mvci::ERR_INVALID_DEVICE_ID;
    }

    return deviceIt->second->readVersion(firmwareVersion, dllVersion, apiVersion);
  }

  mvci::Status ioctl(mvci::ChannelHandle channelId, std::uint32_t ioctlId, void* input, void* output) {
    const auto bindingIt = channels_.find(channelId);
    if (bindingIt == channels_.end()) {
      return mvci::ERR_INVALID_CHANNEL_ID;
    }

    const auto deviceIt = devices_.find(bindingIt->second.deviceId);
    if (deviceIt == devices_.end()) {
      return mvci::ERR_INVALID_DEVICE_ID;
    }

    return deviceIt->second->ioctl(bindingIt->second.internalChannelId, ioctlId, input, output);
  }

private:
  std::uint32_t nextDeviceId_{1};
  std::uint32_t nextChannelId_{1};
  std::unordered_map<mvci::DeviceHandle, std::shared_ptr<mvci::Driver>> devices_;
  std::unordered_map<mvci::ChannelHandle, ChannelBinding> channels_;
};

Registry& registry() {
  static Registry instance;
  return instance;
}

std::mutex& registryMutex() {
  static std::mutex mutex;
  return mutex;
}

thread_local std::array<char, 80> lastErrorBuffer{"No error"};

void updateLastError(mvci::Status status) {
  const char* message = "Unknown error";
  switch (status) {
  case mvci::STATUS_NOERROR:
    message = "No error";
    break;
  case mvci::ERR_FAILED:
    message = "General failure";
    break;
  case mvci::ERR_NOT_INITIALIZED:
    message = "Device not initialized";
    break;
  case mvci::ERR_INVALID_DEVICE_ID:
    message = "Invalid device ID";
    break;
  case mvci::ERR_INVALID_CHANNEL_ID:
    message = "Invalid channel ID";
    break;
  case mvci::ERR_INVALID_MSG:
    message = "Invalid message";
    break;
  case mvci::ERR_TIMEOUT:
    message = "Timeout";
    break;
  case mvci::ERR_BUFFER_EMPTY:
    message = "Buffer empty";
    break;
  case mvci::ERR_NOT_SUPPORTED:
    message = "Not supported";
    break;
  default:
    break;
  }

  std::strncpy(lastErrorBuffer.data(), message, lastErrorBuffer.size() - 1U);
  lastErrorBuffer[lastErrorBuffer.size() - 1U] = '\0';
}

mvci::Status recordResult(mvci::Status status) {
  updateLastError(status);
  return status;
}

} // namespace

MVCI_API mvci::Status MVCI_CALL PassThruOpen(const char* deviceName, mvci::DeviceHandle* deviceId) {
  std::lock_guard<std::mutex> lock(registryMutex());
  return recordResult(registry().open(deviceName, deviceId));
}

MVCI_API mvci::Status MVCI_CALL PassThruClose(mvci::DeviceHandle deviceId) {
  std::lock_guard<std::mutex> lock(registryMutex());
  return recordResult(registry().close(deviceId));
}

MVCI_API mvci::Status MVCI_CALL PassThruConnect(mvci::DeviceHandle deviceId,
                                                std::uint32_t protocolId,
                                                std::uint32_t flags,
                                                std::uint32_t baudRate,
                                                mvci::ChannelHandle* channelId) {
  std::lock_guard<std::mutex> lock(registryMutex());
  return recordResult(registry().connect(deviceId, protocolId, flags, baudRate, channelId));
}

MVCI_API mvci::Status MVCI_CALL PassThruDisconnect(mvci::ChannelHandle channelId) {
  std::lock_guard<std::mutex> lock(registryMutex());
  return recordResult(registry().disconnect(channelId));
}

MVCI_API mvci::Status MVCI_CALL PassThruWriteMsgs(mvci::ChannelHandle channelId,
                                                  const mvci::PassThruMsg* msgs,
                                                  std::uint32_t* numMsgs,
                                                  std::uint32_t timeoutMs) {
  std::lock_guard<std::mutex> lock(registryMutex());
  return recordResult(registry().write(channelId, msgs, numMsgs, timeoutMs));
}

MVCI_API mvci::Status MVCI_CALL PassThruReadMsgs(mvci::ChannelHandle channelId,
                                                 mvci::PassThruMsg* msgs,
                                                 std::uint32_t* numMsgs,
                                                 std::uint32_t timeoutMs) {
  std::lock_guard<std::mutex> lock(registryMutex());
  return recordResult(registry().read(channelId, msgs, numMsgs, timeoutMs));
}

MVCI_API mvci::Status MVCI_CALL PassThruStartPeriodicMsg(mvci::ChannelHandle channelId,
                                                         const mvci::PassThruMsg* msg,
                                                         std::uint32_t* msgId,
                                                         std::uint32_t timeIntervalMs) {
  std::lock_guard<std::mutex> lock(registryMutex());
  return recordResult(registry().startPeriodic(channelId, msg, msgId, timeIntervalMs));
}

MVCI_API mvci::Status MVCI_CALL PassThruStopPeriodicMsg(mvci::ChannelHandle channelId,
                                                        std::uint32_t msgId) {
  std::lock_guard<std::mutex> lock(registryMutex());
  return recordResult(registry().stopPeriodic(channelId, msgId));
}

MVCI_API mvci::Status MVCI_CALL PassThruStartMsgFilter(mvci::ChannelHandle channelId,
                                                       std::uint32_t filterType,
                                                       const mvci::PassThruMsg* maskMsg,
                                                       const mvci::PassThruMsg* patternMsg,
                                                       const mvci::PassThruMsg* flowControlMsg,
                                                       std::uint32_t* filterId) {
  std::lock_guard<std::mutex> lock(registryMutex());
  return recordResult(registry().startFilter(channelId, filterType, maskMsg, patternMsg, flowControlMsg, filterId));
}

MVCI_API mvci::Status MVCI_CALL PassThruStopMsgFilter(mvci::ChannelHandle channelId,
                                                      std::uint32_t filterId) {
  std::lock_guard<std::mutex> lock(registryMutex());
  return recordResult(registry().stopFilter(channelId, filterId));
}

MVCI_API mvci::Status MVCI_CALL PassThruSetProgrammingVoltage(mvci::DeviceHandle deviceId,
                                                              std::uint32_t pin,
                                                              std::uint32_t voltage) {
  std::lock_guard<std::mutex> lock(registryMutex());
  return recordResult(registry().setProgrammingVoltage(deviceId, pin, voltage));
}

MVCI_API mvci::Status MVCI_CALL PassThruIoctl(mvci::ChannelHandle channelId,
                                              std::uint32_t ioctlId,
                                              void* input,
                                              void* output) {
  std::lock_guard<std::mutex> lock(registryMutex());
  return recordResult(registry().ioctl(channelId, ioctlId, input, output));
}

MVCI_API mvci::Status MVCI_CALL PassThruReadVersion(mvci::DeviceHandle deviceId,
                                                    char* firmwareVersion,
                                                    char* dllVersion,
                                                    char* apiVersion) {
  std::lock_guard<std::mutex> lock(registryMutex());
  return recordResult(registry().readVersion(deviceId, firmwareVersion, dllVersion, apiVersion));
}

MVCI_API mvci::Status MVCI_CALL PassThruGetLastError(char* errorDescription) {
  if (!errorDescription) {
    return recordResult(mvci::ERR_INVALID_MSG);
  }

  std::strncpy(errorDescription, lastErrorBuffer.data(), 79);
  errorDescription[79] = '\0';
  return recordResult(mvci::STATUS_NOERROR);
}