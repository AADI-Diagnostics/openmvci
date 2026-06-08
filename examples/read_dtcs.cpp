#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "mvci/api.hpp"
#include "mvci/uds.hpp"

int main(int argc, char** argv) {
  const std::string deviceName = argc > 1 ? argv[1] : "";
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
    std::cerr << "Failed to open MVCI device\n";
    return 1;
  } else if (selector == nullptr) {
    // Auto-discovery succeeded with a real adapter (deviceName was empty).
    std::cout << "Using real MVCI adapter via auto-discovery.\n";
  }

  if (PassThruConnect(deviceId, mvci::PROTOCOL_ISO15765, 0, 500000, &channelId) != mvci::STATUS_NOERROR) {
    std::cerr << "Failed to connect ISO15765\n";
    PassThruClose(deviceId);
    return 1;
  }
  std::cout << "Connected ISO15765 channelId=" << channelId << " @ 500000 baud\n";

  // Use a slightly longer timeout for the first commands after bootstrap on real
  // hardware; some clones need extra time (or recent keepalives) before the first
  // ISO15765 frame is accepted/responded to.
  std::string vin;
  const auto vinStatus = mvci::readVehicleVin(channelId, vin, 5000);
  if (vinStatus == mvci::STATUS_NOERROR) {
    std::cout << "VIN: " << vin << '\n';
  } else {
    std::cout << "VIN unavailable: " << mvci::statusToString(vinStatus) << '\n';
    if (vinStatus == mvci::ERR_TIMEOUT) {
      std::cout << "Hint: for many GM vehicles (e.g. 2013 Cruze) set MVCI_OBD_CAN_ID=0x7E0 before running.\n";
      std::cout << "      Also ensure ignition is ON (RUN position) and the adapter is connected to the OBD port.\n";
    }
  }

  std::vector<mvci::DtcRecord> dtcs;
  const auto status = mvci::readActiveDtcs(channelId, dtcs, 5000, 0xFFU);
  if (status == mvci::STATUS_NOERROR) {
    std::cout << "Read " << dtcs.size() << " DTC(s)\n";
    for (const auto& dtc : dtcs) {
      if (dtc.ecuAddress != 0U) {
        std::cout << "  ECU 0x" << std::hex << std::uppercase << std::setw(3) << std::setfill('0')
                  << dtc.ecuAddress << std::dec << std::setfill(' ') << ": ";
      }
      std::cout << mvci::formatDtc(dtc.code)
                << " status=0x" << std::hex << static_cast<unsigned>(dtc.status) << std::dec << '\n';
    }
  } else {
    std::cerr << "Read failed: " << mvci::statusToString(status) << '\n';
    if (status == mvci::ERR_TIMEOUT) {
      std::cerr << "Hint: try MVCI_OBD_CAN_ID=0x7E0 (physical ECM address) and/or ignition ON.\n";
    }
  }

  PassThruDisconnect(channelId);
  PassThruClose(deviceId);
  return status == mvci::STATUS_NOERROR ? 0 : 1;
}