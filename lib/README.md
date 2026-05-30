# Library Integration

Use the project either as a subdirectory or as an installed CMake package.

## add_subdirectory

```cmake
add_subdirectory(path/to/openmvci)
target_link_libraries(your_target PRIVATE openmvci)
```

## find_package

```cmake
find_package(openmvci CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE OpenMVCI::openmvci)
```

The package exports the `openmvci` target, the `dtc_reader` executable target, and the public headers under `include/`.
