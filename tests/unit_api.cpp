#include <cassert>
#include <cstring>

#include "mvci/api.hpp"

int main() {
  mvci::DeviceHandle deviceId = 0;
  mvci::ChannelHandle channelId = 0;

  assert(MVCI_OpenDevice("loopback", &deviceId) == mvci::STATUS_NOERROR);
  assert(deviceId != 0);

  assert(MVCI_Connect(deviceId, mvci::PROTOCOL_CAN, 0, 500000, &channelId) == mvci::STATUS_NOERROR);
  assert(channelId != 0);

  mvci::PassThruMsg msg{};
  msg.protocolId = mvci::PROTOCOL_CAN;
  msg.dataSize = 8;
  msg.data[0] = 0x01;

  std::uint32_t periodicId = 0;
  assert(MVCI_StartPeriodicMsg(channelId, &msg, &periodicId, 100) == mvci::STATUS_NOERROR);
  assert(MVCI_StopPeriodicMsg(channelId, periodicId) == mvci::STATUS_NOERROR);

  mvci::PassThruMsg mask{};
  mask.protocolId = mvci::PROTOCOL_CAN;
  mask.dataSize = 4;
  std::memset(mask.data, 0xFF, mask.dataSize);
  mvci::PassThruMsg pattern{};
  pattern.protocolId = mvci::PROTOCOL_CAN;
  pattern.dataSize = 4;

  std::uint32_t filterId = 0;
  assert(MVCI_StartMsgFilter(channelId,
                             mvci::FILTER_PASS,
                             &mask,
                             &pattern,
                             nullptr,
                             &filterId) == mvci::STATUS_NOERROR);
  assert(MVCI_StopMsgFilter(channelId, filterId) == mvci::STATUS_NOERROR);

  assert(MVCI_SetProgrammingVoltage(deviceId, 6, 0) == mvci::STATUS_NOERROR);

  char fw[80] = {};
  char dll[80] = {};
  char api[80] = {};
  assert(MVCI_ReadVersion(deviceId, fw, dll, api) == mvci::STATUS_NOERROR);
  assert(std::strlen(fw) > 0);
  assert(std::strlen(dll) > 0);
  assert(std::strlen(api) > 0);

  char err[80] = {};
  assert(MVCI_GetLastError(err) == mvci::STATUS_NOERROR);
  assert(std::strlen(err) > 0);

  assert(MVCI_Disconnect(channelId) == mvci::STATUS_NOERROR);
  assert(MVCI_CloseDevice(deviceId) == mvci::STATUS_NOERROR);
  return 0;
}
