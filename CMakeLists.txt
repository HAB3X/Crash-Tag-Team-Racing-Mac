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

add_executable(crash-cttr src/main.cpp src/fibers.cpp src/radiop.cpp src/audio.cpp src/pctext.cpp src/controls.cpp src/typing.cpp
                          src/menus.cpp src/hints.cpp src/pause.cpp src/pcsettings.cpp)
target_link_libraries(crash-cttr PRIVATE cttr_code ps2runtime)

if(APPLE)
    # A Mac app (build/crash-cttr.app; the release names it "Crash Tag Team Racing.app": the executable keeps its name, which
    # the runs' lock looks for), with the icon tools/make_icon.py makes from the game's own art on your disc: the save icon
    # (a crate with Crash's face, in COMMON.RCF), lit and blended as its flat colours and single picture want (with the
    # python3 on the PATH: it needs Pillow and NumPy)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(ICON ${CMAKE_CURRENT_BINARY_DIR}/icon/AppIcon.icns)
    add_custom_command(
        OUTPUT ${ICON}
        COMMAND /usr/bin/env python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_icon.py ${CTTR_DISC}
                --icon COMMON/COMMON.RCF:art/memcard/crash.ico --yaw 20 --pitch 15 --light --smooth --runtime ${PS2_RUNTIME_DIR}
                --out ${CMAKE_CURRENT_BINARY_DIR}/icon
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_icon.py
        COMMENT "Making the app's icon from the disc"
        VERBATIM)
    target_sources(crash-cttr PRIVATE ${ICON})
    set_source_files_properties(${ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(crash-cttr PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "Crash Tag Team Racing"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.crashtagteamracing.native"
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_ICON_FILE AppIcon)

    # The release: the app made standalone (its libraries in Contents/Frameworks, their licences, signed for this Mac) and
    # zipped: build/dist/Crash Tag Team Racing.app and Crash-Tag-Team-Racing-macOS.zip. `cmake --build build --target release`
    add_custom_target(release
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/bundle_app.py --app $<TARGET_BUNDLE_DIR:crash-cttr>
                --name "Crash Tag Team Racing" --zip
        DEPENDS crash-cttr
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Making the standalone app (build/dist)"
        VERBATIM)
endif()
