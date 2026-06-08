#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstring>

#include "mvci/api.hpp"

TEST_CASE("J2534 API smoke (loopback simulation removed; verifies error paths without hardware)") {
  mvci::DeviceHandle deviceId = 0;
  mvci::ChannelHandle channelId = 0;

  // Loopback simulation transport has been removed. Opening a non-existent selector must fail.
  CHECK(PassThruOpen("nonexistent-device-xyz", &deviceId) != mvci::STATUS_NOERROR);

  // Calls on invalid handles should fail with appropriate codes (no crash).
  CHECK(PassThruConnect(0, mvci::PROTOCOL_CAN, 0, 500000, &channelId) != mvci::STATUS_NOERROR);
  CHECK(PassThruClose(999) != mvci::STATUS_NOERROR);

  // Version / last-error on invalid device should not crash (status may vary by implementation).
  char fw[80] = {};
  char dll[80] = {};
  char api[80] = {};
  (void)PassThruReadVersion(0, fw, dll, api);

  char err[80] = {};
  (void)PassThruGetLastError(err);
}