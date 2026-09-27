cmake_minimum_required(VERSION 3.20)
project(isorogue C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(SOURCES
    src/main.c
    src/dungeon.c
    src/game.c
    src/render.c
    src/input.c
    src/audio.c
)

add_executable(isorogue ${SOURCES})

if(EMSCRIPTEN)
    # SDL3 comes from the Emscripten ports system (downloaded and built on first use).
    set(EM_FLAGS "--use-port=sdl3")
    target_compile_options(isorogue PRIVATE ${EM_FLAGS})
    target_link_options(isorogue PRIVATE
        ${EM_FLAGS}
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=32MB
        -sENVIRONMENT=web
        --shell-file ${CMAKE_SOURCE_DIR}/web/shell.html
    )
    set_target_properties(isorogue PROPERTIES
        OUTPUT_NAME "index"
        SUFFIX ".html"
        LINK_DEPENDS ${CMAKE_SOURCE_DIR}/web/shell.html
    )
else()
    # Prefer an installed SDL3; otherwise fetch it (over SSH) and build it statically.
    find_package(SDL3 CONFIG QUIET)
    if(NOT SDL3_FOUND)
        include(FetchContent)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL3
            GIT_REPOSITORY git@github.com:libsdl-org/SDL.git
            GIT_TAG release-3.2.24
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(SDL3)
    endif()
    target_link_libraries(isorogue PRIVATE SDL3::SDL3)
    if(NOT MSVC)
        target_compile_options(isorogue PRIVATE -Wall -Wextra -Wno-unused-parameter)
        target_link_libraries(isorogue PRIVATE m)
    endif()
endif()
