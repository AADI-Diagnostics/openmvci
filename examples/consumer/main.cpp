#include <iostream>

#include "mvci/api.hpp"

int main() {
  mvci::DeviceHandle deviceId = 0;
  if (PassThruOpen("loopback", &deviceId) != mvci::STATUS_NOERROR) {
    std::cerr << "Failed to open OpenMVCI\n";
    return 1;
  }

  PassThruClose(deviceId);
  std::cout << "OpenMVCI consumer example linked successfully\n";
  return 0;
}
