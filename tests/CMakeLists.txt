add_executable(mp_tests
  MpTest.h MpTest.cpp
  test_core.cpp
  test_split.cpp
  test_sparse_loading.cpp
  test_storage20.cpp
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
  # The codec interface, and the tests that loop over every codec in the
  # registry.
  target_sources(mp_tests PRIVATE test_sample_codec.cpp)
endif()
if(TARGET mp_dsp)
  target_link_libraries(mp_tests PRIVATE mp_dsp)
  target_compile_definitions(mp_tests PRIVATE MP_TEST_HAS_DSP=1)
endif()
# mp_audio carries the JUCE-backed backing store (SampleLibrary). Like mp_dsp
# it only exists in the full build, so its tests are guarded the same way.
if(TARGET mp_audio)
  target_link_libraries(mp_tests PRIVATE mp_audio)
  target_sources(mp_tests PRIVATE test_page_turn.cpp)
  target_compile_definitions(mp_tests PRIVATE MP_TEST_HAS_AUDIO=1)
  # The stop list a screen reader can use: the UI is in mp_audio, and the
  # test reads the accessibility layer of the stops.
  target_sources(mp_tests PRIVATE test_accessibility.cpp)
  target_sources(mp_tests PRIVATE test_midi_power.cpp)
  # The recorder's window: its buttons are sized from their own text (#429).
  target_sources(mp_tests PRIVATE test_recorder_layout.cpp)
  target_sources(mp_tests PRIVATE test_unevenness.cpp)
  target_sources(mp_tests PRIVATE test_settings_archive.cpp)
  target_sources(mp_tests PRIVATE test_held_notes.cpp)
  target_sources(mp_tests PRIVATE test_sustain.cpp)
  # The score panel: the engraving, the page geometry, the player and the
  # panel playing through a small organ. The engraver reads the font folder
  # the configure step prepared.
  if(TARGET mp_score)
    target_sources(mp_tests PRIVATE test_score.cpp)
    target_compile_definitions(mp_tests PRIVATE
      MP_TEST_HAS_SCORE=1
      MP_TEST_SCORE_DATA_DIR="${MP_SCORE_FONTS_DIR}")
  endif()
  target_sources(mp_tests PRIVATE test_pedal_coupling.cpp)
  # The control API, end to end over HTTP on the loopback address.
  if(MP_ENABLE_API)
    target_sources(mp_tests PRIVATE test_api.cpp test_api_transports.cpp)
    target_include_directories(mp_tests PRIVATE ${httplib_SOURCE_DIR})
    if(WIN32)
      target_link_libraries(mp_tests PRIVATE ws2_32)
    endif()
  endif()
endif()

add_test(NAME MpFunctional COMMAND mp_tests --no-perf)
add_test(NAME MpPerf COMMAND mp_tests --perf-only)
