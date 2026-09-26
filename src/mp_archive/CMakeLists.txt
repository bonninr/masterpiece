# Organs read straight from their RAR packages. JUCE-free, like mp_core; the
# only module that links libarchive.
add_library(mp_archive STATIC
  OrganArchive.h OrganArchive.cpp
)
target_include_directories(mp_archive PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(mp_archive PUBLIC cxx_std_20)
target_link_libraries(mp_archive PRIVATE ${MP_LIBARCHIVE_TARGET})
