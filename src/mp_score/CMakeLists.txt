# Engraves MusicXML to SVG pages. JUCE-free, like mp_core: the only
# dependency is the Verovio static library, which stays out of the header.
add_library(mp_score STATIC
  ScoreEngraver.h ScoreEngraver.cpp
)
target_include_directories(mp_score PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(mp_score PUBLIC cxx_std_20)
target_link_libraries(mp_score PRIVATE mp_verovio)
