# CMake generated Testfile for 
# Source directory: /Users/cmarshall/Desktop/openmvci
# Build directory: /Users/cmarshall/Desktop/openmvci/build-ci
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(openmvci_smoke "/Users/cmarshall/Desktop/openmvci/build-ci/openmvci_smoke")
set_tests_properties(openmvci_smoke PROPERTIES  _BACKTRACE_TRIPLES "/Users/cmarshall/Desktop/openmvci/CMakeLists.txt;88;add_test;/Users/cmarshall/Desktop/openmvci/CMakeLists.txt;0;")
add_test(openmvci_unit_api "/Users/cmarshall/Desktop/openmvci/build-ci/openmvci_unit_api")
set_tests_properties(openmvci_unit_api PROPERTIES  _BACKTRACE_TRIPLES "/Users/cmarshall/Desktop/openmvci/CMakeLists.txt;89;add_test;/Users/cmarshall/Desktop/openmvci/CMakeLists.txt;0;")
add_test(openmvci_unit_uds "/Users/cmarshall/Desktop/openmvci/build-ci/openmvci_unit_uds")
set_tests_properties(openmvci_unit_uds PROPERTIES  _BACKTRACE_TRIPLES "/Users/cmarshall/Desktop/openmvci/CMakeLists.txt;90;add_test;/Users/cmarshall/Desktop/openmvci/CMakeLists.txt;0;")
add_test(openmvci_unit_frame_resync "/Users/cmarshall/Desktop/openmvci/build-ci/openmvci_unit_frame_resync")
set_tests_properties(openmvci_unit_frame_resync PROPERTIES  _BACKTRACE_TRIPLES "/Users/cmarshall/Desktop/openmvci/CMakeLists.txt;91;add_test;/Users/cmarshall/Desktop/openmvci/CMakeLists.txt;0;")
subdirs("_deps/doctest-build")
