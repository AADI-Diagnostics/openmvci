#include <iostream>
#include <string>
#include <vector>

#include "mvci/api.hpp"
#include "mvci/uds.hpp"

int main(int argc, char** argv) {
  const std::string deviceName = argc > 1 ? argv[1] : "loopback";
  mvci::DeviceHandle deviceId = 0;
  mvci::ChannelHandle channelId = 0;

  if (PassThruOpen(deviceName.c_str(), &deviceId) != mvci::STATUS_NOERROR) {
    std::cerr << "Failed to open MVCI device\n";
    return 1;
  }

  if (PassThruConnect(deviceId, mvci::PROTOCOL_ISO15765, 0, 500000, &channelId) != mvci::STATUS_NOERROR) {
    std::cerr << "Failed to connect ISO15765\n";
    PassThruClose(deviceId);
    return 1;
  }

  std::vector<mvci::DtcRecord> dtcs;
  const auto status = mvci::readActiveDtcs(channelId, dtcs, 1500, 0xFFU);
  if (status == mvci::STATUS_NOERROR) {
    std::cout << "Read " << dtcs.size() << " DTC(s)\n";
  } else {
    std::cerr << "Read failed: " << mvci::statusToString(status) << '\n';
  }

  PassThruDisconnect(channelId);
  PassThruClose(deviceId);
  return status == mvci::STATUS_NOERROR ? 0 : 1;
}