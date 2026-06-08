# OpenMVCI

OpenMVCI is a cross-platform C++17 open source reimplementation of the Toyota Techstream-facing MVCI/J2534 API.
It includes a libusb backend and a practical command-line DTC reader you can use for day-to-day diagnostics.

Build outputs by platform (drop-in compatible exports):

- Windows: `openmvci.dll`
- macOS: `libopenmvci.dylib`
- Linux: `libopenmvci.so`

## Features

- J2534-style exports, plus MVCI-compatible aliases
- Shared-library focused build (`BUILD_SHARED_LIBS=ON` is recommended)
- libusb backend with known Toyota/Mini-VCI VID/PID matching and keyword fallback discovery
- Mini-VCI bootstrap handshake support for FTDI-class adapters (`0403:6001`, `0403:6010`)
- Resilient packet resynchronization that skips Mini-VCI status chatter before J2534 frame decode
- Serial-first dispatch fallback for Mini-VCI on macOS and Linux when raw USB is unavailable
- CLI tool for reading, clearing, and monitoring DTCs
- CMake integration via both `add_subdirectory()` and `find_package()`
- Smoke and unit test coverage

## Exported API Surface

This library exports the core pass-thru entry points typically expected by Techstream-oriented integrations:

- `PassThruOpen`
- `PassThruClose`
- `PassThruConnect`
- `PassThruDisconnect`
- `PassThruReadMsgs`
- `PassThruWriteMsgs`
- `PassThruStartPeriodicMsg`
- `PassThruStopPeriodicMsg`
- `PassThruStartMsgFilter`
- `PassThruStopMsgFilter`
- `PassThruSetProgrammingVoltage`
- `PassThruIoctl`
- `PassThruReadVersion`
- `PassThruGetLastError`

It also exports MVCI-prefixed compatibility aliases:

- `MVCI_OpenDevice`
- `MVCI_CloseDevice`
- `MVCI_Connect`
- `MVCI_Disconnect`
- `MVCI_ReadMsgs`
- `MVCI_WriteMsgs`
- `MVCI_StartPeriodicMsg`
- `MVCI_StopPeriodicMsg`
- `MVCI_StartMsgFilter`
- `MVCI_StopMsgFilter`
- `MVCI_SetProgrammingVoltage`
- `MVCI_Ioctl`
- `MVCI_ReadVersion`
- `MVCI_GetLastError`

## Build

### macOS (primary target)

```bash
brew install cmake libusb
cmake -S . -B build -DBUILD_SHARED_LIBS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

### Linux

Install CMake, a C++17 toolchain, and `libusb-1.0` development headers first.

```bash
cmake -S . -B build -DBUILD_SHARED_LIBS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

If USB access is denied, add an appropriate udev rule for your adapter, then reconnect the device.

### Windows (x64)

Use MSVC + CMake with a libusb build.
For most adapters, you will need to bind the device to WinUSB/libusb (for example with Zadig).

