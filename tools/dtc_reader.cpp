#include <cstdint>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>
#include <string>
#include <vector>

#include "mvci/api.hpp"
#include "mvci/uds.hpp"

namespace {

struct Options {
  std::string deviceName;
  std::uint32_t baudRate{500000};
  std::uint32_t timeoutMs{1500};
  std::uint32_t intervalMs{2500};
  std::uint8_t statusMask{0xFFU};
  bool read{true};
  bool clear{false};
  bool monitor{false};
  bool verbose{false};
  bool help{false};
  bool fetchVin{true};
};

bool parseUint(const std::string& text, std::uint32_t& value) {
  try {
    value = static_cast<std::uint32_t>(std::stoul(text, nullptr, 0));
    return true;
  } catch (...) {
    return false;
  }
}

void printUsage() {
  std::cout << "Usage: dtc_reader [--device NAME] [--baud N] [--timeout MS] [--mask N] [--interval MS] [--read] [--clear] [--monitor] [--verbose] [--no-vin]\n";
}

void printFrame(const char* label, const std::vector<std::uint8_t>& frame) {
  std::cout << label << mvci::formatFrame(frame) << '\n';
}

bool parseArgs(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--help" || arg == "-h") {
      options.help = true;
      return true;
    }
    if (arg == "--clear") {
      options.clear = true;
      options.read = false;
      continue;
    }
    if (arg == "--read") {
      options.read = true;
      options.clear = false;
      options.monitor = false;
      continue;
    }
    if (arg == "--monitor") {
      options.monitor = true;
      options.read = true;
      options.clear = false;
      continue;
    }
    if (arg == "--verbose" || arg == "-v") {
      options.verbose = true;
      continue;
    }
    if (arg == "--no-vin") {
      options.fetchVin = false;
      continue;
    }
    if (index + 1 >= argc) {
      return false;
    }

    const std::string value = argv[++index];
    if (arg == "--device") {
      options.deviceName = value;
    } else if (arg == "--baud") {
      if (!parseUint(value, options.baudRate)) {
        return false;
      }
    } else if (arg == "--timeout") {
      if (!parseUint(value, options.timeoutMs)) {
        return false;
      }
    } else if (arg == "--interval") {
      if (!parseUint(value, options.intervalMs)) {
        return false;
      }
    } else if (arg == "--mask") {
      std::uint32_t parsed = 0;
      if (!parseUint(value, parsed) || parsed > 0xFFU) {
        return false;
      }
      options.statusMask = static_cast<std::uint8_t>(parsed);
    } else {
      return false;
    }
  }

  return true;
}

void printDtcs(const std::vector<mvci::DtcRecord>& dtcs) {
  if (dtcs.empty()) {
    std::cout << "No active DTCs reported.\n";
    return;
  }

  for (const auto& dtc : dtcs) {
    if (dtc.ecuAddress != 0U) {
      std::cout << "ECU 0x" << std::hex << std::uppercase << std::setw(3) << std::setfill('0')
                << dtc.ecuAddress << std::dec << std::setfill(' ') << " ";
    }
    std::cout << mvci::formatDtc(dtc.code)
              << " status=0x"
              << std::hex
              << static_cast<unsigned>(dtc.status)
              << std::dec
              << '\n';
  }
}

mvci::Status runReadCycle(mvci::ChannelHandle channelId,
                          const Options& options,
                          std::vector<mvci::DtcRecord>& dtcs) {
  std::vector<std::vector<std::uint8_t>> responses;
  const auto request = mvci::buildReadDtcRequest(options.statusMask);

  if (options.verbose) {
    printFrame("TX: ", request);
  }

  const auto status = mvci::sendUdsRequest(channelId, request, responses, options.timeoutMs);
  if (status != mvci::STATUS_NOERROR) {
    return status;
  }

  if (options.verbose) {
    for (const auto& response : responses) {
      printFrame("RX: ", response);
    }
  }

  return mvci::parseActiveDtcResponses(responses, dtcs, options.statusMask);
}

