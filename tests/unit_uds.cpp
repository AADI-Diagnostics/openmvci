#include <cassert>
#include <vector>

#include "mvci/uds.hpp"

int main() {
  const auto request = mvci::buildReadDtcRequest(0xFFU);
  assert(request.size() == 3);
  assert(request[0] == 0x19U);
  assert(request[1] == 0x02U);
  assert(request[2] == 0xFFU);

  const auto clearRequest = mvci::buildClearDtcRequest();
  assert(clearRequest.size() == 4);
  assert(clearRequest[0] == 0x14U);

  std::vector<std::vector<std::uint8_t>> responses{{0x59U, 0x02U, 0xFFU, 0x01U, 0x10U, 0x20U, 0xAAU}};
  std::vector<mvci::DtcRecord> dtcs;
  assert(mvci::parseActiveDtcResponses(responses, dtcs, 0xFFU) == mvci::STATUS_NOERROR);
  assert(dtcs.size() == 1);
  assert(dtcs[0].status == 0xAAU);
  assert(dtcs[0].ecuAddress == 0U);
  assert(mvci::formatDtc(dtcs[0].code) == "0x011020");

  // Test with CAN ID prefix (as now returned by sendUdsRequest for real ISO15765 responses)
  // 0x000007E8 is typical ECM response address.
  std::vector<std::vector<std::uint8_t>> prefixedResponses{
      {0x00, 0x00, 0x07, 0xE8, 0x59, 0x02, 0xFF, 0x01, 0x10, 0x20, 0xAA, 0x02, 0x30, 0x40, 0xBB}
  };
  dtcs.clear();
  assert(mvci::parseActiveDtcResponses(prefixedResponses, dtcs, 0xFFU) == mvci::STATUS_NOERROR);
  assert(dtcs.size() == 2);
  assert(dtcs[0].ecuAddress == 0x000007E8U);
  assert(dtcs[0].code == 0x011020U);
  assert(dtcs[0].status == 0xAAU);
  assert(dtcs[1].ecuAddress == 0x000007E8U);
  assert(dtcs[1].code == 0x023040U);
  assert(dtcs[1].status == 0xBBU);

  return 0;
}