# One keyboard playing a whole organ (#358): JUCE-free, between the MIDI
# inputs and the organ, so the core and the tests build without JUCE.
add_library(mp_split STATIC SplitKeyboard.h SplitKeyboard.cpp SplitText.h SplitText.cpp)
target_include_directories(mp_split PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(mp_split PUBLIC cxx_std_20)
