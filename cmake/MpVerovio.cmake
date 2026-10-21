# Verovio (music engraving), compiled into a static library of our own.
#
# Verovio's own CMake project is written for its command-line tool and its
# language bindings: it sets include paths and warning flags for the whole
# directory, builds a shared library for BUILD_AS_LIBRARY (which gives no
# import library on Windows) and always compiles the bundled copy of
# pugixml. This file takes the sources and nothing else, and as little of
# them as the score panel needs.
#
#  - Only the MusicXML reader is compiled. Humdrum, ABC, Plaine & Easie,
#    GABC and DARMS are switched off; they are the bulk of the code and of
#    the binary. Compressed MusicXML (.mxl) is unpacked by the program with
#    its own zip code, so NO_MXL_SUPPORT is set and src/filereader.cpp, the
#    one user of Verovio's bundled zip library, is left out; MpVerovioStubs.cpp
#    supplies a reader that opens nothing.
#  - Editor code is switched off with NO_EDIT_SUPPORT. In 6.3.0 that switch
#    alone does not compile: src/editfunctor.cpp and the editortoolkit_*.cpp
#    files (cmn, neume, shared) are not guarded and need classes that only
#    exist with the editor, so they are left out of the source list, and
#    MpVerovioStubs.cpp supplies the one symbol the rest still names.
#  - pugixml is the copy the rest of the project uses (target
#    pugixml::pugixml). Verovio's src/pugi/pugixml.cpp is left out and its
#    include/pugi directory is not on the include path, so the program holds
#    one copy of the pugi namespace. Verovio 6.3.0 uses the std::string
#    overloads that pugixml added in 1.16, so the project's tag is v1.16.
#  - Compiled for size: -Os (/O1), each function and datum in a section of
#    its own, and the program linked with unused sections removed, so what
#    the panel never calls is not carried. Engraving runs when a score is
#    opened or the zoom changes, never on the audio thread. Link-time
#    optimisation is off for this library: it would undo the size setting
#    and lengthen every link.
#
# Provides the target mp_verovio, MP_SCORE_FONTS_DIR (the run-time files) and
# MP_SCORE_FONTS_ZIP (the same, packed).
include_guard(GLOBAL)

include(FetchContent)
set(MP_VEROVIO_TAG "version-6.3.0" CACHE STRING "Verovio git tag for FetchContent")
FetchContent_Declare(
  verovio
  GIT_REPOSITORY https://github.com/rism-digital/verovio.git
  GIT_TAG ${MP_VEROVIO_TAG}
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE
  # Verovio's own CMake project is not used: pointing at a folder without a
  # CMakeLists.txt makes FetchContent_MakeAvailable fetch and skip it.
  SOURCE_SUBDIR include
)
FetchContent_MakeAvailable(verovio)
set(_vrv ${verovio_SOURCE_DIR})

