#----------------------------------------------------------------
# Generated CMake target import file.
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "MVCI32::mvci32" for configuration ""
set_property(TARGET MVCI32::mvci32 APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(MVCI32::mvci32 PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_NOCONFIG "CXX"
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libMVCI32.a"
  )

list(APPEND _cmake_import_check_targets MVCI32::mvci32 )
list(APPEND _cmake_import_check_files_for_MVCI32::mvci32 "${_IMPORT_PREFIX}/lib/libMVCI32.a" )

# Import target "MVCI32::dtc_reader" for configuration ""
set_property(TARGET MVCI32::dtc_reader APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(MVCI32::dtc_reader PROPERTIES
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/bin/dtc_reader"
  )

list(APPEND _cmake_import_check_targets MVCI32::dtc_reader )
list(APPEND _cmake_import_check_files_for_MVCI32::dtc_reader "${_IMPORT_PREFIX}/bin/dtc_reader" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
