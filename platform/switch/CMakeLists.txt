cmake_minimum_required(VERSION 3.20)
# Nintendo Switch (libnx) cross-build of the native runtime. Like the Vita
# target, the ROM importer and the three recompilers run on the HOST first;
# this build only compiles their generated C++ for AArch64 and links a .nro.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(NOT DEFINED ENV{DEVKITPRO})
        message(FATAL_ERROR "Set DEVKITPRO (e.g. /opt/devkitpro) first")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/Switch.cmake" CACHE FILEPATH "devkitPro Switch toolchain")
endif()
project(daytona_switch LANGUAGES C CXX)
if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "This directory requires the devkitPro Switch toolchain, not a desktop compiler")
endif()
get_filename_component(DAYTONA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(VITA_DIR "${DAYTONA_ROOT}/platform/vita") # shared SDL2 audio, controls, text, performance
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
# Same floating-point rules as every other target: the recompiled code relies on them.
add_compile_options(-Wall -Wextra -fno-fast-math -ffp-contract=off -ffunction-sections -fdata-sections)

option(DAYTONA_SWITCH_COMPILE_CHECK "Compile runtime and frontend without ROM-derived code; produces no NRO" OFF)
option(DAYTONA_SWITCH_DIAGNOSTICS "Routine startup/performance logging to the SD card (errors are always logged)" OFF)
option(DAYTONA_SWITCH_RENDER_OPT "Exact tile/layer caching and optimized CPU rasterizer (Vita OPT03)" ON)
set(DAYTONA_SWITCH_ROMSET "daytona93" CACHE STRING "ROM set the host build was recompiled from: daytona93 or daytona")
set_property(CACHE DAYTONA_SWITCH_ROMSET PROPERTY STRINGS daytona93 daytona)
if(NOT DAYTONA_SWITCH_ROMSET MATCHES "^(daytona93|daytona)$")
    message(FATAL_ERROR "DAYTONA_SWITCH_ROMSET must be daytona93 or daytona")
endif()
set(SET "${DAYTONA_SWITCH_ROMSET}")
set(DAYTONA_GEN_ROOT "${DAYTONA_ROOT}/build/gen" CACHE PATH "Host-generated i960, TGP and 68000 C++ root")
# Homebrew Menu icon: the app's own logo unless another 256x256 JPEG is given.
set(DAYTONA_SWITCH_ICON "" CACHE FILEPATH "256x256 JPEG icon for the Homebrew Menu (default: icon.jpg here)")
if(NOT DAYTONA_SWITCH_ICON)
    set(DAYTONA_SWITCH_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg")
endif()
# Homebrew Menu details (NACP).
set(DAYTONA_SWITCH_AUTHOR "Toniisound")
set(DAYTONA_SWITCH_VERSION "1.0")

if(NOT DAYTONA_SWITCH_COMPILE_CHECK)
    foreach(path ${SET}/gen_table.cpp ${SET}_tgp/tgp_gen.cpp ${SET}_snd/snd_gen.cpp)
        if(NOT EXISTS "${DAYTONA_GEN_ROOT}/${path}")
            message(FATAL_ERROR "Missing ${DAYTONA_GEN_ROOT}/${path}. Run scripts/recompile.py with the HOST compiler first; no game code ships in the repository.")
        endif()
    endforeach()
endif()

set(SOFTFLOAT_DIR "${DAYTONA_ROOT}/extern/softfloat")
set(YMFM_DIR "${DAYTONA_ROOT}/extern/ymfm/src")
foreach(path "${SOFTFLOAT_DIR}/source/softfloat_state.c" "${YMFM_DIR}/ymfm_opn.cpp")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Missing dependency ${path}. Run python3 scripts/setup.py --no-build on the host first.")
    endif()
endforeach()
include("${DAYTONA_ROOT}/cmake/softfloat_sources.cmake")
add_library(nx_softfloat STATIC ${SOFTFLOAT_SOURCES})
target_include_directories(nx_softfloat PUBLIC "${SOFTFLOAT_DIR}/source/include"
    PRIVATE "${VITA_DIR}/softfloat" "${SOFTFLOAT_DIR}/source/8086-SSE")
target_compile_definitions(nx_softfloat PUBLIC SOFTFLOAT_FAST_INT64 LITTLEENDIAN=1 THREAD_LOCAL=
    PRIVATE SOFTFLOAT_ROUND_ODD INLINE_LEVEL=5 SOFTFLOAT_FAST_DIV32TO16 SOFTFLOAT_FAST_DIV64TO32)
target_compile_options(nx_softfloat PRIVATE -w)

add_library(nx_i960 STATIC
    "${DAYTONA_ROOT}/src/i960/isa.cpp" "${DAYTONA_ROOT}/src/i960/decode.cpp"
    "${DAYTONA_ROOT}/src/i960/reach.cpp" "${DAYTONA_ROOT}/src/i960/fp.cpp")
target_include_directories(nx_i960 PUBLIC "${DAYTONA_ROOT}/src")
target_link_libraries(nx_i960 PUBLIC nx_softfloat)
add_library(nx_trace STATIC "${DAYTONA_ROOT}/src/trace/trace.cpp")
target_include_directories(nx_trace PUBLIC "${DAYTONA_ROOT}/src")
add_library(nx_ymfm STATIC "${YMFM_DIR}/ymfm_opn.cpp" "${YMFM_DIR}/ymfm_adpcm.cpp" "${YMFM_DIR}/ymfm_ssg.cpp")
target_include_directories(nx_ymfm SYSTEM PUBLIC "${YMFM_DIR}")
target_compile_options(nx_ymfm PRIVATE -w)

# The same native runtime as desktop and Vita; no refcore or interpreter.
set(RUNTIME_FILES cpu tgp m2_tgp_board geo raster video m2_board game_loop zip rom_import
    archive m2_replay_bus lockstep snd_cpu multipcm sound_board native_sample_mixer native_sound_sequencer native_sound_engine
    comm_board enhance)
set(RUNTIME_SOURCES)
foreach(name IN LISTS RUNTIME_FILES)
    list(APPEND RUNTIME_SOURCES "${DAYTONA_ROOT}/src/runtime/${name}.cpp")
endforeach()
add_library(nx_runtime STATIC ${RUNTIME_SOURCES})
target_include_directories(nx_runtime PUBLIC "${DAYTONA_ROOT}/src")
target_link_libraries(nx_runtime PUBLIC nx_trace nx_i960 nx_ymfm)
target_compile_definitions(nx_runtime PUBLIC M2_ROMSET="${SET}")
if(DAYTONA_SWITCH_RENDER_OPT)
    # Public because the flag changes Video/Raster class layouts.
    target_compile_definitions(nx_runtime PUBLIC M2_VITA_RENDER_OPT=1)
endif()

# SDL2 from devkitPro portlibs (pacman -S switch-sdl2). The toolchain file
# points pkg-config at the Switch portlibs, never at the host SDL2.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

set_source_files_properties(main.cpp PROPERTIES COMPILE_DEFINITIONS
    "DAYTONA_SWITCH_DIAGNOSTICS=$<BOOL:${DAYTONA_SWITCH_DIAGNOSTICS}>")
set(FRONTEND_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" "${VITA_DIR}" ${SDL2_STATIC_INCLUDE_DIRS} ${SDL2_INCLUDE_DIRS})
if(DAYTONA_SWITCH_COMPILE_CHECK)
    add_library(daytona_switch_frontend OBJECT main.cpp)
    # Our directory comes first so its diagnostic_log.h replaces the Vita one.
    target_include_directories(daytona_switch_frontend BEFORE PRIVATE ${FRONTEND_INCLUDES})
    target_link_libraries(daytona_switch_frontend PRIVATE nx_runtime)
    message(STATUS "ROM-free COMPILE CHECK only: no linked game or NRO will be produced")
else()
    file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${DAYTONA_GEN_ROOT}/${SET}/*.cpp")
    list(APPEND GEN_SOURCES "${DAYTONA_GEN_ROOT}/${SET}_tgp/tgp_gen.cpp" "${DAYTONA_GEN_ROOT}/${SET}_snd/snd_gen.cpp")
    set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label")
    add_executable(daytona_switch main.cpp ${GEN_SOURCES})
    target_include_directories(daytona_switch BEFORE PRIVATE ${FRONTEND_INCLUDES})
    target_link_libraries(daytona_switch PRIVATE nx_runtime ${SDL2_STATIC_LDFLAGS})
    target_link_options(daytona_switch PRIVATE -Wl,--gc-sections)

    nx_generate_nacp(daytona_switch.nacp
        NAME "Daytona Recomp NX"
        AUTHOR "${DAYTONA_SWITCH_AUTHOR}"
        VERSION "${DAYTONA_SWITCH_VERSION}")
    if(DAYTONA_SWITCH_ICON)
        nx_create_nro(daytona_switch NACP daytona_switch.nacp ICON "${DAYTONA_SWITCH_ICON}")
    else()
        nx_create_nro(daytona_switch NACP daytona_switch.nacp)
    endif()
endif()
