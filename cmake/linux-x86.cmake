# Native 32-bit Linux build of the Black Mesa VR Linux shim (linux/).
# The native Linux Black Mesa is a 32-bit x86 program, so the preload library
# must be 32-bit too. Needs gcc with multilib (`gcc -m32` must work).
#   cmake -S . -B build_linux_native -DCMAKE_TOOLCHAIN_FILE=cmake/linux-x86.cmake
# (scripts/build-linux.sh wraps this.) Not for the Windows d3d9.dll build.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)   # the root project enables C and CXX; both must be 32-bit
set(CMAKE_C_FLAGS_INIT "-m32")
set(CMAKE_CXX_FLAGS_INIT "-m32")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32")
