# mp_dsp always ships (ADR-005): it is disabled at config time via
# MP_ENABLE_DSP, or per feature at run time via EngineSwitch — never absent.
# It links juce_dsp unconditionally (ADR-012: JUCE provides the filters), so
# this module builds only in the full CMake build, not the JUCE-free fast loop.
add_library(mp_dsp STATIC Dsp.h Dsp.cpp)
target_include_directories(mp_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(mp_dsp PUBLIC cxx_std_20)
target_link_libraries(mp_dsp PUBLIC mp_core juce::juce_dsp)
