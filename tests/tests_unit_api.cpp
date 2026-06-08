#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstring>

#include "mvci/api.hpp"

TEST_CASE("MVCI (J2534-compat) API smoke (loopback simulation removed; error paths)") {
  mvci::DeviceHandle deviceId = 0;
  mvci::ChannelHandle channelId = 0;

  // No loopback simulation anymore. Unknown device selector must fail to open.
  CHECK(MVCI_OpenDevice("nonexistent-device-xyz", &deviceId) != mvci::STATUS_NOERROR);

  // Negative cases on invalid handles for the compat layer (must not crash).
  CHECK(MVCI_Connect(0, mvci::PROTOCOL_CAN, 0, 500000, &channelId) != mvci::STATUS_NOERROR);
  CHECK(MVCI_CloseDevice(999) != mvci::STATUS_NOERROR);

  // Version / last-error on invalid device should not crash (exact status + buffer contents are impl-defined).
  char fw[80] = {};
  char dll[80] = {};
  char api[80] = {};
  (void)MVCI_ReadVersion(0, fw, dll, api);

  char err[80] = {};
  (void)MVCI_GetLastError(err);

  (void)MVCI_Disconnect(channelId);
  (void)MVCI_CloseDevice(deviceId);
}
