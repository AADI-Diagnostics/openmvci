#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vector>

#include "mvci/uds.hpp"

TEST_CASE("buildReadDtcRequest produces correct UDS request") {
  const auto request = mvci::buildReadDtcRequest(0xFFU);
  CHECK(request.size() == 3);
  CHECK(request[0] == 0x19U);
  CHECK(request[1] == 0x02U);
  CHECK(request[2] == 0xFFU);
}

TEST_CASE("buildClearDtcRequest produces correct request") {
  const auto clearRequest = mvci::buildClearDtcRequest();
  CHECK(clearRequest.size() == 4);
  CHECK(clearRequest[0] == 0x14U);
}

TEST_CASE("parseActiveDtcResponses handles bare UDS response") {
  std::vector<std::vector<std::uint8_t>> responses{{0x59U, 0x02U, 0xFFU, 0x01U, 0x10U, 0x20U, 0xAAU}};
  std::vector<mvci::DtcRecord> dtcs;
  CHECK(mvci::parseActiveDtcResponses(responses, dtcs, 0xFFU) == mvci::STATUS_NOERROR);
  CHECK(dtcs.size() == 1);
  CHECK(dtcs[0].status == 0xAAU);
  CHECK(dtcs[0].ecuAddress == 0U);
  CHECK(mvci::formatDtc(dtcs[0].code) == "0x011020");
}

TEST_CASE("parseActiveDtcResponses extracts ECU address from CAN ID prefixed response") {
  // 0x000007E8 is typical ECM response address (as returned by sendUdsRequest for real ISO15765).
  std::vector<std::vector<std::uint8_t>> prefixedResponses{
      {0x00, 0x00, 0x07, 0xE8, 0x59, 0x02, 0xFF, 0x01, 0x10, 0x20, 0xAA, 0x02, 0x30, 0x40, 0xBB}
  };
  std::vector<mvci::DtcRecord> dtcs;
  CHECK(mvci::parseActiveDtcResponses(prefixedResponses, dtcs, 0xFFU) == mvci::STATUS_NOERROR);
  CHECK(dtcs.size() == 2);
  CHECK(dtcs[0].ecuAddress == 0x000007E8U);
  CHECK(dtcs[0].code == 0x011020U);
  CHECK(dtcs[0].status == 0xAAU);
  CHECK(dtcs[1].ecuAddress == 0x000007E8U);
  CHECK(dtcs[1].code == 0x023040U);
  CHECK(dtcs[1].status == 0xBBU);
}