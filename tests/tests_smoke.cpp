#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstring>

#include "mvci/api.hpp"

TEST_CASE("J2534 API smoke test via loopback transport") {
  mvci::DeviceHandle deviceId = 0;
  mvci::ChannelHandle channelId = 0;

  REQUIRE(PassThruOpen("loopback", &deviceId) == mvci::STATUS_NOERROR);
  REQUIRE(deviceId != 0);

  REQUIRE(PassThruConnect(deviceId, mvci::PROTOCOL_CAN, 0, 500000, &channelId) == mvci::STATUS_NOERROR);
  REQUIRE(channelId != 0);

  mvci::PassThruMsg writeMsg{};
  writeMsg.protocolId = mvci::PROTOCOL_CAN;
  writeMsg.txFlags = 0;
  writeMsg.dataSize = 8;
  std::uint8_t payload[8] = {0x02, 0x10, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00};
  std::memcpy(writeMsg.data, payload, sizeof(payload));

  std::uint32_t writeCount = 1;
  CHECK(PassThruWriteMsgs(channelId, &writeMsg, &writeCount, 100) == mvci::STATUS_NOERROR);
  CHECK(writeCount == 1);

  mvci::PassThruMsg readMsg{};
  std::uint32_t readCount = 1;
  CHECK(PassThruReadMsgs(channelId, &readMsg, &readCount, 100) == mvci::STATUS_NOERROR);
  CHECK(readCount == 1);
  CHECK(readMsg.dataSize == 8);
  CHECK(readMsg.data[0] == 0x02);
  CHECK(readMsg.data[1] == 0x10);

  mvci::ConfigValue config{mvci::CONFIG_BAUDRATE, 250000};
  CHECK(PassThruIoctl(channelId, mvci::IOCTL_SET_CONFIG, &config, nullptr) == mvci::STATUS_NOERROR);
  config.value = 0;
  CHECK(PassThruIoctl(channelId, mvci::IOCTL_GET_CONFIG, nullptr, &config) == mvci::STATUS_NOERROR);
  CHECK(config.value == 250000);

  mvci::ConfigValue configItem{mvci::CONFIG_BAUDRATE, 500000};
  mvci::ConfigList setList{1, &configItem};
  CHECK(PassThruIoctl(channelId, mvci::IOCTL_SET_CONFIG, &setList, nullptr) == mvci::STATUS_NOERROR);
  configItem.value = 0;
  mvci::ConfigList getList{1, &configItem};
  CHECK(PassThruIoctl(channelId, mvci::IOCTL_GET_CONFIG, nullptr, &getList) == mvci::STATUS_NOERROR);
  CHECK(configItem.value == 500000);

  std::uint32_t batt = 0;
  CHECK(PassThruIoctl(channelId, mvci::IOCTL_READ_BATT_VOLTAGE, nullptr, &batt) == mvci::STATUS_NOERROR);
  CHECK(batt == 12200);

  std::uint32_t periodicId = 0;
  CHECK(PassThruStartPeriodicMsg(channelId, &writeMsg, &periodicId, 100) == mvci::STATUS_NOERROR);
  CHECK(periodicId != 0);
  CHECK(PassThruStopPeriodicMsg(channelId, periodicId) == mvci::STATUS_NOERROR);

  mvci::PassThruMsg mask{};
  mask.protocolId = mvci::PROTOCOL_CAN;
  mask.dataSize = 4;
  std::memset(mask.data, 0xFF, mask.dataSize);

  mvci::PassThruMsg pattern{};
  pattern.protocolId = mvci::PROTOCOL_CAN;
  pattern.dataSize = 4;
  std::memset(pattern.data, 0x00, pattern.dataSize);

  std::uint32_t filterId = 0;
  CHECK(PassThruStartMsgFilter(channelId,
                               mvci::FILTER_PASS,
                               &mask,
                               &pattern,
                               nullptr,
                               &filterId) == mvci::STATUS_NOERROR);
  CHECK(filterId != 0);
  CHECK(PassThruStopMsgFilter(channelId, filterId) == mvci::STATUS_NOERROR);

  CHECK(PassThruSetProgrammingVoltage(deviceId, 6, 0) == mvci::STATUS_NOERROR);

  char fw[80] = {};
  char dll[80] = {};
  char api[80] = {};
  CHECK(PassThruReadVersion(deviceId, fw, dll, api) == mvci::STATUS_NOERROR);
  CHECK(std::strlen(fw) > 0);
  CHECK(std::strlen(dll) > 0);
  CHECK(std::strlen(api) > 0);

  char err[80] = {};
  CHECK(PassThruGetLastError(err) == mvci::STATUS_NOERROR);
  CHECK(std::strlen(err) > 0);

  CHECK(PassThruDisconnect(channelId) == mvci::STATUS_NOERROR);
  CHECK(PassThruClose(deviceId) == mvci::STATUS_NOERROR);
}