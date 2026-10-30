# Cross toolchain for Atari TT030 / Falcon030 (m68k-atari-mint-gcc, a.out/PRG output).
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/m68k-atari-mint.cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR m68k)

set(U7_MINT_PREFIX m68k-atari-mint- CACHE STRING "Cross tool prefix")
set(CMAKE_C_COMPILER   ${U7_MINT_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${U7_MINT_PREFIX}g++)
set(CMAKE_ASM_COMPILER ${U7_MINT_PREFIX}gcc)
set(CMAKE_AR           ${U7_MINT_PREFIX}ar)
set(CMAKE_RANLIB       ${U7_MINT_PREFIX}ranlib)

# 68020-60 code runs on the 030 in both machines and selects the matching multilib.
# This also selects MiNTLib's FPU multilib, whose startup refuses to run without a 68881/68882;
# the game replaces that check (_checkcpu in platform/atari/main.c) and builds with -msoft-float.
set(U7_CPU_FLAGS "-m68020-60 -mtune=68030")
set(CMAKE_C_FLAGS_INIT   "${U7_CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${U7_CPU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${U7_CPU_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${U7_CPU_FLAGS}")

set(CMAKE_EXECUTABLE_SUFFIX ".prg")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