file(GLOB _vrv_core CONFIGURE_DEPENDS ${_vrv}/src/*.cpp)
list(FILTER _vrv_core EXCLUDE REGEX
     "/(editfunctor|editortoolkit_shared|editortoolkit_cmn|editortoolkit_neume|filereader)\\.cpp$")
file(GLOB _vrv_mei CONFIGURE_DEPENDS ${_vrv}/libmei/dist/*.cpp ${_vrv}/libmei/addons/*.cpp)
file(GLOB _vrv_midi CONFIGURE_DEPENDS ${_vrv}/src/midi/*.cpp)

add_library(mp_verovio STATIC
  ${_vrv_core} ${_vrv_mei} ${_vrv_midi}
  ${_vrv}/src/crc/crc.cpp
  ${_vrv}/src/json/jsonxx.cc
  ${CMAKE_CURRENT_LIST_DIR}/MpVerovioStubs.cpp)
target_include_directories(mp_verovio PUBLIC
  ${_vrv}/include ${_vrv}/include/vrv
  ${_vrv}/include/crc ${_vrv}/include/midi ${_vrv}/include/hum
  ${_vrv}/include/json ${_vrv}/include/tuning-library
  ${_vrv}/libmei/dist ${_vrv}/libmei/addons)
if(WIN32)
  target_include_directories(mp_verovio PUBLIC ${_vrv}/include/win32)
endif()
target_compile_features(mp_verovio PUBLIC cxx_std_20)
target_compile_definitions(mp_verovio PUBLIC
  NO_HUMDRUM_SUPPORT NO_ABC_SUPPORT NO_PAE_SUPPORT NO_GABC_SUPPORT
  NO_DARMS_SUPPORT NO_MXL_SUPPORT NO_EDIT_SUPPORT NO_RUNTIME)
target_link_libraries(mp_verovio PUBLIC pugixml::pugixml)
if(ANDROID)
  target_link_libraries(mp_verovio PUBLIC log)
endif()
set_target_properties(mp_verovio PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

# A compiler cache of Verovio's own. Its sources change only when the tag
# does, so its objects can be kept from one run to the next whatever else
# changes, in a folder no other compile shares: the program's own cache is
# filled by JUCE and the program and would push them out. Set
# MP_VEROVIO_CCACHE_DIR (here or in the environment, which reaches the
# Android build through Gradle) to a folder that is kept between builds.
set(MP_VEROVIO_CCACHE_DIR "$ENV{MP_VEROVIO_CCACHE_DIR}" CACHE PATH
    "Compiler cache folder for Verovio alone (empty: none)")
if(MP_VEROVIO_CCACHE_DIR)
  find_program(MP_CCACHE ccache)
  if(MP_CCACHE)
    set(_vrv_launcher ${CMAKE_COMMAND} -E env CCACHE_DIR=${MP_VEROVIO_CCACHE_DIR}
        CCACHE_BASEDIR=${CMAKE_BINARY_DIR} ${MP_CCACHE})
    set_target_properties(mp_verovio PROPERTIES
      C_COMPILER_LAUNCHER "${_vrv_launcher}" CXX_COMPILER_LAUNCHER "${_vrv_launcher}")
    message(STATUS "Verovio compiles through ccache in ${MP_VEROVIO_CCACHE_DIR}")
  else()
    message(STATUS "MP_VEROVIO_CCACHE_DIR is set but ccache was not found")
  endif()
endif()
# Third-party code: its warnings are not ours to act on.
if(MSVC)
  target_compile_options(mp_verovio PRIVATE /W0 /bigobj /O1 /Gy /Gw)
  # Unused functions and data left out, and identical ones folded. The
  # release default already, unless a configuration asks for debug
  # information; stated so that one does too.
  target_link_options(mp_verovio INTERFACE
    $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)
else()
  target_compile_options(mp_verovio PRIVATE -w -Os -ffunction-sections -fdata-sections)
  if(APPLE)
    target_link_options(mp_verovio INTERFACE $<$<NOT:$<CONFIG:Debug>>:-Wl,-dead_strip>)
  else()
    target_link_options(mp_verovio INTERFACE $<$<NOT:$<CONFIG:Debug>>:-Wl,--gc-sections>)
  endif()
endif()

# The files the toolkit reads at run time: the bounding boxes and glyph
# outlines of Bravura (the SMuFL reference font, SIL OFL), the bounding boxes
# of Leipzig (the toolkit's default font, which it always loads), and the
# widths of the text font. Copied to a folder of their own, for the tests, and
# packed in a zip the program carries and unpacks at first use: 369 KB.
set(MP_SCORE_FONTS_DIR ${CMAKE_BINARY_DIR}/score-fonts-${MP_VEROVIO_TAG})
set(MP_SCORE_FONTS_ZIP ${CMAKE_BINARY_DIR}/score-resources/fonts.zip)
if(NOT EXISTS ${MP_SCORE_FONTS_DIR}/.complete OR NOT EXISTS ${MP_SCORE_FONTS_ZIP})
  file(REMOVE_RECURSE ${MP_SCORE_FONTS_DIR})
  file(COPY ${_vrv}/data/Bravura ${_vrv}/data/Bravura.xml ${_vrv}/data/Leipzig.xml ${_vrv}/data/text
       DESTINATION ${MP_SCORE_FONTS_DIR})
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/score-resources)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar cf ${MP_SCORE_FONTS_ZIP} --format=zip -- Bravura Bravura.xml Leipzig.xml text
    WORKING_DIRECTORY ${MP_SCORE_FONTS_DIR}
    RESULT_VARIABLE _zip_result)
  if(NOT _zip_result EQUAL 0)
    message(FATAL_ERROR "Packing the score fonts failed")
  endif()
  file(WRITE ${MP_SCORE_FONTS_DIR}/.complete "")
endif()
set(MP_SCORE_FONTS_DIR ${MP_SCORE_FONTS_DIR} CACHE INTERNAL "")
set(MP_SCORE_FONTS_ZIP ${MP_SCORE_FONTS_ZIP} CACHE INTERNAL "")
# Bravura itself, for the text the page draws with it: the note in a metronome
# mark and the other music symbols inside text. 513 KB, SIL OFL.
set(MP_SCORE_MUSIC_FONT ${_vrv}/fonts/Bravura/Bravura.otf CACHE INTERNAL "")
