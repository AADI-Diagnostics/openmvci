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
  std::unordered_map<ChannelHandle, ChannelState> channels_;
};

std::unique_ptr<ITransport> createPlatformTransport();

} // namespace mvci