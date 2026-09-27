add_library(mp_sampler STATIC
  StreamingEngine.h StreamingEngine.cpp
  DiskProbe.h DiskProbe.cpp
  VoiceEngine.h VoiceEngine.cpp
  SimdRun.h SimdRun.cpp
)
# The per-frame reader and the vector runs must compute the same numbers.
# Clang (Apple's, by default on arm64) and GCC may fuse a multiply and an add
# into one instruction with a single rounding; neither may here. MSVC does not
# fuse unless asked.
set_source_files_properties(VoiceEngine.cpp SimdRun.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
target_include_directories(mp_sampler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(mp_sampler PUBLIC cxx_std_20)
# The voice engine runs a worker pool (ADR-012), so it needs pthreads.
find_package(Threads REQUIRED)
target_link_libraries(mp_sampler PUBLIC mp_core Threads::Threads)
if(TARGET juce::juce_audio_formats)
  target_link_libraries(mp_sampler PUBLIC juce::juce_audio_basics juce::juce_audio_formats)
endif()
