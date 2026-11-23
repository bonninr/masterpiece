# jOrgan organs: dispositions and their SoundFonts, translated into the
# Hauptwerk definition the loader reads, as GrandOrgue organs are. A module
# of its own: the loader calls JOrganImport.h and nothing else. Uses headers
# of mp_core only, so mp_core can link it. JUCE only for drawing skins and
# unpacking sfArk, where the build has JUCE's graphics; without, consoles in
# the stock layout and SoundFonts as .sf2 files only.
add_library(mp_jorgan STATIC
  SoundFont2.h SoundFont2.cpp
  JOrganDisposition.h JOrganDisposition.cpp
  JOrganOrgan.h
  JOrganHauptwerk.h JOrganHauptwerk.cpp
  JOrganSkin.h JOrganSkin.cpp
  JOrganSfArk.h JOrganSfArk.cpp
  JOrganImport.h JOrganImport.cpp
)
target_include_directories(mp_jorgan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(mp_jorgan PUBLIC cxx_std_20)
target_link_libraries(mp_jorgan PUBLIC pugixml::pugixml)

if(TARGET juce::juce_graphics)
  # Private: its sources build into this library only, not every one above.
  target_link_libraries(mp_jorgan PRIVATE juce::juce_graphics)
  target_compile_definitions(mp_jorgan PRIVATE MP_JORGAN_SKINS=1)

  # sfArkLib (GPL-3): sfArk-packed SoundFonts. Its own sources, built here,
  # on a zlib of two calls (sfark/zlib.h) over JUCE's.
  option(MP_FETCH_SFARKLIB "Fetch sfArkLib for SoundFonts packed with sfArk" ON)
  if(MP_FETCH_SFARKLIB)
    include(FetchContent)
    FetchContent_Declare(
      sfarklib
      GIT_REPOSITORY https://github.com/raboof/sfArkLib.git
      GIT_TAG c710d851a9b9c1759ac8597e12f55ea290a3f007
      SOURCE_SUBDIR do-not-build
    )
    FetchContent_MakeAvailable(sfarklib)
    # Not sfklZip.cpp: its one function, UnMemcomp, is in JOrganSfArk.cpp.
    # sfklZip.cpp declares the message buffer a pointer where sfklCoding.cpp
    # makes it an array: unlinkable with MSVC, a stray write elsewhere.
    set(MP_SFARK_SOURCES
      ${sfarklib_SOURCE_DIR}/sfklCoding.cpp ${sfarklib_SOURCE_DIR}/sfklCrunch.cpp
      ${sfarklib_SOURCE_DIR}/sfklDiff.cpp ${sfarklib_SOURCE_DIR}/sfklFile.cpp
      ${sfarklib_SOURCE_DIR}/sfklLPC.cpp ${sfarklib_SOURCE_DIR}/sfklString.cpp)
    target_sources(mp_jorgan PRIVATE ${MP_SFARK_SOURCES})
    # Every target is little-endian; the library's own warnings are its own.
    set_source_files_properties(${MP_SFARK_SOURCES} PROPERTIES
      COMPILE_DEFINITIONS "USE_MANUAL_ENDIANNESS;MANUAL_LITTLE_ENDIAN;_CRT_SECURE_NO_WARNINGS"
      INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/sfark"
      COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
    target_include_directories(mp_jorgan PRIVATE ${sfarklib_SOURCE_DIR})
    target_compile_definitions(mp_jorgan PRIVATE MP_JORGAN_SFARK=1)
  endif()
endif()
