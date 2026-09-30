cmake_minimum_required(VERSION 3.20)

project(CHGame_Emulator LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# The machine itself: CPU, peripherals, display, buzzer. No SDL in here, so
# the headless tools and the Emscripten build share it unchanged.
# ---------------------------------------------------------------------------
add_library(chgame_core STATIC
    src/rv32.c
    src/bus.c
    src/machine.c
    src/st7735.c
    src/audio.c
    src/loader.c
    src/bootloader_image.c
)
target_include_directories(chgame_core PUBLIC src)

if(NOT MSVC)
    target_compile_options(chgame_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options(chgame_core PRIVATE -O3)
    endif()
endif()
if(NOT WIN32 AND NOT EMSCRIPTEN)
    target_link_libraries(chgame_core PUBLIC m)
endif()

# ---------------------------------------------------------------------------
# The emulator with its SDL3 front end
#
# Natively SDL3 is found as an installed package (static when there is one,
# so the Windows exe runs on its own). Under Emscripten SDL3 comes from
# emscripten's own port.
# ---------------------------------------------------------------------------
add_executable(CHGame_Emulator src/main.c)
target_link_libraries(CHGame_Emulator PRIVATE chgame_core)

if(EMSCRIPTEN)
    set_target_properties(CHGame_Emulator PROPERTIES SUFFIX ".html")
    target_compile_options(CHGame_Emulator PRIVATE -sUSE_SDL=3)
    target_link_options(CHGame_Emulator PRIVATE
        -sUSE_SDL=3
        -sALLOW_MEMORY_GROWTH=1
        -sFORCE_FILESYSTEM=1
        -lidbfs.js
        -sEXPORTED_RUNTIME_METHODS=FS,ccall
        --shell-file ${PROJECT_SOURCE_DIR}/web/shell.html
    )
    target_compile_options(chgame_core PRIVATE -O3)
else()
    find_package(SDL3 REQUIRED CONFIG)
    if(TARGET SDL3::SDL3-static AND MINGW)
        target_link_libraries(CHGame_Emulator PRIVATE SDL3::SDL3-static)
        target_link_options(CHGame_Emulator PRIVATE -static-libgcc -static)
    else()
        target_link_libraries(CHGame_Emulator PRIVATE SDL3::SDL3)
    endif()
    if(NOT MSVC)
        target_compile_options(CHGame_Emulator PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    if(WIN32)
        set_target_properties(CHGame_Emulator PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tools and tests
# ---------------------------------------------------------------------------
if(NOT EMSCRIPTEN)
    add_executable(chg_headless tools/headless.c)
    target_link_libraries(chg_headless PRIVATE chgame_core)

    add_executable(xw_test tests/xw_test.c)
    target_link_libraries(xw_test PRIVATE chgame_core)

    enable_testing()
    add_test(NAME xw_decode COMMAND xw_test ${PROJECT_SOURCE_DIR}/tests/xw_golden.txt)
endif()
