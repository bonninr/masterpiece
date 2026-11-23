# jOrgan organs: dispositions and their SoundFonts, translated into the
# Hauptwerk definition the loader reads, as GrandOrgue organs are. A module
# of its own: the loader calls JOrganImport.h and nothing else. Uses headers
# of mp_core only, so mp_core can link it. JUCE-free, like mp_core.
add_library(mp_jorgan STATIC
  SoundFont2.h SoundFont2.cpp
  JOrganDisposition.h JOrganDisposition.cpp
  JOrganOrgan.h
  JOrganHauptwerk.h JOrganHauptwerk.cpp
  JOrganImport.h JOrganImport.cpp
)
target_include_directories(mp_jorgan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(mp_jorgan PUBLIC cxx_std_20)
target_link_libraries(mp_jorgan PUBLIC pugixml::pugixml)
