juce_add_plugin(MasterpiecePlugin
  COMPANY_NAME "Masterpiece"
  BUNDLE_ID "org.masterpiece.player"
  PLUGIN_MANUFACTURER_CODE Mspc
  PLUGIN_CODE Msp1
  FORMATS VST3 AU LV2 Standalone
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
