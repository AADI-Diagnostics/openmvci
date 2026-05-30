cmake_minimum_required(VERSION 3.16)

project(OpenMVCI VERSION 1.0.0 LANGUAGES CXX)

option(MVCI_BUILD_TESTS "Build MVCI smoke tests" ON)
option(MVCI_BUILD_EXAMPLES "Build MVCI examples" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(openmvci
  src/api.cpp
  src/mvcicompat.cpp
  src/driver.cpp
  src/packet.cpp
  src/uds.cpp
  src/platform/usb_vci.cpp
  src/platform/loopback_transport.cpp
)

if(APPLE)
  target_sources(openmvci PRIVATE src/platform/macos_transport.cpp)
elseif(WIN32)
  target_sources(openmvci PRIVATE src/platform/windows_transport.cpp)
else()
  target_sources(openmvci PRIVATE src/platform/linux_transport.cpp)
endif()

target_include_directories(openmvci
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(openmvci PUBLIC cxx_std_17)
target_compile_definitions(openmvci PRIVATE $<$<BOOL:${BUILD_SHARED_LIBS}>:MVCI_BUILDING_DLL=1>)

set_target_properties(openmvci PROPERTIES
  OUTPUT_NAME openmvci
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

if(WIN32)
  set_target_properties(openmvci PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

find_package(Threads REQUIRED)
target_link_libraries(openmvci PRIVATE Threads::Threads)

find_package(LibUSB REQUIRED)
target_link_libraries(openmvci PRIVATE LibUSB::LibUSB)

if(MVCI_BUILD_TESTS)
  enable_testing()
  add_executable(openmvci_smoke tests/smoke.cpp)
  add_executable(openmvci_unit_api tests/unit_api.cpp)
  add_executable(openmvci_unit_uds tests/unit_uds.cpp)
  target_link_libraries(openmvci_smoke PRIVATE openmvci Threads::Threads)
  target_link_libraries(openmvci_unit_api PRIVATE openmvci Threads::Threads)
  target_link_libraries(openmvci_unit_uds PRIVATE openmvci Threads::Threads)
  add_test(NAME openmvci_smoke COMMAND openmvci_smoke)
  add_test(NAME openmvci_unit_api COMMAND openmvci_unit_api)
  add_test(NAME openmvci_unit_uds COMMAND openmvci_unit_uds)
endif()

add_executable(dtc_reader tools/dtc_reader.cpp)
target_link_libraries(dtc_reader PRIVATE openmvci Threads::Threads)

if(MVCI_BUILD_EXAMPLES)
  add_executable(read_dtcs examples/read_dtcs.cpp)
  target_link_libraries(read_dtcs PRIVATE openmvci Threads::Threads)
endif()

set(OPENMVCI_PACKAGE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/openmvci)

configure_package_config_file(
  cmake/openmvciConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/openmvciConfig.cmake
  INSTALL_DESTINATION ${OPENMVCI_PACKAGE_INSTALL_DIR}
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/openmvciConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

configure_file(
  cmake/FindLibUSB.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/FindLibUSB.cmake
  COPYONLY
)

install(TARGETS openmvci dtc_reader
  EXPORT openmvciTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES cmake/FindLibUSB.cmake DESTINATION ${OPENMVCI_PACKAGE_INSTALL_DIR})

install(EXPORT openmvciTargets
  FILE openmvciTargets.cmake
  NAMESPACE OpenMVCI::
  DESTINATION ${OPENMVCI_PACKAGE_INSTALL_DIR}
)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/openmvciConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/openmvciConfigVersion.cmake
  DESTINATION ${OPENMVCI_PACKAGE_INSTALL_DIR}
)

install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/openmvci)

export(EXPORT openmvciTargets FILE ${CMAKE_CURRENT_BINARY_DIR}/openmvciTargets.cmake NAMESPACE OpenMVCI::)