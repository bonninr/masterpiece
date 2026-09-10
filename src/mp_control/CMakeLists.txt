add_library(mp_control STATIC Control.h Control.cpp MidiMap.h MidiMap.cpp
                              SwitchNetwork.h SwitchNetwork.cpp
                              Combinations.h Combinations.cpp
                              StageSwitches.h StageSwitches.cpp
                              Stepper.h Stepper.cpp
                              WindSolver.h WindSolver.cpp)
target_include_directories(mp_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(mp_control PUBLIC cxx_std_20)
target_link_libraries(mp_control PUBLIC mp_core)
