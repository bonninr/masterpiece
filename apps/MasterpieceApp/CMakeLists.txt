juce_add_gui_app(MasterpieceApp
  VERSION "${PROJECT_VERSION}"
  PRODUCT_NAME "Masterpiece"
  COMPANY_NAME "Masterpiece"
  BUNDLE_ID "org.masterpiece.player"
  ICON_BIG "${CMAKE_SOURCE_DIR}/packaging/masterpiece.png"
  ICON_SMALL ""
  # Finder's "Open With" for organ definitions and packages (#90).
  DOCUMENT_EXTENSIONS Organ_Hauptwerk_xml CustomOrgan_Hauptwerk_xml organ orgue rar
)

target_sources(MasterpieceApp PRIVATE Main.cpp)
# On Android the app is a shared library that JUCE's Java side loads by this
# name (System.loadLibrary("juce_jni")).
if(ANDROID)
  set_target_properties(MasterpieceApp PROPERTIES OUTPUT_NAME juce_jni)
endif()
target_compile_features(MasterpieceApp PRIVATE cxx_std_20)
# One place holds the version: the project() call. It was written out a second
# time in Main.cpp, and the two drifted apart at nearly every release.
target_compile_definitions(MasterpieceApp PRIVATE MP_VERSION="${PROJECT_VERSION}")
target_link_libraries(MasterpieceApp PRIVATE
  mp_audio mp_ui mp_control mp_core
  juce::juce_gui_basics juce::juce_audio_utils juce::juce_audio_devices
)
if(TARGET mp_sampler)
  target_link_libraries(MasterpieceApp PRIVATE mp_sampler)
endif()
if(TARGET mp_dsp)
  target_link_libraries(MasterpieceApp PRIVATE mp_dsp)
endif()
