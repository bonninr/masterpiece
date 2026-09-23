cmake_minimum_required(VERSION 3.22)

project(Masterpiece VERSION 0.5.4 LANGUAGES C CXX)

# A release build linked to the DLL C runtime needs the Visual C++
# Redistributable installed on the machine it runs on; a plain download of
# the zip with nothing else on the system fails to launch at all, with an
# error naming VCRUNTIME140.dll or MSVCP140.dll. Statically linking it makes
# the standalone app and the plugin self-contained -- a few hundred KB larger,
# nothing else installed required. CMP0091 is NEW by default here because
# cmake_minimum_required above asks for 3.22.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MP_FETCH_JUCE "Fetch JUCE via FetchContent (CLI-only, no Projucer)" ON)
option(MP_BUILD_STANDALONE "Build Masterpiece standalone app" ON)
option(MP_BUILD_PLUGIN "Build Masterpiece plugin (VST3/AU/LV2/Standalone wrapper)" ON)
option(MP_BUILD_VALIDATOR "Build mp-validate-odf CLI" ON)
option(MP_BUILD_TESTS "Build + register CTest unit tests" ON)
option(MP_ENABLE_DSP "Compile full DSP (wind/trem/enclosure/voicing/IR). Runtime-togglable." ON)
option(MP_ENABLE_STREAMING "Compile disk-streaming sampler (for low-memory machines)" ON)
option(MP_BUILD_ASAN "Enable ASan+UBSan (GNU/Clang: Linux/macOS CI jobs)" OFF)
option(MP_BUILD_COVERAGE "Enable gcov coverage instrumentation (GNU/Clang)" OFF)

