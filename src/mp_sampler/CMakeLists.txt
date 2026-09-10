add_library(mp_sampler STATIC
  StreamingEngine.h StreamingEngine.cpp
  DiskProbe.h DiskProbe.cpp
  VoiceEngine.h VoiceEngine.cpp
)
target_include_directories(mp_sampler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(mp_sampler PUBLIC cxx_std_20)
# The voice engine runs a worker pool (ADR-012), so it needs pthreads.
find_package(Threads REQUIRED)
target_link_libraries(mp_sampler PUBLIC mp_core Threads::Threads)
if(TARGET juce::juce_audio_formats)
  target_link_libraries(mp_sampler PUBLIC juce::juce_audio_basics juce::juce_audio_formats)
endif()
