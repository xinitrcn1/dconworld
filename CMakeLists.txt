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

# Fetched dependencies (DataContainer, TBB, glm, stb), anthiel style
add_subdirectory(deps EXCLUDE_FROM_ALL)
add_subdirectory(ParserGenerator EXCLUDE_FROM_ALL)

# Window, input and the Vulkan surface come from the system SDL3; the Vulkan
# loader, headers and glslangValidator (the shaders' compiler) from the system
find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3)
find_package(Vulkan 1.3 REQUIRED COMPONENTS glslangValidator)

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

# Shaders (src/shaders): GLSL compiled to SPIR-V at build time, each wrapped
# into a C source + header in the build tree (generated/shaders/<name>_<stage>.c/h,
# defining <name>_<stage>_spv and <name>_<stage>_spv_size)
set(SHADER_DIR "${PROJECT_SOURCE_DIR}/src/shaders")
set(SHADER_OUT "${GENERATED_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT}")
set(SHADERS
    flat.vert flat.frag
    room.vert room.frag
    decal.vert decal.frag
    warp.vert warp_tear.frag warp_screen.frag water.frag
)
# #included by the shaders above
set(SHADER_INCLUDES "${SHADER_DIR}/warp_common.glsl")
set(SHADER_SOURCES "")
foreach(shader IN LISTS SHADERS)
    string(REPLACE "." "_" stem "${shader}")
    add_custom_command(
        OUTPUT "${SHADER_OUT}/${stem}.c" "${SHADER_OUT}/${stem}.h"
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 --quiet -o "${SHADER_OUT}/${stem}.spv" "${SHADER_DIR}/${shader}"
        COMMAND ${CMAKE_COMMAND} -DSPV=${SHADER_OUT}/${stem}.spv -DNAME=${stem}_spv -DOUT_C=${SHADER_OUT}/${stem}.c -DOUT_H=${SHADER_OUT}/${stem}.h -P "${PROJECT_SOURCE_DIR}/cmake/spirv_to_c.cmake"
        DEPENDS "${SHADER_DIR}/${shader}" ${SHADER_INCLUDES} "${PROJECT_SOURCE_DIR}/cmake/spirv_to_c.cmake"
        COMMENT "Compiling shader ${shader} to SPIR-V"
        VERBATIM
    )
    list(APPEND SHADER_SOURCES "${SHADER_OUT}/${stem}.c")
endforeach()
add_library(rwcpp_shaders STATIC ${SHADER_SOURCES})
target_include_directories(rwcpp_shaders PUBLIC "${SHADER_OUT}")

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
    src/gpu.cpp
    src/selftest.cpp
    src/shortcuts.cpp
    src/items.cpp
    src/outposts.cpp
    src/item_graphics.cpp
    src/gates.cpp
    src/piggyback.cpp
    src/simulation.cpp
    src/rain.cpp
    src/dens.cpp
    src/spawns.cpp
    src/pathing.cpp
    src/body_parts.cpp
    src/creature.cpp
    src/relationships.cpp
    src/tracker.cpp
    src/migrate.cpp
    src/npc.cpp
    src/centipede.cpp
    src/centipede_graphics.cpp
    src/dropbug.cpp
    src/dropbug_graphics.cpp
    src/cicada.cpp
    src/cicada_graphics.cpp
    src/lizard.cpp
    src/lizard_ai.cpp
    src/lizard_breeds.cpp
    src/lizard_graphics.cpp
    src/lizard_cosmetics.cpp
    src/daddy.cpp
    src/daddy_tentacle.cpp
    src/daddy_graphics.cpp
    src/leech.cpp
    src/leech_graphics.cpp
    src/tube_worm.cpp
    src/tube_worm_graphics.cpp
    src/needle_worm.cpp
    src/needle_worm_graphics.cpp
    src/pole_mimic.cpp
    src/pole_mimic_graphics.cpp
    src/tentacle_plant.cpp
    src/tentacle_plant_graphics.cpp
    src/jetfish.cpp
    src/jetfish_graphics.cpp
    src/scavenger.cpp
    src/scavenger_graphics.cpp
    src/deer.cpp
    src/deer_graphics.cpp
    src/puff_ball.cpp
    src/miros.cpp
    src/miros_graphics.cpp
    src/barnacle.cpp
    src/barnacle_graphics.cpp
    src/egg_bug.cpp
    src/egg_bug_graphics.cpp
    src/garbage_worm.cpp
    src/garbage_worm_graphics.cpp
    src/lantern_mouse.cpp
    src/lantern_mouse_graphics.cpp
    src/loach.cpp
    src/loach_graphics.cpp
    src/moth_grub.cpp
    src/moth_grub_graphics.cpp
    src/overseer.cpp
    src/overseer_graphics.cpp
    src/snail.cpp
    src/snail_graphics.cpp
    src/sky_whale.cpp
    src/sky_whale_graphics.cpp
    src/yeek.cpp
    src/yeek_graphics.cpp
    src/egg_bug_egg.cpp
    src/vulture.cpp
    src/vulture_tentacle.cpp
    src/vulture_tusks.cpp
    src/vulture_graphics.cpp
    src/worm_grass.cpp
    src/rot.cpp
    src/warps.cpp
    src/water.cpp
    src/particles.cpp
    src/explosions.cpp
    src/terrain.cpp
    src/audio.cpp
    src/sound_ids.cpp
    src/codecs.cpp
    src/unity_bundle.cpp
    src/fsb5.cpp
    src/unity_assets.cpp
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
    dependency_stb
    dependency_dr_libs
    dependency_vgmstream_fsb
    SDL3::SDL3
    Vulkan::Vulkan
    rwcpp_shaders
)
target_compile_options(rwcpp PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-rtti -fno-exceptions)

# our own assets (HUD font, slugcat atlas) are looked up next to the executable
add_custom_command(TARGET rwcpp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:rwcpp>/assets"
    VERBATIM
)