mvci::Status runVinReadCycle(mvci::ChannelHandle channelId,
                             const Options& options,
                             std::string& vin) {
  std::vector<std::vector<std::uint8_t>> responses;
  const auto request = mvci::buildReadVinRequest();

  if (options.verbose) {
    printFrame("TX VIN: ", request);
  }

  const auto status = mvci::sendUdsRequest(channelId, request, responses, options.timeoutMs);
  if (status != mvci::STATUS_NOERROR) {
    return status;
  }

  if (options.verbose) {
    for (const auto& response : responses) {
      printFrame("RX VIN: ", response);
    }
  }

  return mvci::parseVinResponses(responses, vin);
}

mvci::Status runClearCycle(mvci::ChannelHandle channelId, const Options& options) {
  std::vector<std::vector<std::uint8_t>> responses;
  const auto request = mvci::buildClearDtcRequest();

  if (options.verbose) {
    printFrame("TX: ", request);
  }

  const auto status = mvci::sendUdsRequest(channelId, request, responses, options.timeoutMs);
  if (status != mvci::STATUS_NOERROR) {
    return status;
  }

  if (options.verbose) {
    for (const auto& response : responses) {
      printFrame("RX: ", response);
    }
  }

  for (const auto& response : responses) {
    const auto uds = mvci::stripCanIdPrefix(response);
    if (!uds.empty() && uds.front() == 0x54U) {
      return mvci::STATUS_NOERROR;
    }
    if (!uds.empty() && uds.front() == 0x7FU) {
      return mvci::ERR_FAILED;
    }
  }

  return mvci::ERR_FAILED;
}

} // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parseArgs(argc, argv, options)) {
    printUsage();
    return 1;
  }

  if (options.help) {
    printUsage();
    return 0;
  }

  if (!options.read && !options.clear) {
    options.read = true;
  }

  mvci::DeviceHandle deviceId = 0;
  mvci::ChannelHandle channelId = 0;

  auto status = PassThruOpen(options.deviceName.empty() ? nullptr : options.deviceName.c_str(), &deviceId);
  if (status != mvci::STATUS_NOERROR) {
    std::cerr << "PassThruOpen failed: " << mvci::statusToString(status) << '\n';
    return 1;
  }

  status = PassThruConnect(deviceId, mvci::PROTOCOL_ISO15765, 0, options.baudRate, &channelId);
  if (status != mvci::STATUS_NOERROR) {
    std::cerr << "PassThruConnect failed: " << mvci::statusToString(status) << '\n';
    PassThruClose(deviceId);
    return 1;
  }

  const auto cleanup = [&]() {
    PassThruDisconnect(channelId);
    PassThruClose(deviceId);
  };

  if (options.clear) {
    status = runClearCycle(channelId, options);
    if (status != mvci::STATUS_NOERROR) {
      std::cerr << "Clear DTC request failed: " << mvci::statusToString(status) << '\n';
      if (status == mvci::ERR_TIMEOUT) {
        std::cerr << "Hint: try MVCI_OBD_CAN_ID=0x7E0 (physical ECM address) and/or ignition ON.\n";
      }
      cleanup();
      return 1;
    }

    std::cout << "DTC clear request accepted.\n";
    cleanup();
    return 0;
  }

  do {
    if (options.fetchVin) {
      std::string vin;
      const auto vinStatus = runVinReadCycle(channelId, options, vin);
      if (vinStatus == mvci::STATUS_NOERROR) {
        std::cout << "VIN: " << vin << '\n';
      } else {
        std::cout << "VIN unavailable: " << mvci::statusToString(vinStatus) << '\n';
        if (vinStatus == mvci::ERR_TIMEOUT) {
          std::cout << "Hint: for many GM vehicles (e.g. 2013 Cruze) set MVCI_OBD_CAN_ID=0x7E0 before running.\n";
          std::cout << "      Also ensure ignition is ON (RUN position) and the adapter is connected to the OBD port.\n";
        }
      }
    }

    std::vector<mvci::DtcRecord> dtcs;
    status = runReadCycle(channelId, options, dtcs);
    if (status != mvci::STATUS_NOERROR) {
      std::cerr << "Read DTC request failed: " << mvci::statusToString(status) << '\n';
      if (status == mvci::ERR_TIMEOUT) {
        std::cerr << "Hint: try MVCI_OBD_CAN_ID=0x7E0 (physical ECM address) and/or ignition ON.\n";
      }
      cleanup();
      return 1;
    }

    printDtcs(dtcs);
    if (!options.monitor) {
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(options.intervalMs));
  } while (options.monitor);

  cleanup();
  return 0;
}