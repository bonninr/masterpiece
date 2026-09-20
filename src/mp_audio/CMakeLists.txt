# The editor is compiled INTO mp_audio, not into a library layered above it.
# MasterpieceProcessor::createEditor() is a member of a class declared here, so
# its definition belongs here too: when it lived in mp_ui (which PUBLIC-links
# mp_audio) CMake put mp_ui first on the link line and the linker walked past
# Ui.o before mp_audio introduced the undefined vtable entry. Whole-archiving
# mp_ui "fixed" that by dragging in duplicate copies of JUCE's bundled libpng
# and FLAC. Owning the file outright is the fix; mp_ui stays an INTERFACE
# target so existing link lines keep working.
add_library(mp_audio STATIC
  MasterpieceProcessor.h MasterpieceProcessor.cpp
  SampleLibrary.h SampleLibrary.cpp
  Metronome.h Metronome.cpp
  AudioRecorder.h AudioRecorder.cpp
  MidiRecorder.h MidiRecorder.cpp
  Convolver.h Convolver.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../mp_ui/Ui.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../mp_ui/Ui.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../mp_ui/BmpImage.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../mp_ui/BmpImage.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../mp_ui/Console.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../mp_ui/Console.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../mp_ui/Settings.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../mp_ui/Settings.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../mp_ui/Wizard.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../mp_ui/LoadingDialog.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../mp_ui/ManualDialog.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../mp_ui/ManualDialog.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../mp_ui/OrganDialog.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../mp_ui/OrganDialog.cpp
)
target_include_directories(mp_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(mp_audio PUBLIC cxx_std_20)
target_link_libraries(mp_audio PUBLIC
  mp_core mp_control
  juce::juce_audio_processors juce::juce_audio_utils juce::juce_audio_devices
  juce::juce_audio_formats juce::juce_data_structures
  juce::juce_gui_basics juce::juce_graphics
)
if(TARGET mp_sampler)
  target_link_libraries(mp_audio PUBLIC mp_sampler)
endif()
if(TARGET mp_dsp)
  target_link_libraries(mp_audio PUBLIC mp_dsp)
  target_compile_definitions(mp_audio PUBLIC MP_ENABLE_DSP=1)
endif()
