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
add_subdirectory("${U7_UPSTREAM_DIR}" game)

file(GLOB U7_ATARI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/atari/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/atari/*.S")
add_executable(u7 ${U7_ATARI_SOURCES})
set_target_properties(u7 PROPERTIES OUTPUT_NAME ULTIMA7)
target_include_directories(u7 PRIVATE
    "${U7_UPSTREAM_DIR}/platform"
    "${U7_UPSTREAM_DIR}/platform/matoya")   # vgafont8.h
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
