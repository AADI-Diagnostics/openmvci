#pragma once

#include <memory>

#include "mvci/platform/transport.hpp"

namespace mvci {

std::unique_ptr<ITransport> createSerialTransport();

} // namespace mvci
