# OpenMVCI

OpenMVCI is a cross-platform C++17 open source reimplementation of the Toyota Techstream-facing MVCI/J2534 API.
It includes a libusb backend and a practical command-line DTC reader you can use for day-to-day diagnostics.

Build outputs by platform:

- Windows: `openmvci.dll` (drop-in compatible exports, non-infringing filename)
- macOS: `libMVCI32.dylib`
- Linux: `libMVCI32.so`

## Features

- J2534-style exports, plus MVCI-compatible aliases
- Shared-library focused build (`BUILD_SHARED_LIBS=ON` is recommended)
- libusb backend with known Toyota/Mini-VCI VID/PID matching and keyword fallback discovery
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

- `--device <selector>`: `loopback` or `vid:pid[:serial]`
- `--baud <n>`
- `--timeout <ms>`
- `--interval <ms>`
- `--mask <status-mask>`
- `--verbose`

Examples:

```bash
build/dtc_reader --read --device 0403:6001
build/dtc_reader --clear --device 0403:6001
build/dtc_reader --monitor --interval 1000 --verbose
```

## Integration

### As a subdirectory

```cmake
add_subdirectory(external/mvci32_min)
target_link_libraries(your_target PRIVATE mvci32)
```

### As an installed package

```bash
cmake -S . -B build -DBUILD_SHARED_LIBS=ON
cmake --build build -j4
cmake --install build --prefix /your/prefix
```

```cmake
find_package(mvci32 CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE MVCI32::mvci32)
```

## Drop-In Replacement Notes

- ABI exports are designed to match common MVCI32/J2534 entry points used by Techstream-like consumers.
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