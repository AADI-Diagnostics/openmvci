#pragma once

#include <cstdint>

#include "mvci/j2534.hpp"

#if defined(_WIN32)
#  define MVCI_CALL __stdcall
#  ifdef MVCI_BUILDING_DLL
#    define MVCI_API extern "C" __declspec(dllexport)
#    define MVCI_CPP_API __declspec(dllexport)
#  else
#    define MVCI_API extern "C" __declspec(dllimport)
#    define MVCI_CPP_API __declspec(dllimport)
#  endif
#else
#  define MVCI_CALL
#  define MVCI_API extern "C" __attribute__((visibility("default")))
#  define MVCI_CPP_API __attribute__((visibility("default")))
#endif

MVCI_API mvci::Status MVCI_CALL PassThruOpen(const char* deviceName, mvci::DeviceHandle* deviceId);
MVCI_API mvci::Status MVCI_CALL PassThruClose(mvci::DeviceHandle deviceId);
MVCI_API mvci::Status MVCI_CALL PassThruConnect(mvci::DeviceHandle deviceId,
                                                std::uint32_t protocolId,
                                                std::uint32_t flags,
                                                std::uint32_t baudRate,
                                                mvci::ChannelHandle* channelId);
MVCI_API mvci::Status MVCI_CALL PassThruDisconnect(mvci::ChannelHandle channelId);
MVCI_API mvci::Status MVCI_CALL PassThruWriteMsgs(mvci::ChannelHandle channelId,
                                                  const mvci::PassThruMsg* msgs,
                                                  std::uint32_t* numMsgs,
                                                  std::uint32_t timeoutMs);
MVCI_API mvci::Status MVCI_CALL PassThruReadMsgs(mvci::ChannelHandle channelId,
                                                 mvci::PassThruMsg* msgs,
                                                 std::uint32_t* numMsgs,
                                                 std::uint32_t timeoutMs);
MVCI_API mvci::Status MVCI_CALL PassThruStartPeriodicMsg(mvci::ChannelHandle channelId,
                                                         const mvci::PassThruMsg* msg,
                                                         std::uint32_t* msgId,
                                                         std::uint32_t timeIntervalMs);
MVCI_API mvci::Status MVCI_CALL PassThruStopPeriodicMsg(mvci::ChannelHandle channelId,
                                                        std::uint32_t msgId);
MVCI_API mvci::Status MVCI_CALL PassThruStartMsgFilter(mvci::ChannelHandle channelId,
                                                       std::uint32_t filterType,
                                                       const mvci::PassThruMsg* maskMsg,
                                                       const mvci::PassThruMsg* patternMsg,
                                                       const mvci::PassThruMsg* flowControlMsg,
                                                       std::uint32_t* filterId);
MVCI_API mvci::Status MVCI_CALL PassThruStopMsgFilter(mvci::ChannelHandle channelId,
                                                      std::uint32_t filterId);
MVCI_API mvci::Status MVCI_CALL PassThruSetProgrammingVoltage(mvci::DeviceHandle deviceId,
                                                              std::uint32_t pin,
                                                              std::uint32_t voltage);
MVCI_API mvci::Status MVCI_CALL PassThruIoctl(mvci::ChannelHandle channelId,
                                              std::uint32_t ioctlId,
                                              void* input,
                                              void* output);
MVCI_API mvci::Status MVCI_CALL PassThruReadVersion(mvci::DeviceHandle deviceId,
                                                    char* firmwareVersion,
                                                    char* dllVersion,
                                                    char* apiVersion);
MVCI_API mvci::Status MVCI_CALL PassThruGetLastError(char* errorDescription);

MVCI_API mvci::Status MVCI_CALL MVCI_OpenDevice(const char* deviceName, mvci::DeviceHandle* deviceId);
MVCI_API mvci::Status MVCI_CALL MVCI_CloseDevice(mvci::DeviceHandle deviceId);
MVCI_API mvci::Status MVCI_CALL MVCI_Connect(mvci::DeviceHandle deviceId,
                                             std::uint32_t protocolId,
                                             std::uint32_t flags,
                                             std::uint32_t baudRate,
                                             mvci::ChannelHandle* channelId);
MVCI_API mvci::Status MVCI_CALL MVCI_Disconnect(mvci::ChannelHandle channelId);
MVCI_API mvci::Status MVCI_CALL MVCI_WriteMsgs(mvci::ChannelHandle channelId,
                                               const mvci::PassThruMsg* msgs,
                                               std::uint32_t* numMsgs,
                                               std::uint32_t timeoutMs);
MVCI_API mvci::Status MVCI_CALL MVCI_ReadMsgs(mvci::ChannelHandle channelId,
                                              mvci::PassThruMsg* msgs,
                                              std::uint32_t* numMsgs,
                                              std::uint32_t timeoutMs);
MVCI_API mvci::Status MVCI_CALL MVCI_StartPeriodicMsg(mvci::ChannelHandle channelId,
                                                      const mvci::PassThruMsg* msg,
                                                      std::uint32_t* msgId,
                                                      std::uint32_t timeIntervalMs);
MVCI_API mvci::Status MVCI_CALL MVCI_StopPeriodicMsg(mvci::ChannelHandle channelId,
                                                     std::uint32_t msgId);
MVCI_API mvci::Status MVCI_CALL MVCI_StartMsgFilter(mvci::ChannelHandle channelId,
                                                    std::uint32_t filterType,
                                                    const mvci::PassThruMsg* maskMsg,
                                                    const mvci::PassThruMsg* patternMsg,
                                                    const mvci::PassThruMsg* flowControlMsg,
                                                    std::uint32_t* filterId);
MVCI_API mvci::Status MVCI_CALL MVCI_StopMsgFilter(mvci::ChannelHandle channelId,
                                                   std::uint32_t filterId);
MVCI_API mvci::Status MVCI_CALL MVCI_SetProgrammingVoltage(mvci::DeviceHandle deviceId,
                                                           std::uint32_t pin,
                                                           std::uint32_t voltage);
MVCI_API mvci::Status MVCI_CALL MVCI_Ioctl(mvci::ChannelHandle channelId,
                                           std::uint32_t ioctlId,
                                           void* input,
                                           void* output);
MVCI_API mvci::Status MVCI_CALL MVCI_ReadVersion(mvci::DeviceHandle deviceId,
                                                 char* firmwareVersion,
                                                 char* dllVersion,
                                                 char* apiVersion);
MVCI_API mvci::Status MVCI_CALL MVCI_GetLastError(char* errorDescription);