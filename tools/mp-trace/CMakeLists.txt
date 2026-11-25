add_executable(mp-trace main.cpp)
target_compile_features(mp-trace PRIVATE cxx_std_20)
target_link_libraries(mp-trace PRIVATE mp_core mp_control)
