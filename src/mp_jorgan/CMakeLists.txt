# jOrgan organs: dispositions and their SoundFonts, converted to GrandOrgue
# organs that the program then opens as any other. A module of its own:
# nothing outside it depends on how it works, only on JOrganImport.h.
# JUCE-free, like mp_core.
add_library(mp_jorgan STATIC
  SoundFont2.h SoundFont2.cpp
  JOrganDisposition.h JOrganDisposition.cpp
  JOrganImport.h JOrganImport.cpp
)
target_include_directories(mp_jorgan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(mp_jorgan PUBLIC cxx_std_20)
target_link_libraries(mp_jorgan PUBLIC mp_core pugixml::pugixml)
