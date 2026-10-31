cmake_minimum_required(VERSION 3.20)
project(u7atari VERSION 0.1.0 LANGUAGES C CXX ASM)

# Which game: "Black Gate" or "Serpent Isle" (both ultima7-portable trees share plat.h).
set(U7_GAME "Black Gate" CACHE STRING "Upstream ultima7-portable game to build")
set(U7_UPSTREAM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/upstream/ultima7-portable/${U7_GAME}")
if(NOT EXISTS "${U7_UPSTREAM_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Missing ${U7_UPSTREAM_DIR}; run: git submodule update --init")
endif()

# Cross-compiled, this builds the Atari game. Built for the PC, it builds u7host instead: the
# same game with platform/host, the lockstep reference the Atari build is compared with
# (tools/lockstep-compare.sh). Build that 32-bit, as the Atari is:
#   cmake -B build-host -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32
if(CMAKE_CROSSCOMPILING)
    set(U7_HOST OFF)
else()
    set(U7_HOST ON)
endif()

# Upstream builds the game as the u7_game object library; we supply the backend instead of
# libmatoya, munt and Nuked OPL3.
set(U7_BACKEND_MATOYA OFF CACHE BOOL "" FORCE)
# The game's arena: upstream gives it 32 MB, of which the game uses about 2 MB (both games,
# measured under Hatari). 4 MB leaves room to spare and lets a 14 MB Falcon run from ST RAM.
add_compile_definitions("LINEAR_MEMORY_SIZE=(UINT32_C(4)<<20)")

if(U7_HOST)
    add_subdirectory("${U7_UPSTREAM_DIR}" game)
    file(GLOB U7_HOST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/common/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/host/*.c")
    add_executable(u7host ${U7_HOST_SOURCES} tests/save_test.cpp tests/speech_hook.cpp)
    if(U7_GAME STREQUAL "Serpent Isle")
        target_compile_definitions(u7host PRIVATE U7_SERPENT_ISLE)
    endif()
    target_compile_definitions(u7host PRIVATE U7_DEBUG_LOG)
    target_include_directories(u7host PRIVATE platform/common
        "${U7_UPSTREAM_DIR}/platform" "${U7_UPSTREAM_DIR}/platform/matoya")
    target_compile_options(u7host PRIVATE -Wall -Wextra)
    set_source_files_properties(tests/save_test.cpp tests/speech_hook.cpp PROPERTIES
        INCLUDE_DIRECTORIES "$<TARGET_PROPERTY:u7_game,INCLUDE_DIRECTORIES>"
        COMPILE_OPTIONS "$<TARGET_PROPERTY:u7_game,COMPILE_OPTIONS>;-w")
    target_link_libraries(u7host PRIVATE u7_game)
    return()
endif()

# The 030 reads and writes words and longs at any address, so let GCC do so: with its m68k
# default it copies and compares anything not known to be aligned a byte at a time.
add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-mno-strict-align>)
# No FPU code, so a Falcon without a 68882 runs the game too: the game has no floating point,
# but GCC saves the FPU registers around setjmp. main.c replaces MiNTLib's FPU check.
add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-msoft-float>)
# C++ type_info at even addresses despite upstream's #pragma pack(1): see the header.
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:--include=${CMAKE_CURRENT_SOURCE_DIR}/platform/atari/rtti_align.h>")
add_subdirectory("${U7_UPSTREAM_DIR}" game)
# Copy-mode shape spans are drawn by platform/atari/shapes.S.
target_compile_definitions(u7_game PRIVATE U7_SHAPE_SPANS_68K)

file(GLOB U7_ATARI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/common/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/atari/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/atari/*.S")
add_executable(u7 ${U7_ATARI_SOURCES})
if(U7_GAME STREQUAL "Serpent Isle")
    set(U7_PRG_NAME SERPENT)
    target_compile_definitions(u7 PRIVATE U7_SERPENT_ISLE)
else()
    set(U7_PRG_NAME ULTIMA7)
endif()
set_target_properties(u7 PROPERTIES OUTPUT_NAME ${U7_PRG_NAME})
# A link map, to turn addresses from Hatari's debugger into function names.
target_link_options(u7 PRIVATE "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${U7_PRG_NAME}.map")
target_include_directories(u7 PRIVATE
    platform/common
    platform/atari
    "${U7_UPSTREAM_DIR}/platform"
    "${U7_UPSTREAM_DIR}/platform/matoya")   # vgafont8.h
target_compile_options(u7 PRIVATE -Wall -Wextra)
option(U7_DEBUG_LOG "Log frame statistics and input to the console" OFF)
if(U7_DEBUG_LOG)
    target_compile_definitions(u7 PRIVATE U7_DEBUG_LOG)
    # --program savetest: the save files' byte order, against a GAMEDAT from the DOS game
    # and the script step "speech N" (tests/speech_hook.cpp)
    target_sources(u7 PRIVATE tests/save_test.cpp tests/speech_hook.cpp)
    set_source_files_properties(tests/save_test.cpp tests/speech_hook.cpp PROPERTIES
        INCLUDE_DIRECTORIES "$<TARGET_PROPERTY:u7_game,INCLUDE_DIRECTORIES>"
        COMPILE_OPTIONS "$<TARGET_PROPERTY:u7_game,COMPILE_OPTIONS>;-w")
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
    # The TOS loader refuses a fixup at an odd address; Hatari's GEMDOS drive does not check.
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/prg-relocs.py" "$<TARGET_FILE:u7>"
    VERBATIM)

# Compile-time checks that structs read from the game's files keep the PC's layout here.
add_library(u7_layout_check OBJECT tests/layout_check.cpp)
target_link_libraries(u7_layout_check PRIVATE u7_game)
target_compile_options(u7_layout_check PRIVATE -fpermissive -w
    -include "${U7_UPSTREAM_DIR}/game/include/u7port.h")
add_dependencies(u7 u7_layout_check)

# tests/shapes_test.cpp: shapes.S against the game's C shape drawing, run on the Atari or under
# Hatari (cmake --build build --target shapes_test).
add_executable(shapes_test EXCLUDE_FROM_ALL tests/shapes_test.cpp
    "${U7_UPSTREAM_DIR}/game/main/xmm/shapedraw.cpp"
    platform/atari/shapes.S)
set_target_properties(shapes_test PROPERTIES SUFFIX ".PRG")
target_include_directories(shapes_test PRIVATE $<TARGET_PROPERTY:u7_game,INCLUDE_DIRECTORIES>)
target_compile_options(shapes_test PRIVATE $<TARGET_PROPERTY:u7_game,COMPILE_OPTIONS>)
target_link_directories(shapes_test PRIVATE $<TARGET_PROPERTY:u7,LINK_DIRECTORIES>)
add_custom_command(TARGET shapes_test POST_BUILD
    COMMAND sh -c "printf '\\000\\000\\000\\007' | dd of='$<TARGET_FILE:shapes_test>' bs=1 seek=22 conv=notrunc status=none"
    VERBATIM)

# tests/c2p_frame_test.c: c2p.S against c2p.h, on the Atari or under Hatari.
add_executable(c2p_frame_test EXCLUDE_FROM_ALL tests/c2p_frame_test.c platform/atari/c2p.S)
set_target_properties(c2p_frame_test PROPERTIES SUFFIX ".PRG")
target_include_directories(c2p_frame_test PRIVATE platform/atari)
target_compile_options(c2p_frame_test PRIVATE -Wall -Wextra)
target_link_directories(c2p_frame_test PRIVATE $<TARGET_PROPERTY:u7,LINK_DIRECTORIES>)
