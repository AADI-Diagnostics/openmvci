find_path(LIBUSB_INCLUDE_DIR
  NAMES libusb.h
  PATH_SUFFIXES libusb-1.0
  HINTS
    /opt/homebrew/include
    /usr/local/include
    /opt/homebrew/opt/libusb/include
    /usr/local/opt/libusb/include
)

find_library(LIBUSB_LIBRARY
  NAMES usb-1.0 libusb-1.0 usb
  HINTS
    /opt/homebrew/lib
    /usr/local/lib
    /opt/homebrew/opt/libusb/lib
    /usr/local/opt/libusb/lib
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
