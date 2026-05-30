cmake_minimum_required(VERSION 3.16)

project(MVCI32Mini VERSION 1.0.0 LANGUAGES CXX)

option(MVCI_BUILD_TESTS "Build MVCI smoke tests" ON)
option(MVCI_BUILD_EXAMPLES "Build MVCI examples" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(mvci32
  src/api.cpp
  src/mvcicompat.cpp
  src/driver.cpp
  src/packet.cpp
  src/uds.cpp
  src/platform/usb_vci.cpp
  src/platform/loopback_transport.cpp
)

if(APPLE)
  target_sources(mvci32 PRIVATE src/platform/macos_transport.cpp)
elseif(WIN32)
  target_sources(mvci32 PRIVATE src/platform/windows_transport.cpp)
else()
  target_sources(mvci32 PRIVATE src/platform/linux_transport.cpp)
endif()

target_include_directories(mvci32
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(mvci32 PUBLIC cxx_std_17)
target_compile_definitions(mvci32 PRIVATE $<$<BOOL:${BUILD_SHARED_LIBS}>:MVCI_BUILDING_DLL=1>)

set_target_properties(mvci32 PROPERTIES
  OUTPUT_NAME MVCI32
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

if(WIN32)
  set_target_properties(mvci32 PROPERTIES OUTPUT_NAME openmvci)
  set_target_properties(mvci32 PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

find_package(Threads REQUIRED)
target_link_libraries(mvci32 PRIVATE Threads::Threads)

find_package(LibUSB REQUIRED)
target_link_libraries(mvci32 PRIVATE LibUSB::LibUSB)

if(MVCI_BUILD_TESTS)
  enable_testing()
  add_executable(mvci32_smoke tests/smoke.cpp)
  add_executable(mvci32_unit_api tests/unit_api.cpp)
  add_executable(mvci32_unit_uds tests/unit_uds.cpp)
  target_link_libraries(mvci32_smoke PRIVATE mvci32 Threads::Threads)
  target_link_libraries(mvci32_unit_api PRIVATE mvci32 Threads::Threads)
  target_link_libraries(mvci32_unit_uds PRIVATE mvci32 Threads::Threads)
  add_test(NAME mvci32_smoke COMMAND mvci32_smoke)
  add_test(NAME mvci32_unit_api COMMAND mvci32_unit_api)
  add_test(NAME mvci32_unit_uds COMMAND mvci32_unit_uds)
endif()

add_executable(dtc_reader tools/dtc_reader.cpp)
target_link_libraries(dtc_reader PRIVATE mvci32 Threads::Threads)

if(MVCI_BUILD_EXAMPLES)
  add_executable(read_dtcs examples/read_dtcs.cpp)
  target_link_libraries(read_dtcs PRIVATE mvci32 Threads::Threads)
endif()

set(MVCI32_PACKAGE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/mvci32)

configure_package_config_file(
  cmake/mvci32Config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/mvci32Config.cmake
  INSTALL_DESTINATION ${MVCI32_PACKAGE_INSTALL_DIR}
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/mvci32ConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

configure_file(
  cmake/FindLibUSB.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/FindLibUSB.cmake
  COPYONLY
)

install(TARGETS mvci32 dtc_reader
  EXPORT mvci32Targets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES cmake/FindLibUSB.cmake DESTINATION ${MVCI32_PACKAGE_INSTALL_DIR})

install(EXPORT mvci32Targets
  FILE mvci32Targets.cmake
  NAMESPACE MVCI32::
  DESTINATION ${MVCI32_PACKAGE_INSTALL_DIR}
)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/mvci32Config.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/mvci32ConfigVersion.cmake
  DESTINATION ${MVCI32_PACKAGE_INSTALL_DIR}
)

install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/mvci32)

export(EXPORT mvci32Targets FILE ${CMAKE_CURRENT_BINARY_DIR}/mvci32Targets.cmake NAMESPACE MVCI32::)