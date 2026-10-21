#------------------------------------------------------------------------------
# Ecstatica — PlayStation Vita build
#------------------------------------------------------------------------------
#
#   cmake -S platforms/vita -B platforms/vita/build && cmake --build platforms/vita/build
#       → ecstatica-e1.vpk (ECST00001, "Ecstatica",    ux0:data/ecstatica/e1)
#       → ecstatica-e2.vpk (ECST00002, "Ecstatica II", ux0:data/ecstatica/e2)
#   cmake -S platforms/vita -B platforms/vita/build -DPROFILE=ON   → both, writing prof.log
#
# Needs VitaSDK with $VITASDK set. Like psp/ and dos/, this compiles ../../src
# directly; the whole platform surface is ../../src/platforms/vita.c.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "VITASDK is not set — install VitaSDK and export VITASDK")
    endif()
endif()

project(ecstatica C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

# APP_VER in param.sfo, "XX.YY". The Vita and its updaters compare it, so a
# release must raise it: GAME_VERSION v0.MINOR.PATCH (CI passes the tag)
# becomes MINOR.PATCH, v0.9.1 -> 09.01, v0.10.0 -> 10.00. Builds without it
# keep 01.00, below every release since 0.9. A 1.0 needs a new mapping.
set(GAME_VERSION "" CACHE STRING "Release version, e.g. v0.9.1")
set(VITA_VERSION "01.00")
if(GAME_VERSION MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(ver_minor ${CMAKE_MATCH_2})
    set(ver_patch ${CMAKE_MATCH_3})
    if(NOT CMAKE_MATCH_1 EQUAL 0 OR ver_minor GREATER 99 OR ver_patch GREATER 99)
        message(FATAL_ERROR "${GAME_VERSION} does not fit APP_VER XX.YY; change the mapping")
    endif()
    if(ver_minor LESS 10)
        set(ver_minor "0${ver_minor}")
    endif()
    if(ver_patch LESS 10)
        set(ver_patch "0${ver_patch}")
    endif()
    set(VITA_VERSION "${ver_minor}.${ver_patch}")
elseif(GAME_VERSION)
    message(WARNING "GAME_VERSION ${GAME_VERSION} is not vX.Y.Z; APP_VER stays ${VITA_VERSION}")
endif()
message(STATUS "Vita APP_VER ${VITA_VERSION}")

option(PROFILE "Frame-phase profiler writing prof.log" OFF)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
file(GLOB ENGINE_SRCS ${SRC_DIR}/*.c ${SRC_DIR}/tools/*.c)

# One VPK per game, each its own LiveArea bubble opening its own data folder.
# The engine is the same; only VITA_GAME_DIR and the package metadata differ.
function(ecstatica_vpk game title_id app_name)
    set(exe ecstatica_${game})
    add_executable(${exe} ${ENGINE_SRCS} ${SRC_DIR}/platforms/vita.c
                   ${SRC_DIR}/platforms/third_party/zstddeclib.c)
    target_include_directories(${exe} PRIVATE ${SRC_DIR})
    set_property(TARGET ${exe} PROPERTY C_STANDARD 99)
    # ECS_WIDESCREEN: the 16:9 mode for the re-release data, which fits the
    # 960x544 panel exactly.
    target_compile_definitions(${exe} PRIVATE VITA_GAME_DIR="${game}" ECS_WIDESCREEN)
    set_source_files_properties(${SRC_DIR}/platforms/third_party/zstddeclib.c
        PROPERTIES COMPILE_OPTIONS "-w")

    # -fsigned-char: `char` is unsigned on ARM but signed on the x86 the
    # original Watcom binary targeted, and the engine keeps signed data in
    # plain char arrays (shade_tab, shadow_tab, the bitmap planes).
    target_compile_options(${exe} PRIVATE
        -O2 -Wall -Wextra -fwrapv -fsigned-char
        -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare
        -Wno-unused-but-set-variable -Wno-char-subscripts)
    if(PROFILE)
        target_compile_definitions(${exe} PRIVATE ECS_PROFILE)
        target_link_options(${exe} PRIVATE
            -Wl,--wrap=sceIoOpen -Wl,--wrap=sceIoRead -Wl,--wrap=sceIoLseek
            -Wl,--wrap=sceIoWrite)
    endif()

    target_link_libraries(${exe}
        SceDisplay_stub
        SceCtrl_stub
        SceTouch_stub
        SceAudio_stub
        ScePower_stub
        m
    )

    set(sys sce_sys/${game})
    vita_create_self(eboot_${game}.bin ${exe} UNSAFE)
    vita_create_vpk(ecstatica-${game}.vpk ${title_id} eboot_${game}.bin
        VERSION ${VITA_VERSION}
        NAME "${app_name}"
        FILE ${sys}/icon0.png sce_sys/icon0.png
             ${sys}/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
             ${sys}/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
             ${sys}/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
    )
endfunction()

ecstatica_vpk(e1 ECST00001 "Ecstatica")
ecstatica_vpk(e2 ECST00002 "Ecstatica II")
