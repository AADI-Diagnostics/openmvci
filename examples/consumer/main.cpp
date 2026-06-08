#include <iostream>

#include "mvci/api.hpp"

int main() {
  mvci::DeviceHandle deviceId = 0;
  // Try auto-discovery (no device name). This will fail on machines without a real adapter,
  // which is fine for a basic link/runtime smoke of the consumer example.
  if (PassThruOpen(nullptr, &deviceId) != mvci::STATUS_NOERROR) {
    std::cout << "No MVCI adapter present (auto open failed) -- this is expected without hardware.\n";
  } else {
    PassThruClose(deviceId);
  }
  std::cout << "OpenMVCI consumer example linked successfully\n";
  return 0;
}
