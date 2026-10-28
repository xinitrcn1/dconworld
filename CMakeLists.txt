cmake_minimum_required(VERSION 3.28)

project(rwcpp LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# no C++20 modules here, skip the dependency scanning step
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# This is developed on a weak machine: never compile with more than 2 jobs
# (honoured by the Ninja generator; with Makefiles pass -j2 yourself) and keep
# optimisation at -O2 rather than CMake's default -O3 for Release.
set_property(GLOBAL PROPERTY JOB_POOLS compile_pool=2 link_pool=1)
set(CMAKE_JOB_POOL_COMPILE compile_pool)
set(CMAKE_JOB_POOL_LINK link_pool)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

# Fetched dependencies (DataContainer, TBB, glm, GLEW, stb), anthiel style
add_subdirectory(deps EXCLUDE_FROM_ALL)
add_subdirectory(ParserGenerator EXCLUDE_FROM_ALL)

# Window, input and the OpenGL context come from the system SDL3
find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3)
find_package(OpenGL REQUIRED)

set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

# Data container: the generator writes next to its input, so run it on a copy
# (with comments stripped, the generator's format has none)
add_custom_command(
    OUTPUT "${GENERATED_DIR}/dcon_generated.hpp" "${GENERATED_DIR}/dcon_generated_ids.hpp"
    COMMAND ${CMAKE_COMMAND} -DIN=${PROJECT_SOURCE_DIR}/src/dcon_generated.txt -DOUT=${GENERATED_DIR}/dcon_generated.txt -P "${PROJECT_SOURCE_DIR}/cmake/strip_comments.cmake"
    COMMAND DCONGENERATOR "${GENERATED_DIR}/dcon_generated.txt"
    DEPENDS DCONGENERATOR "${PROJECT_SOURCE_DIR}/src/dcon_generated.txt" "${PROJECT_SOURCE_DIR}/cmake/strip_comments.cmake"
    COMMENT "Generating data container from dcon_generated.txt"
    VERBATIM
)

# Build time parser generation (see src/parser_defs.txt)
add_custom_command(
    OUTPUT "${GENERATED_DIR}/parser_defs_generated.hpp"
    COMMAND ParserGenerator "${PROJECT_SOURCE_DIR}/src/parser_defs.txt" "${GENERATED_DIR}/parser_defs_generated.hpp"
    DEPENDS ParserGenerator "${PROJECT_SOURCE_DIR}/src/parser_defs.txt"
    COMMENT "Generating parsers from parser_defs.txt"
    VERBATIM
)

add_executable(rwcpp
    src/main.cpp
    src/dcon.cpp
    src/layered_fs.cpp
    src/parsers.cpp
    src/world.cpp
    src/physics.cpp
    src/player.cpp
    src/player_graphics.cpp
    src/render.cpp
    src/selftest.cpp
    src/shortcuts.cpp
    src/items.cpp
    src/gates.cpp
    src/simulation.cpp
    src/audio.cpp
    src/sound_ids.cpp
    src/codecs.cpp
    src/unity_bundle.cpp
    src/fsb5.cpp
    "${GENERATED_DIR}/dcon_generated.hpp"
    "${GENERATED_DIR}/dcon_generated_ids.hpp"
    "${GENERATED_DIR}/parser_defs_generated.hpp"
)
target_include_directories(rwcpp PRIVATE "${PROJECT_SOURCE_DIR}/src")
# generated code is treated like a third party header (no warnings)
target_include_directories(rwcpp SYSTEM PRIVATE "${GENERATED_DIR}")
target_link_libraries(rwcpp PRIVATE
    dependency_DataContainer
    dependency_tbb
    dependency_glm
    dependency_glew
    dependency_stb
    dependency_dr_libs
    dependency_vgmstream_fsb
    SDL3::SDL3
    OpenGL::GL
)
target_compile_options(rwcpp PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-rtti -fno-exceptions)

# our own assets (HUD font, shaders, slugcat atlas) are looked up next to the executable
add_custom_command(TARGET rwcpp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:rwcpp>/assets"
    VERBATIM
)
