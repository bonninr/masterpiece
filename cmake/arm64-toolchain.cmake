# ARM64 (Raspberry Pi OS 64-bit) cross-compilation toolchain.
#
# This is the Pi target that matters now: Raspberry Pi OS has shipped 64-bit
# by default since 2022, and a Pi 5 is rarely run any other way. The 32-bit
# armhf toolchain beside this one stays for older installs.
#
# Like that one, it targets a Debian multiarch host rather than a sysroot: the
# arm64 libraries sit under /usr/lib/aarch64-linux-gnu on the build machine's
# own root.
#
#   cmake --preset ci-linux-arm64

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_AR aarch64-linux-gnu-ar CACHE FILEPATH "ARM64 ar")
set(CMAKE_RANLIB aarch64-linux-gnu-ranlib CACHE FILEPATH "ARM64 ranlib")

# No -march here. ARMv8-A is the floor for every 64-bit Pi and NEON is part of
# the base architecture rather than an extension, so the generic target covers
# Pi 3 through Pi 5 without pinning the binary to one of them.

# Debian multiarch: this is what sends find_library into
# /usr/lib/aarch64-linux-gnu instead of the host's own /usr/lib.
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

# Host tools (juceaide and friends) must still be the host's own, but headers
# and libraries have to resolve against the ARM tree. There is no sysroot
# here, so ONLY would leave CMake with nowhere to look.
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu /usr)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# LIBDIR rather than PATH: PATH appends to the host's search list and lets a
# host .pc file satisfy an ARM dependency, which then fails at link time.
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
