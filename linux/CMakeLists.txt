# Black Mesa VR - native Linux (ToGL) port, milestone 1 (observe-only shim).
# Added by the root CMakeLists.txt on non-Windows hosts; also configurable alone:
#   cmake -S linux -B build_linux_native -DCMAKE_TOOLCHAIN_FILE=cmake/linux-x86.cmake
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(BMVR_Linux LANGUAGES C)
endif()

if(NOT UNIX OR APPLE)
  message(FATAL_ERROR "linux/ is for native Linux only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "Native Black Mesa is 32-bit. Configure with:\n"
    "  cmake -S . -B build_linux_native -DCMAKE_TOOLCHAIN_FILE=cmake/linux-x86.cmake")
endif()

add_library(bmvr_preload32 SHARED bmvr_preload.c)
set_target_properties(bmvr_preload32 PROPERTIES
  PREFIX ""
  OUTPUT_NAME "bmvr_preload32"
  C_STANDARD 11
  C_VISIBILITY_PRESET hidden)   # only the ToGL interposers (visibility attribute) are exported
# -fno-stack-protector / -U_FORTIFY_SOURCE: avoid __stack_chk_fail / __*_chk symbol versions that
# an older glibc (Steam Linux Runtime) may lack. See the portability note in bmvr_preload.c.
target_compile_options(bmvr_preload32 PRIVATE -Wall -Wextra -fno-strict-aliasing
                       -fno-stack-protector -U_FORTIFY_SOURCE)
target_link_libraries(bmvr_preload32 PRIVATE ${CMAKE_DL_LIBS} m)

# Optional: cmake --install build_linux_native --prefix "<Black Mesa folder>"
install(TARGETS bmvr_preload32 LIBRARY DESTINATION .)
