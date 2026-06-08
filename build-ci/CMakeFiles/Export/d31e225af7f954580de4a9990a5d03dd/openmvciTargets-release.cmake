#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "OpenMVCI::openmvci" for configuration "Release"
set_property(TARGET OpenMVCI::openmvci APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(OpenMVCI::openmvci PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libopenmvci.dylib"
  IMPORTED_SONAME_RELEASE "@rpath/libopenmvci.dylib"
  )

list(APPEND _cmake_import_check_targets OpenMVCI::openmvci )
list(APPEND _cmake_import_check_files_for_OpenMVCI::openmvci "${_IMPORT_PREFIX}/lib/libopenmvci.dylib" )

# Import target "OpenMVCI::dtc_reader" for configuration "Release"
set_property(TARGET OpenMVCI::dtc_reader APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(OpenMVCI::dtc_reader PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/dtc_reader"
  )

list(APPEND _cmake_import_check_targets OpenMVCI::dtc_reader )
list(APPEND _cmake_import_check_files_for_OpenMVCI::dtc_reader "${_IMPORT_PREFIX}/bin/dtc_reader" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
