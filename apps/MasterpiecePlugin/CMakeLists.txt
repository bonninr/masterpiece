# The formats each platform's package ships. The program itself is
# apps/MasterpieceApp; a plugin Standalone would be a second copy of it.
if(APPLE)
  set(_mp_plugin_formats VST3 AU)
else()
  set(_mp_plugin_formats VST3 LV2)
endif()

juce_add_plugin(MasterpiecePlugin
  COMPANY_NAME "Masterpiece"
  BUNDLE_ID "org.masterpiece.player"
  PLUGIN_MANUFACTURER_CODE Mspc
  PLUGIN_CODE Msp1
  FORMATS ${_mp_plugin_formats}
  # LV2 identifies plugins by URI and requires a real scheme; JUCE static-asserts
  # on a bare name. This URI is an identifier, not a live address.
  LV2URI "https://masterpiece.audio/plugins/masterpiece"
  PRODUCT_NAME "Masterpiece"
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE
  IS_SYNTH TRUE
)

target_sources(MasterpiecePlugin PRIVATE PluginEntry.cpp)
target_compile_features(MasterpiecePlugin PRIVATE cxx_std_20)
target_link_libraries(MasterpiecePlugin PRIVATE mp_audio mp_ui mp_control mp_core
  juce::juce_audio_utils
)
if(TARGET mp_sampler)
  target_link_libraries(MasterpiecePlugin PRIVATE mp_sampler)
endif()
if(TARGET mp_dsp)
  target_link_libraries(MasterpiecePlugin PRIVATE mp_dsp)
endif()

# CLAP, through clap-juce-extensions (free-audio): off by default. Tried with
# JUCE 9.0.1 on Linux: builds, loads in a CLAP host, restores its organ from
# the host's state and plays. A host must offer timer support: JUCE's message
# loop runs on its timers there.
option(MP_BUILD_CLAP "Build the plugin as CLAP too (clap-juce-extensions)" OFF)
set(MP_CLAP_JUCE_DIR "" CACHE PATH "A clap-juce-extensions checkout with its submodules; fetched when empty")
set(MP_CLAP_JUCE_COMMIT "3837f4a" CACHE STRING "clap-juce-extensions commit")
if(MP_BUILD_CLAP)
  if(MP_CLAP_JUCE_DIR STREQUAL "")
    include(FetchContent)
    FetchContent_Declare(clap_juce_extensions
      GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
      GIT_TAG ${MP_CLAP_JUCE_COMMIT}
      GIT_SUBMODULES_RECURSE TRUE)
    FetchContent_MakeAvailable(clap_juce_extensions)
  else()
    add_subdirectory(${MP_CLAP_JUCE_DIR} ${CMAKE_BINARY_DIR}/clap-juce-extensions EXCLUDE_FROM_ALL)
  endif()
  clap_juce_extensions_plugin(TARGET MasterpiecePlugin
    CLAP_ID "org.masterpiece.player"
    CLAP_FEATURES instrument sampler)
endif()
