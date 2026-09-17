add_executable(mp_tests
  MpTest.h MpTest.cpp
  test_core.cpp
)
target_compile_features(mp_tests PRIVATE cxx_std_20)
# mp_control supplies resolvePipes() and the coupler/combination systems the
# tests exercise; the WSL fast loop compiles it directly, so a missing link
# here only ever shows up in the full CMake build.
target_link_libraries(mp_tests PRIVATE mp_core mp_control)
target_compile_definitions(mp_tests PRIVATE
  MP_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
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
endif()

add_test(NAME MpFunctional COMMAND mp_tests --no-perf)
add_test(NAME MpPerf COMMAND mp_tests --perf-only)
