#pragma once

#include <memory>

#include "mvci/platform/transport.hpp"

namespace mvci {

#if !defined(_WIN32)
std::unique_ptr<ITransport> createSerialTransport();
#endif

} // namespace mvci
