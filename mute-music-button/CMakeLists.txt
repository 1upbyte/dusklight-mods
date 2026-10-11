cmake_minimum_required(VERSION 3.25)
project(mute_music_button LANGUAGES CXX)

set(DUSKLIGHT_VERSION "v2.0.1" CACHE STRING "Dusklight source version")
set(DUSKLIGHT_DIR "" CACHE PATH "Path to an existing Dusklight source checkout")

if(NOT DUSKLIGHT_DIR)
    include(FetchContent)
    FetchContent_Declare(dusklight
        GIT_REPOSITORY https://github.com/TwilitRealm/dusklight.git
        GIT_TAG ${DUSKLIGHT_VERSION}
        GIT_SHALLOW TRUE)
    FetchContent_GetProperties(dusklight)
    if(NOT dusklight_POPULATED)
        FetchContent_Populate(dusklight)
    endif()
    set(DUSKLIGHT_DIR "${dusklight_SOURCE_DIR}")
endif()
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusk-sdk EXCLUDE_FROM_ALL)

add_mod(mute_music_button
    FEATURES game
    SOURCES src/mod.cpp
    MOD_JSON mod.json)

# The audio setting is a game setting, rather than a Mod SDK service.
target_include_directories(mute_music_button PRIVATE "${DUSKLIGHT_DIR}/src")
