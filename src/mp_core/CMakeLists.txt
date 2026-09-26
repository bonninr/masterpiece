add_library(mp_core STATIC
  OrganModel.h
  OdfLoader.h OdfLoader.cpp
  GrandOrgueImport.h GrandOrgueImport.cpp
  CodmCompiler.h CodmCompiler.cpp
  Temperament.h Temperament.cpp
)
target_include_directories(mp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(mp_core PUBLIC cxx_std_20)
target_link_libraries(mp_core PUBLIC pugixml::pugixml)
if(TARGET juce::juce_core)
  target_link_libraries(mp_core PUBLIC juce::juce_core juce::juce_data_structures juce::juce_audio_basics juce::juce_audio_formats)
endif()