if(MP_BUILD_ASAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
if(MP_BUILD_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(--coverage)
  add_link_options(--coverage)
endif()

# ---- macOS SIMD fix (JUCE DSP template compatibility with Apple Clang) ----
# JUCE 9.0.1 has template instantiation issues on macOS Clang.
# Add -fno-strict-aliasing to work around SIMD register width detection.
if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
  add_compile_options(-fno-strict-aliasing)
endif()

# ---- Performance compile flags (ADR-012) ----
option(MP_ENABLE_LTO "Enable IPO/LTO in Release builds (demanding DSP/mixing code)" ON)
option(MP_NATIVE_ARCH "Optimize for host CPU (-march=native, MSVC /arch:AVX2). OFF for portable/CI builds." OFF)

if(MP_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _mp_ipo_ok OUTPUT _mp_ipo_msg)
  if(_mp_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  else()
    message(STATUS "Masterpiece: IPO/LTO not supported (${_mp_ipo_msg})")
  endif()
endif()
if(MP_NATIVE_ARCH)
  if(MSVC)
    add_compile_options(/arch:AVX2)
  else()
    add_compile_options(-march=native)
  endif()
endif()

set(MP_JUCE_TAG "9.0.1" CACHE STRING "JUCE git tag for FetchContent")
set(MP_JUCE_REPO_URL "https://github.com/juce-framework/JUCE.git" CACHE STRING "JUCE repo URL")
set(MP_JUCE_SOURCE_DIR "" CACHE PATH "Use local JUCE checkout instead of fetching (empty = fetch)")

if(MP_FETCH_JUCE)
  include(FetchContent)
  if(MP_JUCE_SOURCE_DIR AND NOT MP_JUCE_SOURCE_DIR STREQUAL "")
    add_subdirectory(${MP_JUCE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/_juce)
  else()
    FetchContent_Declare(
      juce
      GIT_REPOSITORY ${MP_JUCE_REPO_URL}
      GIT_TAG ${MP_JUCE_TAG}
      GIT_SHALLOW TRUE
      GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(juce)
  endif()
else()
  find_package(JUCE CONFIG REQUIRED)
endif()

# ---- pugixml (ODF XML parsing in mp_core, ADR-014) ----
option(MP_FETCH_PUGIXML "Fetch pugixml via FetchContent" ON)
set(MP_PUGIXML_TAG "v1.14" CACHE STRING "pugixml git tag for FetchContent")
if(MP_FETCH_PUGIXML)
  include(FetchContent)
  FetchContent_Declare(
    pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG ${MP_PUGIXML_TAG}
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
  )
  FetchContent_MakeAvailable(pugixml)
else()
  find_package(pugixml CONFIG REQUIRED)
endif()

# JUCE feature flags. An organ player has no use for an embedded web browser,
# and pulling one in would make GTK + WebKit a hard build dependency on Linux
# for no gain. Curl is off for the same reason: nothing here talks HTTP.
# These are read by the JUCE module sources, which are compiled into each
# consuming target, so they must be set before those targets are declared.
if(TARGET juce::juce_core)
  add_compile_definitions(
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    # There is no VST2 build of Masterpiece, so the VST3 must not claim to
    # replace one: that switch changes parameter-automation IDs, and JUCE
    # raises a hard #error when it is left on without a VST2 target.
    JUCE_VST3_CAN_REPLACE_VST2=0
  )
  # ASIO, on Windows only. It is the driver model a player with a real audio
  # interface will look for, and without it the lowest latency on offer is
  # WASAPI exclusive. GrandOrgue reaches for WDM-KS instead -- its users
  # measure 10 ms at 64-sample buffers against WASAPI's 30 -- but JUCE has no
  # WDM-KS backend, so that answer is not available here and this is.
  #
  # The old blocker was the licence: the Steinberg SDK was proprietary and
  # needed a signed agreement, which is why GrandOrgue's sources can compile
  # with ASIO and its official builds ship without it. Steinberg relicensed in
  # October 2025, and the SDK JUCE bundles is now offered under "the Steinberg
  # ASIO License, or alternatively ... the General Public License (GPL)
  # Version 3", the choice being ours. Masterpiece is GPL-3.0-only, so we take
  # the GPL arm; nothing is signed and nothing extra is downloaded. The
  # headers travel with JUCE, in modules/juce_audio_devices/native/asio/.
  #
  # Windows-only because that is where the driver model exists. The flag is
  # harmless elsewhere -- JUCE guards the code with JUCE_WINDOWS -- but a
  # definition that only means something on one platform is clearer set on
  # that platform.
  if(WIN32)
    add_compile_definitions(JUCE_ASIO=1)
  endif()
  # JACK on Linux, which on a current distribution usually means PipeWire.
  # JUCE compiles ALSA only by default, so the audio panel had exactly one
  # device type and therefore no way to choose anything: Linux audio has moved
  # from ALSA to PulseAudio to PipeWire inside this project's lifetime, and a
  # player on a machine where one route is silent needs another to try.
  # GrandOrgue has offered this choice for years -- its device names carry the
  # API that reaches them -- and this is the same idea with JUCE's parts.
  #
  # Nothing is LINKED: JUCE dlopens libjack.so.0 when it scans, so a machine
  # without JACK simply does not list the type and the shipped .deb gains no
  # dependency. The headers are another matter -- juce_audio_devices.cpp
  # includes <jack/jack.h> to compile the backend at all -- so every Linux
  # builder needs libjack-jackd2-dev present, which the CI dependency lists
  # and cmake/ci-arm-multiarch.sh now install. A development box that already
  # had them will build this without noticing, which is exactly how it got
  # committed claiming otherwise.
  #
  # The device-type dropdown in AudioDeviceSelectorComponent appears by itself
  # once a second type exists, so this needs no UI work.
  if(UNIX AND NOT APPLE)
    add_compile_definitions(
      JUCE_JACK=1
      # What a patchbay shows. The default is "JUCEJack", which tells an
      # organist nothing about which program is asking for their outputs.
      JUCE_JACK_CLIENT_NAME="Masterpiece"
    )
  endif()
  # macOS: prefer Apple's vDSP framework for audio processing to avoid SIMD
  # template instantiation issues with JUCE 9.0.1 on Apple Clang.
  if(APPLE)
    add_compile_definitions(JUCE_USE_VDSP_FRAMEWORK=1)
  endif()
endif()

add_subdirectory(src/mp_core)
if(MP_ENABLE_STREAMING)
  add_subdirectory(src/mp_sampler)
endif()
if(MP_ENABLE_DSP)
  # mp_dsp links juce_dsp unconditionally (ADR-012: JUCE supplies the filters),
  # so this combination cannot work. Say so here rather than at link time.
  if(NOT TARGET juce::juce_dsp)
    message(FATAL_ERROR
      "MP_ENABLE_DSP=ON requires JUCE (juce::juce_dsp). Either enable "
      "MP_FETCH_JUCE / provide MP_JUCE_SOURCE_DIR, or configure with "
      "-DMP_ENABLE_DSP=OFF (the core-only preset does this).")
  endif()
  add_subdirectory(src/mp_dsp)
endif()
add_subdirectory(src/mp_control)
# JUCE-dependent layers: only when JUCE targets are available
# (core-only builds without JUCE still get mp_core/mp_control + validator + tests).
if(TARGET juce::juce_audio_processors)
  add_subdirectory(src/mp_audio)
endif()
if(TARGET juce::juce_gui_basics)
  add_subdirectory(src/mp_ui)
endif()

if(MP_BUILD_STANDALONE)
  add_subdirectory(apps/MasterpieceApp)
endif()
if(MP_BUILD_PLUGIN)
  add_subdirectory(apps/MasterpiecePlugin)
endif()
# The developer tools and the test suite are not part of a release tree. They
# are built when present and skipped silently when they are not, so the same
# CMakeLists serves a full checkout and a source release that ships only the
# app and the plugin.
if(MP_BUILD_VALIDATOR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/mp-validate-odf")
  add_subdirectory(apps/mp-validate-odf)
endif()
# The headless renderer needs the full audio stack, so it follows the same
# availability rule as the app rather than the JUCE-free validator.
if(TARGET mp_audio AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/mp-render")
  add_subdirectory(apps/mp-render)
endif()

if(MP_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()

# Installers (NSIS on Windows, .deb on Linux). See cmake/Packaging.cmake.
include(cmake/Packaging.cmake)
