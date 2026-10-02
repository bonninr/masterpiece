juce_add_gui_app(MasterpieceApp
  VERSION "${PROJECT_VERSION}"
  PRODUCT_NAME "Masterpiece"
  COMPANY_NAME "Masterpiece"
  BUNDLE_ID "org.masterpiece.player"
  ICON_BIG "${CMAKE_SOURCE_DIR}/packaging/masterpiece.png"
  ICON_SMALL ""
  # Finder's "Open With" for organ definitions and packages (#90).
  DOCUMENT_EXTENSIONS Organ_Hauptwerk_xml CustomOrgan_Hauptwerk_xml organ orgue rar
  # iOS only; the other platforms ignore these. A sample set reaches an iPad
  # through the Files app, into the app's own Documents folder, and the organ
  # keeps sounding when the screen locks or another app comes to the front.
  FILE_SHARING_ENABLED TRUE
  DOCUMENT_BROWSER_ENABLED TRUE
  BACKGROUND_AUDIO_ENABLED TRUE
  REQUIRES_FULL_SCREEN TRUE
  STATUS_BAR_HIDDEN TRUE
  TARGETED_DEVICE_FAMILY "1,2"
  # A console is wider than it is tall.
  IPHONE_SCREEN_ORIENTATIONS UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
  IPAD_SCREEN_ORIENTATIONS UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
)

target_sources(MasterpieceApp PRIVATE Main.cpp)
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
