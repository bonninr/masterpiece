add_executable(mp-trace main.cpp)
target_compile_features(mp-trace PRIVATE cxx_std_20)
target_link_libraries(mp-trace PRIVATE mp_core mp_control)
# With the audio engine built, the trace runs through a whole processor.
if(TARGET mp_audio)
  target_link_libraries(mp-trace PRIVATE mp_audio)
  target_compile_definitions(mp-trace PRIVATE MP_TRACE_HAS_PROCESSOR=1)
endif()
