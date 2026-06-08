# FindLibUSB.cmake
# Locates libusb-1.0 for the OpenMVCI project.
# Supports:
# - pkg-config (preferred on Linux/macOS)
# - Common system paths (Linux, macOS/Homebrew, /usr/local)
# - vcpkg (with or without toolchain file; hints for manual setups)
# - The project's original macOS hints for compatibility

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_LIBUSB QUIET libusb-1.0)
endif()

find_path(LIBUSB_INCLUDE_DIR
  NAMES libusb.h
  PATH_SUFFIXES libusb-1.0
  HINTS
    ${PC_LIBUSB_INCLUDEDIR}
    ${PC_LIBUSB_INCLUDE_DIRS}
    /opt/homebrew/include
    /usr/local/include
    /usr/include
    /opt/homebrew/opt/libusb/include
    /usr/local/opt/libusb/include
    $ENV{VCPKG_ROOT}/installed/x64-windows/include
    $ENV{VCPKG_ROOT}/installed/x64-windows/include/libusb-1.0
    $ENV{VCPKG_INSTALLED_DIR}/x64-windows/include
)

find_library(LIBUSB_LIBRARY
  NAMES usb-1.0 libusb-1.0 usb libusb
  HINTS
    ${PC_LIBUSB_LIBDIR}
    ${PC_LIBUSB_LIBRARY_DIRS}
    /opt/homebrew/lib
    /usr/local/lib
    /usr/lib
    /usr/lib/x86_64-linux-gnu
    /usr/lib/aarch64-linux-gnu
    /opt/homebrew/opt/libusb/lib
    /usr/local/opt/libusb/lib
    $ENV{VCPKG_ROOT}/installed/x64-windows/lib
    $ENV{VCPKG_INSTALLED_DIR}/x64-windows/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibUSB
  REQUIRED_VARS LIBUSB_INCLUDE_DIR LIBUSB_LIBRARY
)

if(LIBUSB_FOUND AND NOT TARGET LibUSB::LibUSB)
  add_library(LibUSB::LibUSB UNKNOWN IMPORTED)
  set_target_properties(LibUSB::LibUSB PROPERTIES
    IMPORTED_LOCATION "${LIBUSB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBUSB_INCLUDE_DIR}"
  )
endif()
