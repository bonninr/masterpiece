# A small iOS app for trying window placement on iPhone and iPad simulators
# in minutes: one window, buttons, a popup menu and a second window, each
# variant chosen by a launch argument and every geometry and touch logged.
# Built and run by .github/workflows/ios-probe.yml.
cmake_minimum_required(VERSION 3.22)
project(IosProbe VERSION 0.1.0 LANGUAGES C CXX OBJCXX)

include(FetchContent)
FetchContent_Declare(juce
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 9.0.1
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(juce)

juce_add_gui_app(IosProbe
  PRODUCT_NAME "Probe"
  COMPANY_NAME "Masterpiece"
  BUNDLE_ID "org.masterpiece.probe"
  FILE_SHARING_ENABLED TRUE
  REQUIRES_FULL_SCREEN TRUE
  STATUS_BAR_HIDDEN TRUE
  TARGETED_DEVICE_FAMILY "1,2"
  IPHONE_SCREEN_ORIENTATIONS UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
  IPAD_SCREEN_ORIENTATIONS UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight)

target_sources(IosProbe PRIVATE Main.cpp Scene.mm)
target_compile_features(IosProbe PRIVATE cxx_std_20)
target_compile_definitions(IosProbe PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(IosProbe PRIVATE juce::juce_gui_basics)
