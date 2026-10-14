cmake_minimum_required(VERSION 3.20)
project(crash_cttr_mac VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)

# The runtime: its folder in this repository, or a checkout of its own next to it
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/ps2-native-runtime/CMakeLists.txt)
    set(PS2_RUNTIME_DEFAULT ${CMAKE_CURRENT_SOURCE_DIR}/ps2-native-runtime)
else()
    set(PS2_RUNTIME_DEFAULT ${CMAKE_CURRENT_SOURCE_DIR}/../ps2-native-runtime)
endif()
set(PS2_RUNTIME_DIR ${PS2_RUNTIME_DEFAULT} CACHE PATH "ps2-native-runtime's folder")
add_subdirectory(${PS2_RUNTIME_DIR} ps2-native-runtime)

# Your own disc image: Crash Tag Team Racing, USA (SLUS-21191), an .iso
set(CTTR_DISC "" CACHE FILEPATH "Your Crash Tag Team Racing (USA) disc image")
if(NOT CTTR_DISC)
    message(FATAL_ERROR "Set CTTR_DISC to your disc image: cmake -DCTTR_DISC=/path/to/game.iso ...")
endif()

ps2_recompile(cttr_code CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/config/cttr.toml DISC ${CTTR_DISC} EXECUTABLE SLUS_211.91 FILES 48)

add_executable(crash-cttr src/main.cpp)
target_link_libraries(crash-cttr PRIVATE cttr_code ps2runtime)
