# The Atari Jaguar: marsdev's m68k-elf GCC (newlib, 68000) builds the game to run in place from
# cartridge ROM. Use with -DCMAKE_TOOLCHAIN_FILE=cmake/m68k-jaguar.cmake; MARSDEV names the
# toolchain if it is not in ~/dev/toolchains/mars.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR m68k)
if(NOT DEFINED ENV{MARSDEV})
    set(ENV{MARSDEV} "$ENV{HOME}/dev/toolchains/mars")
endif()
set(U7_JAGUAR_TOOLS "$ENV{MARSDEV}/m68k-elf/bin")
set(CMAKE_C_COMPILER "${U7_JAGUAR_TOOLS}/m68k-elf-gcc")
set(CMAKE_CXX_COMPILER "${U7_JAGUAR_TOOLS}/m68k-elf-g++")
set(CMAKE_ASM_COMPILER "${U7_JAGUAR_TOOLS}/m68k-elf-gcc")
set(CMAKE_OBJCOPY "${U7_JAGUAR_TOOLS}/m68k-elf-objcopy")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(U7_JAGUAR ON CACHE BOOL "Build for the Atari Jaguar" FORCE)
# newlib makes int32_t a long; the game (written for MiNTLib and the PC, where it is an int)
# then finds its int16_t/int32_t overloads ambiguous for an int. Both are 32 bits here.
set(U7_CPU_FLAGS "-m68000 -U__INT32_TYPE__ -D__INT32_TYPE__=int -U__UINT32_TYPE__")
set(CMAKE_C_FLAGS_INIT "${U7_CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${U7_CPU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${U7_CPU_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${U7_CPU_FLAGS}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
