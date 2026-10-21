cmake_minimum_required(VERSION 3.20)
project(ecstatica C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(C_SRCS
    init.c
    asm_f.c
    display.c
    debug_overlay.c
    edit.c
    game.c
    file.c
    move.c
    menu.c
    req.c
    music.c
    topo.c
    map.c
    anim.c
    ellipse.c
    tri.c
    icon.c
    tools/viewer.c
    tools/pointcloud.c
    win.c
    chars.c
    main.c
    render.c
    prefetch.c
    raster.c
    render_gl.c
    gl_loader.c
)

list(APPEND C_SRCS platforms/desktop_common.c platforms/threads.c
     platforms/third_party/zstddeclib.c)

if(APPLE)
    enable_language(OBJC)
    list(APPEND C_SRCS platforms/macos.m)
elseif(WIN32)
    list(APPEND C_SRCS platforms/windows.c)
elseif(UNIX)
    list(APPEND C_SRCS platforms/linux.c)
endif()

add_executable(ecstatica ${C_SRCS})

target_include_directories(ecstatica PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(ecstatica PRIVATE /W3 /std:c17)
    target_compile_definitions(ecstatica PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(ecstatica PRIVATE
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fwrapv
    )
    if(APPLE)
        target_compile_options(ecstatica PRIVATE
            $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>
        )
        # OpenGL has been deprecated on macOS since 10.14 and still works. The
        # renderer is confined to render_gl.c plus five platform functions
        # precisely so that a Metal backend would be a replacement rather than
        # a rewrite; until then the warning says nothing actionable.
        target_compile_definitions(ecstatica PRIVATE GL_SILENCE_DEPRECATION)
    endif()
endif()

target_compile_definitions(ecstatica PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
    ENABLE_FRAME_DUMP
    ECS_WIDESCREEN
)

# Vendored zstd decoder (Views/Fallback); its own warnings are not ours.
if(NOT MSVC)
    set_source_files_properties(platforms/third_party/zstddeclib.c
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

# The hardware renderer. Only this build defines ECS_ENABLE_GL; dos/, pocket/
# and psp/ compile the same sources with it undefined, which reduces
# render_gl.c and gl_loader.c to empty translation units.
#
# Not REQUIRED: a machine without the GL development files should still get a
# working software build rather than a configure error, so the feature is
# switched off instead. ECS_HARDWARE_RENDERER=OFF forces that by hand.
option(ECS_HARDWARE_RENDERER "Build the OpenGL 3.3 renderer" ON)
find_package(OpenGL)
if(ECS_HARDWARE_RENDERER AND OPENGL_FOUND)
    target_compile_definitions(ecstatica PRIVATE ECS_ENABLE_GL)
    target_link_libraries(ecstatica PRIVATE OpenGL::GL)
    message(STATUS "Hardware renderer: OpenGL 3.3 enabled")
else()
    message(STATUS "Hardware renderer: disabled (software renderer only)")
endif()

# backtrace() lives in libc on glibc and macOS; musl has no execinfo.h.
if(NOT WIN32)
    target_compile_definitions(ecstatica PRIVATE HAVE_EXECINFO)
endif()

if(APPLE)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    find_library(AUDIOUNIT_FRAMEWORK AudioUnit REQUIRED)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
    find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
    find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
    find_library(GAMECONTROLLER_FRAMEWORK GameController REQUIRED)

    target_link_libraries(ecstatica PRIVATE
        ${COCOA_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${AUDIOUNIT_FRAMEWORK}
        ${AUDIOTOOLBOX_FRAMEWORK}
        ${COREAUDIO_FRAMEWORK}
        ${AVFOUNDATION_FRAMEWORK}
        ${GAMECONTROLLER_FRAMEWORK}
        m
    )
elseif(WIN32)
    target_link_libraries(ecstatica PRIVATE
        gdi32
        user32
        winmm
        xinput
    )
elseif(UNIX)
    find_package(X11 REQUIRED)
    find_library(ALSA_LIB asound REQUIRED)

    target_include_directories(ecstatica PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(ecstatica PRIVATE
        ${X11_LIBRARIES}
        ${ALSA_LIB}
        ${CMAKE_DL_LIBS}
        pthread
        m
    )
endif()

set_target_properties(ecstatica PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
