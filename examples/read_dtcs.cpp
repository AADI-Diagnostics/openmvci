#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "mvci/api.hpp"
#include "mvci/uds.hpp"

int main(int argc, char** argv) {
  const std::string deviceName = argc > 1 ? argv[1] : "";
  bool usingLoopback = deviceName == "";
  mvci::DeviceHandle deviceId = 0;
  mvci::ChannelHandle channelId = 0;

  const char* selector = deviceName.empty() ? nullptr : deviceName.c_str();
  auto openStatus = PassThruOpen(selector, &deviceId);
  if (selector == nullptr && openStatus != mvci::STATUS_NOERROR) {
    for (int attempt = 1; attempt <= 2 && openStatus != mvci::STATUS_NOERROR; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      openStatus = PassThruOpen(selector, &deviceId);
    }
  }
  if (openStatus != mvci::STATUS_NOERROR) {
    if (selector == nullptr) {
      std::cerr << "No MVCI adapter discovered; retrying in loopback simulation mode.\n";
      openStatus = PassThruOpen("loopback", &deviceId);
      usingLoopback = openStatus == mvci::STATUS_NOERROR;
    }
    if (openStatus != mvci::STATUS_NOERROR) {
      std::cerr << "Failed to open MVCI device (auto or loopback)\n";
      return 1;
    }
  }

  if (PassThruConnect(deviceId, mvci::PROTOCOL_ISO15765, 0, 500000, &channelId) != mvci::STATUS_NOERROR) {
    std::cerr << "Failed to connect ISO15765\n";
    PassThruClose(deviceId);
    return 1;
  }

  std::string vin;
  const auto vinStatus = mvci::readVehicleVin(channelId, vin, 1500);
  if (vinStatus == mvci::STATUS_NOERROR) {
    if (usingLoopback) {
      std::cout << "VIN (simulated): " << vin << '\n';
    } else {
      std::cout << "VIN: " << vin << '\n';
    }
  } else {
    std::cout << "VIN unavailable: " << mvci::statusToString(vinStatus) << '\n';
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