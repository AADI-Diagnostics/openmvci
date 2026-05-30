#include "mvci/platform/transport.hpp"
#include "mvci/platform/usb_vci.hpp"

namespace mvci {

std::unique_ptr<ITransport> createPlatformTransport() {
  return createUsbVciTransport();
}

} // namespace mvci