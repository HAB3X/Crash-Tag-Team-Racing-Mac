cmake_minimum_required(VERSION 3.20)
project(ps2_native_runtime C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

find_package(SDL3 REQUIRED CONFIG)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(PS2_RUNTIME_ROOT ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "")
set(PS2_PYTHON ${Python3_EXECUTABLE} CACHE INTERNAL "")

# The hardware layer (hw/README.md)
add_library(ps2hw STATIC
    hw/ee/gif.cpp
    hw/ee/vif.cpp
    hw/ee/vu.cpp
    hw/ee/vu1translate.cpp
    hw/gs/gs.cpp
    hw/gs/gsdraw.cpp
    hw/gs/gsmemory.cpp
    hw/gs/gsscale.cpp
    hw/gs/workers.cpp
    hw/png.cpp
)
target_include_directories(ps2hw PUBLIC hw)
target_compile_options(ps2hw PRIVATE -O2 -fno-strict-float-cast-overflow -Wno-deprecated-declarations)

# The runtime: memory, the kernel, the hardware registers, the DMAC, the IOP and the presenter
add_library(ps2runtime STATIC
    runtime/src/boot.cpp
    runtime/src/cpu.cpp
    runtime/src/disc.cpp
    runtime/src/dmac.cpp
    runtime/src/gs.cpp
    runtime/src/input.cpp
    runtime/src/io.cpp
    runtime/src/kernel.cpp
    runtime/src/log.cpp
    runtime/src/memory.cpp
    runtime/src/render.cpp
    runtime/src/mmi.c
    runtime/src/iop/sif.cpp
    runtime/src/iop/mcserv.cpp
    runtime/src/iop/padman.cpp
    runtime/src/iop/servers.cpp
    runtime/src/sound/libsd.cpp
    runtime/src/sound/output.cpp
    runtime/src/sound/spu2.cpp
)
target_include_directories(ps2runtime PUBLIC runtime/include)
target_link_libraries(ps2runtime PUBLIC ps2hw SDL3::SDL3)
target_compile_options(ps2runtime PRIVATE -O2 -g -Wall -Wno-unused-function)

# ps2_recompile(TARGET CONFIG file.toml DISC image EXECUTABLE name [FILES n])
#
# Copies the executable out of your disc image at build time, recompiles it to C in the build folder, and makes a library of
# it. Nothing of the game's goes in a source folder.
function(ps2_recompile target)
    cmake_parse_arguments(ARG "" "CONFIG;DISC;EXECUTABLE;FILES" "" ${ARGN})
    if(NOT ARG_FILES)
        set(ARG_FILES 48)
    endif()
    if(NOT ARG_DISC)
        message(FATAL_ERROR "ps2_recompile: no disc image given (DISC)")
    endif()
    set(out ${CMAKE_CURRENT_BINARY_DIR}/generated/${target})
    set(elf ${CMAKE_CURRENT_BINARY_DIR}/generated/${ARG_EXECUTABLE})
    set(sources ${out}/table.c)
    math(EXPR last "${ARG_FILES} - 1")
    foreach(n RANGE ${last})
        string(LENGTH "${n}" len)
        if(len EQUAL 1)
            set(name "00${n}")
        elseif(len EQUAL 2)
            set(name "0${n}")
        else()
            set(name "${n}")
        endif()
        list(APPEND sources ${out}/code_${name}.c)
    endforeach()
    file(GLOB recomp_py ${PS2_RUNTIME_ROOT}/recomp/*.py)
    add_custom_command(
        OUTPUT ${elf}
        COMMAND ${PS2_PYTHON} ${PS2_RUNTIME_ROOT}/tools/disc.py get ${ARG_DISC} ${ARG_EXECUTABLE} ${elf}
        DEPENDS ${PS2_RUNTIME_ROOT}/tools/disc.py
        COMMENT "Reading ${ARG_EXECUTABLE} from the disc"
        VERBATIM)
    add_custom_command(
        OUTPUT ${sources} ${out}/funcs.h
        COMMAND ${PS2_PYTHON} ${PS2_RUNTIME_ROOT}/recomp/recomp.py ${ARG_CONFIG} ${elf} ${out} --files ${ARG_FILES}
        DEPENDS ${elf} ${ARG_CONFIG} ${recomp_py}
        COMMENT "Recompiling ${ARG_EXECUTABLE}"
        VERBATIM)
    add_library(${target} STATIC ${sources})
    target_include_directories(${target} PRIVATE ${out})
    target_link_libraries(${target} PUBLIC ps2runtime)
    # MIPS integer arithmetic wraps; the generated code is big, so it's optimised but not hard
    target_compile_options(${target} PRIVATE -O1 -fwrapv -fno-strict-aliasing -Wno-unused-label -Wno-unused-variable
                           -Wno-parentheses-equality -Wno-unused-value -Wno-tautological-compare)

    # VU1's programs, translated from the executable's .vutext (recomp/vutranslate.py; hw/ee/vu1translate.h)
    set(vu1 ${out}/vu1programs.cpp)
    foreach(shard RANGE 7)
        list(APPEND vu1 ${out}/vu1programs_${shard}.cpp)
    endforeach()
    add_custom_command(
        OUTPUT ${vu1}
        COMMAND ${PS2_PYTHON} ${PS2_RUNTIME_ROOT}/recomp/vutranslate.py ${elf} ${out}/vu1programs.cpp
        DEPENDS ${elf} ${PS2_RUNTIME_ROOT}/recomp/vutranslate.py ${PS2_RUNTIME_ROOT}/recomp/vudecode.py
        COMMENT "Translating ${ARG_EXECUTABLE}'s VU1 programs"
        VERBATIM)
    target_sources(${target} PRIVATE ${vu1})
    set_source_files_properties(${vu1} PROPERTIES COMPILE_OPTIONS
        "-O2;-g0;-std=c++20;-ffp-contract=off;-fno-strict-float-cast-overflow;-Wno-unused-variable;-Wno-unused-label;-Wno-unused-but-set-variable")
endfunction()
