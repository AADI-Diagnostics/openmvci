#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "mvci/j2534.hpp"

namespace mvci {

class ITransport;

class Driver {
public:
  Driver();
  ~Driver();

  Status open(const std::string& deviceName);
  void close();

  Status connect(std::uint32_t protocolId,
                 std::uint32_t flags,
                 std::uint32_t baudRate,
                 ChannelHandle& channelId);
  Status disconnect(ChannelHandle channelId);

  Status write(ChannelHandle channelId,
               const PassThruMsg* msgs,
               std::uint32_t& numMsgs,
               std::uint32_t timeoutMs);
  Status read(ChannelHandle channelId,
              PassThruMsg* msgs,
              std::uint32_t& numMsgs,
              std::uint32_t timeoutMs);
  Status startPeriodic(ChannelHandle channelId,
                       const PassThruMsg* msg,
                       std::uint32_t& msgId,
                       std::uint32_t timeIntervalMs);
  Status stopPeriodic(ChannelHandle channelId, std::uint32_t msgId);
  Status startFilter(ChannelHandle channelId,
                     std::uint32_t filterType,
                     const PassThruMsg* maskMsg,
                     const PassThruMsg* patternMsg,
                     const PassThruMsg* flowControlMsg,
                     std::uint32_t& filterId);
  Status stopFilter(ChannelHandle channelId, std::uint32_t filterId);
  Status setProgrammingVoltage(std::uint32_t pin, std::uint32_t voltage);
  Status readVersion(char* firmwareVersion, char* dllVersion, char* apiVersion);
  Status ioctl(ChannelHandle channelId,
               std::uint32_t ioctlId,
               void* input,
               void* output);

  bool isOpen() const;

private:
  struct ChannelState {
    std::uint32_t protocolId{0};
    std::uint32_t flags{0};
    std::uint32_t baudRate{0};
  };

  std::unique_ptr<ITransport> transport_;
  bool open_{false};
  std::uint32_t nextChannel_{1};
  std::uint32_t nextPeriodicId_{1};
  std::uint32_t nextFilterId_{1};
  std::unordered_map<ChannelHandle, ChannelState> channels_;
  std::unordered_map<ChannelHandle, std::unordered_map<std::uint32_t, PassThruMsg>> periodicMessages_;
  std::unordered_map<ChannelHandle, std::unordered_map<std::uint32_t, std::uint32_t>> filters_;
};

std::unique_ptr<ITransport> createPlatformTransport();

} // namespace mvci