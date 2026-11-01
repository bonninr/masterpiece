# Profile-guided optimisation, as two builds of one build directory:
#
#   -DMP_PGO=GENERATE   instrumented; then run the training:
#                         mp_tests                    (the engine, every shape of work)
#                         Masterpiece --train-profile (the program itself, offline)
#   -DMP_PGO=USE        built again with what the training recorded
#
# GCC writes its counters per object file into MP_PGO_DIR and reads them back
# from there, so both builds use the same build directory. Clang writes raw
# profiles where LLVM_PROFILE_FILE says; merge them into
# MP_PGO_DIR/default.profdata before the second build. MSVC keeps one profile
# per program, written beside it, so only a program that ran in the training is
# optimised by it: the second build only links again.
#
# Only Masterpiece's own targets take part. The libraries are shared by the
# program and the test suite, so the suite's training reaches the program's
# engine on GCC and Clang.
set(MP_PGO "" CACHE STRING "Profile-guided optimisation: GENERATE, USE, or empty for none")
set_property(CACHE MP_PGO PROPERTY STRINGS "" GENERATE USE)
set(MP_PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "Where the training run writes its profile")

function(mp_pgo)
  if(NOT MP_PGO)
    return()
  endif()
  if(NOT MP_PGO MATCHES "^(GENERATE|USE)$")
    message(FATAL_ERROR "MP_PGO is GENERATE, USE or empty, not ${MP_PGO}")
  endif()
  file(MAKE_DIRECTORY "${MP_PGO_DIR}")
  set(_profdata "${MP_PGO_DIR}/default.profdata")
  if(MP_PGO STREQUAL "USE" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC AND NOT EXISTS "${_profdata}")
    message(FATAL_ERROR "MP_PGO=USE: no ${_profdata}; merge the training's raw profiles first")
  endif()
  foreach(_t IN LISTS ARGN)
    if(NOT TARGET ${_t})
      continue()
    endif()
    get_target_property(_type ${_t} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
      continue()
    endif()
    if(MSVC)
      # Whole-program code generation is what the profile is applied in.
      set_property(TARGET ${_t} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
      if(_type STREQUAL "EXECUTABLE" OR _type STREQUAL "SHARED_LIBRARY" OR _type STREQUAL "MODULE_LIBRARY")
        if(MP_PGO STREQUAL "GENERATE")
          target_link_options(${_t} PRIVATE $<$<CONFIG:Release>:/GENPROFILE>)
        else()
          target_link_options(${_t} PRIVATE $<$<CONFIG:Release>:/USEPROFILE>)
        endif()
      endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      if(MP_PGO STREQUAL "GENERATE")
        set(_flags -fprofile-generate=${MP_PGO_DIR} -fprofile-update=prefer-atomic)
        set(_link -fprofile-generate=${MP_PGO_DIR})
      else()
        # Code the training never ran is optimised as without a profile.
        set(_flags -fprofile-use=${MP_PGO_DIR} -fprofile-partial-training -fprofile-correction -Wno-missing-profile)
        set(_link -fprofile-use=${MP_PGO_DIR})
      endif()
      target_compile_options(${_t} PRIVATE ${_flags})
      if(NOT _type STREQUAL "STATIC_LIBRARY")
        target_link_options(${_t} PRIVATE ${_link})
      endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      if(MP_PGO STREQUAL "GENERATE")
        set(_flags -fprofile-instr-generate)
        set(_link -fprofile-instr-generate)
      else()
        set(_flags -fprofile-instr-use=${_profdata} -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
        set(_link -fprofile-instr-use=${_profdata})
      endif()
      target_compile_options(${_t} PRIVATE ${_flags})
      if(NOT _type STREQUAL "STATIC_LIBRARY")
        target_link_options(${_t} PRIVATE ${_link})
      endif()
    else()
      message(STATUS "Masterpiece: no profile-guided build for ${CMAKE_CXX_COMPILER_ID}")
      return()
    endif()
  endforeach()
  message(STATUS "Masterpiece: profile-guided build, ${MP_PGO}, profile in ${MP_PGO_DIR}")
endfunction()
