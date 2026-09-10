# ARM (Raspberry Pi, 32-bit armhf) cross-compilation toolchain.
# Targets a Debian multiarch host: the ARM libraries sit under
# /usr/lib/arm-linux-gnueabihf on the build machine's own root, so there is
# no separate sysroot to point at.
#
#   cmake --preset ci-linux-arm

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_AR arm-linux-gnueabihf-ar CACHE FILEPATH "ARM ar")
set(CMAKE_RANLIB arm-linux-gnueabihf-ranlib CACHE FILEPATH "ARM ranlib")

# -mfloat-abi=hard needs an FPU, and bare armv7-a declares none — gcc rejects
# the combination outright ("selected architecture lacks an FPU"). neon-vfpv4
# is the Cortex-A7 unit, so one binary covers Pi 2 through Pi 5 in 32-bit
# mode; it excludes only the ARMv6 Pi 1 / Zero, which cannot carry this
# engine anyway.
set(MP_ARM_FLAGS "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mthumb")
set(CMAKE_C_FLAGS_INIT "${MP_ARM_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${MP_ARM_FLAGS}")

# Debian multiarch: this is what sends find_library into
# /usr/lib/arm-linux-gnueabihf instead of the host's own /usr/lib.
set(CMAKE_LIBRARY_ARCHITECTURE arm-linux-gnueabihf)

# Host tools (juceaide and friends) must still be the host's own, but headers
# and libraries have to resolve against the ARM multiarch tree. There is no
# sysroot here, so ONLY would leave CMake with nowhere to look.
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf /usr)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# LIBDIR rather than PATH: PATH appends to the host's search list and lets a
# host .pc file satisfy an ARM dependency, which then fails at link time.
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
