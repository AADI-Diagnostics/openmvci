#pragma once

#include <cstdint>

namespace mvci {

using Status = std::int32_t;
using DeviceHandle = std::uint32_t;
using ChannelHandle = std::uint32_t;

constexpr Status STATUS_NOERROR = 0;
constexpr Status ERR_FAILED = -1;
constexpr Status ERR_NOT_INITIALIZED = -2;
constexpr Status ERR_INVALID_DEVICE_ID = -3;
constexpr Status ERR_INVALID_CHANNEL_ID = -4;
constexpr Status ERR_INVALID_MSG = -5;
constexpr Status ERR_TIMEOUT = -6;
constexpr Status ERR_BUFFER_EMPTY = -7;
constexpr Status ERR_NOT_SUPPORTED = -8;

constexpr std::uint32_t PROTOCOL_CAN = 0x0005;
constexpr std::uint32_t PROTOCOL_ISO15765 = 0x0006;

constexpr std::uint32_t FILTER_PASS = 0x0001;
constexpr std::uint32_t FILTER_BLOCK = 0x0002;
constexpr std::uint32_t FILTER_FLOW_CONTROL = 0x0003;

// J2534-1 TxFlags / RxStatus bits.
constexpr std::uint32_t ISO15765_FRAME_PAD = 0x00000040;
constexpr std::uint32_t CAN_29BIT_ID = 0x00000100;

constexpr std::uint32_t IOCTL_SET_CONFIG = 0x0001;
constexpr std::uint32_t IOCTL_GET_CONFIG = 0x0002;
constexpr std::uint32_t IOCTL_CLEAR_RX_BUFFER = 0x0003;
constexpr std::uint32_t IOCTL_CLEAR_TX_BUFFER = 0x0004;
constexpr std::uint32_t IOCTL_READ_BATT_VOLTAGE = 0x0005;

constexpr std::uint32_t CONFIG_BAUDRATE = 0x0100;

struct PassThruMsg {
  std::uint32_t protocolId{0};
  std::uint32_t rxStatus{0};
  std::uint32_t txFlags{0};
  std::uint32_t timestamp{0};
  std::uint32_t dataSize{0};
  std::uint32_t extraDataIndex{0};
  std::uint8_t data[4128]{};
};

struct ConfigValue {
  std::uint32_t parameter{0};
  std::uint32_t value{0};
};

struct ConfigList {
  std::uint32_t numOfParams{0};
  ConfigValue* configPtr{nullptr};
};

struct ByteArray {
  std::uint32_t numOfBytes{0};
  std::uint8_t* bytePtr{nullptr};
};

} // namespace mvci