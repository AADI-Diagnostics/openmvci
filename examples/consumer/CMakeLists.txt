cmake_minimum_required(VERSION 3.16)
project(OpenMVCIConsumerExample LANGUAGES CXX)

find_package(openmvci CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE OpenMVCI::openmvci)
