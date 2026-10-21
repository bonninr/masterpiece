# Every test_*.cpp here is part of the suite: a new test file needs no line
# in this file. A test that needs a module only some builds have guards
# itself with the MP_TEST_HAS_* definition below, and compiles to nothing
# where that module is absent.
file(GLOB MP_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
add_executable(mp_tests
  MpTest.h MpTest.cpp
  ${MP_TEST_SOURCES}
)
target_compile_features(mp_tests PRIVATE cxx_std_20)
# mp_control supplies resolvePipes() and the coupler/combination systems the
# tests exercise; the WSL fast loop compiles it directly, so a missing link
# here only ever shows up in the full CMake build.
target_link_libraries(mp_tests PRIVATE mp_core mp_control mp_archive mp_split)
target_compile_definitions(mp_tests PRIVATE
  MP_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  MP_TEST_TRANSLATIONS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../resources/translations"
  MP_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
if(TARGET mp_sampler)
  target_link_libraries(mp_tests PRIVATE mp_sampler)
  target_compile_definitions(mp_tests PRIVATE MP_TEST_HAS_SAMPLER=1)
endif()
if(TARGET mp_dsp)
  target_link_libraries(mp_tests PRIVATE mp_dsp)
  target_compile_definitions(mp_tests PRIVATE MP_TEST_HAS_DSP=1)
endif()
# mp_audio carries the JUCE-backed backing store (SampleLibrary). Like mp_dsp
# it only exists in the full build, so its tests are guarded the same way.
if(TARGET mp_audio)
  target_link_libraries(mp_tests PRIVATE mp_audio)
  target_compile_definitions(mp_tests PRIVATE MP_TEST_HAS_AUDIO=1)
  # The score panel's tests read the font folder the configure step prepared.
  if(TARGET mp_score)
    target_compile_definitions(mp_tests PRIVATE
      MP_TEST_HAS_SCORE=1
      MP_TEST_SCORE_DATA_DIR="${MP_SCORE_FONTS_DIR}")
  endif()
  # The control API, end to end over HTTP on the loopback address.
  if(MP_ENABLE_API)
    target_compile_definitions(mp_tests PRIVATE MP_TEST_HAS_API=1)
    target_include_directories(mp_tests PRIVATE ${httplib_SOURCE_DIR})
    if(WIN32)
      target_link_libraries(mp_tests PRIVATE ws2_32)
    endif()
  endif()
endif()

add_test(NAME MpFunctional COMMAND mp_tests --no-perf)
add_test(NAME MpPerf COMMAND mp_tests --perf-only)
