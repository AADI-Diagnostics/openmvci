cmake_minimum_required(VERSION 3.16)
project(MVCI32ConsumerExample LANGUAGES CXX)

find_package(mvci32 CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE MVCI32::mvci32)
