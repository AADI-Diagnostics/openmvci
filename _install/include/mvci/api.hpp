#pragma once

#include <cstdint>

#include "mvci/j2534.hpp"

#if defined(_WIN32)
#  ifdef MVCI_BUILDING_DLL
#    define MVCI_API extern "C" __declspec(dllexport)
#  else
#    define MVCI_API extern "C" __declspec(dllimport)
#  endif
#else
#  define MVCI_API extern "C" __attribute__((visibility("default")))
#endif

MVCI_API mvci::Status PassThruOpen(const char* deviceName, mvci::DeviceHandle* deviceId);
MVCI_API mvci::Status PassThruClose(mvci::DeviceHandle deviceId);
MVCI_API mvci::Status PassThruConnect(mvci::DeviceHandle deviceId,
                                      std::uint32_t protocolId,
                                      std::uint32_t flags,
                                      std::uint32_t baudRate,
                                      mvci::ChannelHandle* channelId);
MVCI_API mvci::Status PassThruDisconnect(mvci::ChannelHandle channelId);
MVCI_API mvci::Status PassThruWriteMsgs(mvci::ChannelHandle channelId,
                                        const mvci::PassThruMsg* msgs,
                                        std::uint32_t* numMsgs,
                                        std::uint32_t timeoutMs);
MVCI_API mvci::Status PassThruReadMsgs(mvci::ChannelHandle channelId,
                                       mvci::PassThruMsg* msgs,
                                       std::uint32_t* numMsgs,
                                       std::uint32_t timeoutMs);
MVCI_API mvci::Status PassThruIoctl(mvci::ChannelHandle channelId,
                                    std::uint32_t ioctlId,
                                    void* input,
                                    void* output);