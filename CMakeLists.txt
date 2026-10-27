cmake_minimum_required(VERSION 3.20)
project(u7_atari VERSION 0.1.0 LANGUAGES C CXX ASM)

# Which game: "Black Gate" or "Serpent Isle" (both ultima7-portable trees share plat.h).
set(U7_GAME "Black Gate" CACHE STRING "Upstream ultima7-portable game to build")
set(U7_UPSTREAM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/upstream/ultima7-portable/${U7_GAME}")
if(NOT EXISTS "${U7_UPSTREAM_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Missing ${U7_UPSTREAM_DIR}; run: git submodule update --init")
endif()

# Upstream builds the game as the u7_game object library; we supply the backend instead of
# libmatoya, munt and Nuked OPL3.
set(U7_BACKEND_MATOYA OFF CACHE BOOL "" FORCE)
# The 030 reads and writes words and longs at any address, so let GCC do so: with its m68k
# default it copies and compares anything not known to be aligned a byte at a time.
add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-mno-strict-align>)
add_subdirectory("${U7_UPSTREAM_DIR}" game)

file(GLOB U7_ATARI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/atari/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/atari/*.S")
add_executable(u7 ${U7_ATARI_SOURCES})
set_target_properties(u7 PROPERTIES OUTPUT_NAME ULTIMA7)
# A link map, to turn addresses from Hatari's debugger into function names.
target_link_options(u7 PRIVATE "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/ULTIMA7.map")
target_include_directories(u7 PRIVATE
    "${U7_UPSTREAM_DIR}/platform"
    "${U7_UPSTREAM_DIR}/platform/matoya")   # vgafont8.h
target_compile_options(u7 PRIVATE -Wall -Wextra)
option(U7_DEBUG_LOG "Log frame statistics and input to the console" OFF)
if(U7_DEBUG_LOG)
    target_compile_definitions(u7 PRIVATE U7_DEBUG_LOG)
endif()
target_link_libraries(u7 PRIVATE u7_game)
set_target_properties(u7 PROPERTIES SUFFIX ".PRG")

# g++ always links -lm. The game uses no floating point, so when the toolchain has no libm
# (fdlibm is a separate MiNT package) an empty archive stands in for it.
execute_process(COMMAND ${CMAKE_C_COMPILER} -m68020-60 -print-file-name=libm.a
    OUTPUT_VARIABLE U7_LIBM OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT IS_ABSOLUTE "${U7_LIBM}")
    set(U7_STUB_LIBDIR "${CMAKE_CURRENT_BINARY_DIR}/stublib")
    file(MAKE_DIRECTORY "${U7_STUB_LIBDIR}")
    execute_process(COMMAND ${CMAKE_AR} rc "${U7_STUB_LIBDIR}/libm.a")
    target_link_directories(u7 PRIVATE "${U7_STUB_LIBDIR}")
    message(STATUS "No libm for the target; linking an empty stand-in")
endif()

# PRG header flags (offset 22): fastload, load into TT-RAM, Malloc from TT-RAM. The 32 MB game
# arena and the code then live in fast RAM; the backend takes screen and DMA sound buffers
# from ST RAM explicitly with Mxalloc.
add_custom_command(TARGET u7 POST_BUILD
    COMMAND sh -c "printf '\\000\\000\\000\\007' | dd of='$<TARGET_FILE:u7>' bs=1 seek=22 conv=notrunc status=none"
    VERBATIM)

# Compile-time checks that structs read from the game's files keep the PC's layout here.
add_library(u7_layout_check OBJECT tests/layout_check.cpp)
target_link_libraries(u7_layout_check PRIVATE u7_game)
target_compile_options(u7_layout_check PRIVATE -fpermissive -w
    -include "${U7_UPSTREAM_DIR}/game/include/u7port.h")
add_dependencies(u7 u7_layout_check)
