# Library Integration

Use the project either as a subdirectory or as an installed CMake package.

## add_subdirectory

```cmake
add_subdirectory(path/to/mvci32_min)
target_link_libraries(your_target PRIVATE mvci32)
```

## find_package

```cmake
find_package(mvci32 CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE MVCI32::mvci32)
```

The package exports the `mvci32` target, the `dtc_reader` executable target, and the public headers under `include/`.
