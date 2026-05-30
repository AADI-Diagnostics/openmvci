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
  assert(mvci::formatDtc(dtcs[0].code) == "0x011020");

  return 0;
}