```powershell
cmake -S . -B build -A x64 -DBUILD_SHARED_LIBS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## DTC Reader CLI

`dtc_reader` supports these primary modes:

- `--read` read active DTCs
- `--clear` clear DTCs
- `--monitor` continuously poll

Additional options include:

- `--device <selector>`: `vid:pid[:serial]`, `serial:/dev/...`, or `/dev/...` (or omit for auto-discovery on supported platforms)
- `--baud <n>`
- `--timeout <ms>`
- `--interval <ms>`
- `--mask <status-mask>`
- `--verbose`
- `--no-vin` (skip VIN retrieval before reading DTCs)

Examples:

```bash
build/dtc_reader --read --device 0403:6001
build/dtc_reader --clear --device 0403:6001
build/dtc_reader --monitor --interval 1000 --verbose
build/dtc_reader --read --device serial:/dev/cu.usbserial-FTXYZ123
```

By default, read and monitor modes also attempt VIN retrieval over UDS `0x22 F1 90` and then fall back to OBD Mode 09 PID 02 when needed, printing the decoded VIN before DTC output.

The `read_dtcs` example now defaults to automatic real-device discovery (same behavior as `dtc_reader`).

On macOS, automatic serial discovery probes both `/dev/tty.usb*` and `/dev/cu.usb*` nodes.

### Mini-VCI runtime toggles

- `MVCI_MINIVCI_BOOTSTRAP=0` disables the Mini-VCI startup handshake (enabled by default).
- `MVCI_MINIVCI_BOOTSTRAP_STRICT=1` rejects adapters that fail bootstrap (disabled by default).
- `MVCI_MINIVCI_STAGE1_STRICT=1` requires explicit stage-1 ACK (`01 60`), otherwise stage-1 timeout is tolerated.
- `MVCI_MINIVCI_POST_BOOTSTRAP=0` disables the PCAP-derived post-bootstrap unlock script (enabled by default).
- `MVCI_MINIVCI_POST_BOOTSTRAP_STRICT=1` treats post-bootstrap script mismatch as bootstrap failure.
- `MVCI_MINIVCI_KEEPALIVE_BRIDGE=0` disables the pre-ICVM low-level keepalive probe.
- `MVCI_MINIVCI_SESSION_TICKLE=0` disables periodic low-level session tickles while waiting for ICVM replies.
- `MVCI_OBD_CAN_ID=0x7E0` overrides the ISO-15765 request CAN ID prepended to UDS payloads (default `0x7DF` functional broadcast). Use a physical request ID such as `0x7E0` for ECUs that ignore broadcast (common on some GM/Ford platforms).

If serial opens fail intermittently with `No such file or directory` or `Operation timed out` on `/dev/cu.usbserial-*`, power-cycle or replug the adapter and retry. Some clone devices temporarily drop their VCP node during line-state transitions.

`mvci_probe` now supports focused pcap windows and custom packet probes:
- `--baud <rate>` limits probing to a single baud.
- `--pcap-start-offset <n>` and `--pcap-max-steps <n>` select replay window after startup.
- `--pcap-show-rx` prints per-step reply hex.
- `--no-decode-icvm` disables probe-side decoding attempts for obfuscated ICVM-like replies.
- `--send-hex <hex>` sends custom frames after replay (repeatable).
- `--send-icvm-protocol <n>` overrides bytes 8..11 of the matching custom ICVM request with a little-endian MVCI protocol value.
- `--send-read-ms <ms>` controls custom probe read timeout.

For repeatable mapping runs, use:
`tools/run_minivci_mapping.sh [serial-node] [pcap-path] [build-dir]`
- `MAPPING_ICVM_ATTEMPTS=<n>` retries the ICVM custom probe phase (default `5`) to catch intermittent response windows.
- `MVCI_VERBOSE_USB=1` enables verbose USB transport logs.
- `MVCI_VERBOSE_SERIAL=1` enables verbose serial transport logs.
- `MVCI_SERIAL_BAUD=<rate>` overrides the initial serial baud rate (default `500000`).
- `MVCI_SERIAL_OPEN_RETRY_MS=<ms>` controls how long serial open waits for adapter re-enumeration (default `3000`).
- `MVCI_SERIAL_ASSERT_CTRL=0` disables asserting `DTR/RTS` on open (enabled by default).
- `MVCI_SERIAL_CTRL_MODE=<assert|none|pulse>` explicitly controls `DTR/RTS` policy on open.
- `MVCI_SERIAL_CTRL_AUTO=0` disables automatic fallback across control-line modes during bootstrap.
- `MVCI_SERIAL_RTSCTS=1` enables hardware flow control (`RTS/CTS`) for adapters that require it.

Serial transport now also attempts automatic in-session recovery when macOS reports transient device loss errors (for example `Device not configured`) by reopening and reconfiguring the active serial node.

After successful serial recovery, openmvci now re-runs Mini bootstrap before allowing ICVM traffic.

If an ICVM write arrives while Mini transport is not ready, openmvci now performs a one-shot bootstrap retry (including baud/control-mode fallback) before rejecting the write with `not initialized`.

With `MVCI_VERBOSE_SERIAL=1`, serial transport logs now include hex dumps for TX/RX chunks.

When bootstrap is enabled, the serial transport will probe additional common Mini-VCI baud rates (`230400`, `115200`, `38400`) if bootstrap does not respond at the initial configured rate.

## Integration

### As a subdirectory

```cmake
add_subdirectory(external/openmvci)
target_link_libraries(your_target PRIVATE openmvci)
```

### As an installed package

```bash
cmake -S . -B build -DBUILD_SHARED_LIBS=ON
cmake --build build -j4
cmake --install build --prefix /your/prefix
```

```cmake
find_package(openmvci CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE OpenMVCI::openmvci)
```

## Drop-In Replacement Notes

- ABI exports are designed to match common Techstream-style J2534 entry points used by downstream consumers.
- If you extend the library, keep exported names and calling conventions unchanged to preserve compatibility.
- On Windows, deploy `openmvci.dll` and map or rename it according to your integration strategy.
- For unsupported vendor-specific behavior, extend the backend in `src/platform/usb_vci.cpp` and `src/driver.cpp`.

## Repository Layout

- `include/` public API headers
- `src/` core implementation
- `src/platform/` platform and USB transport backends
- `tools/` CLI tools
- `examples/` consumer examples
- `tests/` smoke and unit tests
- `cmake/` package/config helper modules

## License

See `LICENSE` for license terms.