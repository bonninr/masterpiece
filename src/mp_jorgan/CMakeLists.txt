# jOrgan organs: dispositions and their SoundFonts, translated into the
# Hauptwerk definition the loader reads, as GrandOrgue organs are. A module
# of its own: the loader calls JOrganImport.h and nothing else. Uses headers
# of mp_core only, so mp_core can link it. JUCE only for drawing skins, where
# the build has JUCE's graphics; without, consoles in the stock layout.
add_library(mp_jorgan STATIC
  SoundFont2.h SoundFont2.cpp
  JOrganDisposition.h JOrganDisposition.cpp
  JOrganOrgan.h
  JOrganHauptwerk.h JOrganHauptwerk.cpp
  JOrganSkin.h JOrganSkin.cpp
  JOrganImport.h JOrganImport.cpp
)
target_include_directories(mp_jorgan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(mp_jorgan PUBLIC cxx_std_20)
target_link_libraries(mp_jorgan PUBLIC pugixml::pugixml)
if(TARGET juce::juce_graphics)
  # Private: its sources build into this library only, not every one above.
  target_link_libraries(mp_jorgan PRIVATE juce::juce_graphics)
  target_compile_definitions(mp_jorgan PRIVATE MP_JORGAN_SKINS=1)
endif()
