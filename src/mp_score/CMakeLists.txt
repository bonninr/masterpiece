# Engraves MusicXML to SVG pages, and reads and writes the player's marks in
# the file. JUCE-free, like mp_core: Verovio, which stays out of the header,
# and pugixml for the marks.
add_library(mp_score STATIC
  ScoreEngraver.h ScoreEngraver.cpp
  ScoreMarks.h ScoreMarks.cpp
)
target_include_directories(mp_score PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(mp_score PUBLIC cxx_std_20)
target_link_libraries(mp_score PRIVATE mp_verovio pugixml::pugixml)
