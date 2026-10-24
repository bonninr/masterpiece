# macOS 15 and later ask the player before an app sends to a device on the
# local network, as the control API does when it answers a tablet or phone over
# OSC. This description is the explanation in that question. Phones and tablets
# leave the API out of the build, and the key with it.
set(_mp_local_network)
if(MP_ENABLE_API)
  set(_mp_local_network
    LOCAL_NETWORK_PERMISSION_ENABLED TRUE
    LOCAL_NETWORK_PERMISSION_TEXT "Masterpiece uses the local network to answer the tablets and phones that control it, when the control API is on.")
endif()

juce_add_gui_app(MasterpieceApp
  VERSION "${PROJECT_VERSION}"
  PRODUCT_NAME "Masterpiece"
  COMPANY_NAME "Masterpiece"
  BUNDLE_ID "org.masterpiece.player"
  ICON_BIG "${CMAKE_SOURCE_DIR}/packaging/masterpiece.png"
  ICON_SMALL ""
  # Finder's "Open With" for organ definitions and packages (#90).
  DOCUMENT_EXTENSIONS Organ_Hauptwerk_xml CustomOrgan_Hauptwerk_xml organ orgue rar
  # macOS and iOS end an app that opens the system's Bluetooth MIDI window
  # (the "Bluetooth MIDI" button in the audio settings) without this
  # description in its Info.plist (#563).
  BLUETOOTH_PERMISSION_ENABLED TRUE
  BLUETOOTH_PERMISSION_TEXT "Masterpiece uses Bluetooth to connect to Bluetooth MIDI keyboards and consoles."
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
  ${_mp_local_network}
)

target_sources(MasterpieceApp PRIVATE Main.cpp JackMidi.cpp)
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
