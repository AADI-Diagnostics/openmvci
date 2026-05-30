#include "mvci/api.hpp"

MVCI_API mvci::Status MVCI_CALL MVCI_OpenDevice(const char* deviceName, mvci::DeviceHandle* deviceId) {
  return PassThruOpen(deviceName, deviceId);
}

MVCI_API mvci::Status MVCI_CALL MVCI_CloseDevice(mvci::DeviceHandle deviceId) {
  return PassThruClose(deviceId);
}

MVCI_API mvci::Status MVCI_CALL MVCI_Connect(mvci::DeviceHandle deviceId,
                                             std::uint32_t protocolId,
                                             std::uint32_t flags,
                                             std::uint32_t baudRate,
                                             mvci::ChannelHandle* channelId) {
  return PassThruConnect(deviceId, protocolId, flags, baudRate, channelId);
}

MVCI_API mvci::Status MVCI_CALL MVCI_Disconnect(mvci::ChannelHandle channelId) {
  return PassThruDisconnect(channelId);
}

MVCI_API mvci::Status MVCI_CALL MVCI_WriteMsgs(mvci::ChannelHandle channelId,
                                               const mvci::PassThruMsg* msgs,
                                               std::uint32_t* numMsgs,
                                               std::uint32_t timeoutMs) {
  return PassThruWriteMsgs(channelId, msgs, numMsgs, timeoutMs);
}

MVCI_API mvci::Status MVCI_CALL MVCI_ReadMsgs(mvci::ChannelHandle channelId,
                                              mvci::PassThruMsg* msgs,
                                              std::uint32_t* numMsgs,
                                              std::uint32_t timeoutMs) {
  return PassThruReadMsgs(channelId, msgs, numMsgs, timeoutMs);
}

MVCI_API mvci::Status MVCI_CALL MVCI_StartPeriodicMsg(mvci::ChannelHandle channelId,
                                                      const mvci::PassThruMsg* msg,
                                                      std::uint32_t* msgId,
                                                      std::uint32_t timeIntervalMs) {
  return PassThruStartPeriodicMsg(channelId, msg, msgId, timeIntervalMs);
}

MVCI_API mvci::Status MVCI_CALL MVCI_StopPeriodicMsg(mvci::ChannelHandle channelId,
                                                     std::uint32_t msgId) {
  return PassThruStopPeriodicMsg(channelId, msgId);
}

MVCI_API mvci::Status MVCI_CALL MVCI_StartMsgFilter(mvci::ChannelHandle channelId,
                                                    std::uint32_t filterType,
                                                    const mvci::PassThruMsg* maskMsg,
                                                    const mvci::PassThruMsg* patternMsg,
                                                    const mvci::PassThruMsg* flowControlMsg,
                                                    std::uint32_t* filterId) {
  return PassThruStartMsgFilter(channelId, filterType, maskMsg, patternMsg, flowControlMsg, filterId);
}

MVCI_API mvci::Status MVCI_CALL MVCI_StopMsgFilter(mvci::ChannelHandle channelId,
                                                   std::uint32_t filterId) {
  return PassThruStopMsgFilter(channelId, filterId);
}

MVCI_API mvci::Status MVCI_CALL MVCI_SetProgrammingVoltage(mvci::DeviceHandle deviceId,
                                                           std::uint32_t pin,
                                                           std::uint32_t voltage) {
  return PassThruSetProgrammingVoltage(deviceId, pin, voltage);
}

MVCI_API mvci::Status MVCI_CALL MVCI_Ioctl(mvci::ChannelHandle channelId,
                                           std::uint32_t ioctlId,
                                           void* input,
                                           void* output) {
  return PassThruIoctl(channelId, ioctlId, input, output);
}

MVCI_API mvci::Status MVCI_CALL MVCI_ReadVersion(mvci::DeviceHandle deviceId,
                                                 char* firmwareVersion,
                                                 char* dllVersion,
                                                 char* apiVersion) {
  return PassThruReadVersion(deviceId, firmwareVersion, dllVersion, apiVersion);
}

MVCI_API mvci::Status MVCI_CALL MVCI_GetLastError(char* errorDescription) {
  return PassThruGetLastError(errorDescription);
}
