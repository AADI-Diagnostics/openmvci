#include <iostream>

#include "mvci/api.hpp"

int main() {
  mvci::DeviceHandle deviceId = 0;
  if (PassThruOpen("loopback", &deviceId) != mvci::STATUS_NOERROR) {
    std::cerr << "Failed to open MVCI32\n";
    return 1;
  }

  PassThruClose(deviceId);
  std::cout << "MVCI32 consumer example linked successfully\n";
  return 0;
}
