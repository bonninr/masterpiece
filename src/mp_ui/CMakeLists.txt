# The UI sources are compiled into mp_audio (see that CMakeLists for why the
# editor cannot live in a library layered above the processor). mp_ui remains
# as an INTERFACE target so app and plugin link lines — and any future
# UI-only sources — keep working without a second copy of anything.
add_library(mp_ui INTERFACE)
# Console.cpp is compiled into mp_audio alongside Ui.cpp, for the same reason:
# it is part of the editor the processor hands back.
target_include_directories(mp_ui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mp_ui INTERFACE mp_audio juce::juce_gui_basics juce::juce_graphics)
