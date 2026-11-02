# ctest (from the build directory): `ctest --output-on-failure`
#
# ve_utils.hpp: one executable per ve:: target (ve.hpp picks its backend from
# the -m flags), so every target's bit casts and shifts are compiled and
# checked; a target this CPU can't run reports itself skipped
set(VE_TARGETS
    "sse3|-msse3"
    "sse4|-msse4.2"
    "avx|-mavx"
    "avx2|-mavx2,-mfma"
    "avx512|-mavx512f,-mavx512bw,-mavx512dq,-mavx512vl,-mavx2,-mfma"
)
foreach(entry IN LISTS VE_TARGETS)
    # name|flag,flag...
    string(REPLACE "|" ";" parts "${entry}")
    string(REPLACE "," ";" parts "${parts}")
    list(GET parts 0 name)
    list(SUBLIST parts 1 -1 flags)
    set(exe "test_ve_${name}")
    add_executable(${exe} ve.cpp)
    target_include_directories(${exe} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${exe} PRIVATE dependency_DataContainer)
    # no parallel loops here: ve.hpp needs no TBB then
    target_compile_definitions(${exe} PRIVATE VE_NO_TBB "VE_TEST_TARGET=\"${name}\"")
    target_compile_options(${exe} PRIVATE -Wall -Wextra ${flags})
    add_test(NAME ve_utils_${name} COMMAND ${exe})
    set_tests_properties(ve_utils_${name} PROPERTIES SKIP_RETURN_CODE 77 LABELS ve)
endforeach()

# the game's code (rwcpp_core) under test. test_support: what both share.
# (an OBJECT library's objects only reach what links it directly: each
# executable links both)
add_library(rwcpp_test_support OBJECT support.cpp)
target_link_libraries(rwcpp_test_support PUBLIC rwcpp_core)
target_include_directories(rwcpp_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(rwcpp_test_support PRIVATE -Wall -Wextra -Wno-unused-parameter)

# the scripted scenarios in synthetic rooms (no game data)
add_executable(rwcpp_selftest selftest.cpp)
target_link_libraries(rwcpp_selftest PRIVATE rwcpp_test_support rwcpp_core)
target_compile_options(rwcpp_selftest PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME selftest COMMAND rwcpp_selftest)
set_tests_properties(selftest PROPERTIES LABELS sim)

# the soaks and the bench on the game's world: RWCPP_GAME_ROOTS (each a
# --root, first has priority; by default the repository's game/ when it's
# there), else $RW_ROOT or the Steam install; skipped without any.
# They step the whole world for minutes, so ctest only has them with
# -DRWCPP_DATA_TESTS=ON (then `ctest -L data` runs just them, `-LE data`
# leaves them out); rwcpp_check is built either way
set(default_roots "")
if(EXISTS "${PROJECT_SOURCE_DIR}/game/world/regions.txt")
    if(EXISTS "${PROJECT_SOURCE_DIR}/game/mergedmods")
        list(APPEND default_roots "${PROJECT_SOURCE_DIR}/game/mergedmods")
    endif()
    list(APPEND default_roots "${PROJECT_SOURCE_DIR}/game")
endif()
set(RWCPP_GAME_ROOTS "${default_roots}" CACHE STRING "StreamingAssets-like roots for the data tests (a ; list)")
set(root_args "")
foreach(r IN LISTS RWCPP_GAME_ROOTS)
    list(APPEND root_args --root "${r}")
endforeach()
add_executable(rwcpp_check tools.cpp)
target_link_libraries(rwcpp_check PRIVATE rwcpp_test_support rwcpp_core)
target_compile_options(rwcpp_check PRIVATE -Wall -Wextra -Wno-unused-parameter)
option(RWCPP_DATA_TESTS "register the slow world tests (fuzz, bench, npc soak) with ctest" OFF)
if(RWCPP_DATA_TESTS)
    add_test(NAME fuzz COMMAND rwcpp_check ${root_args} --fuzz)
    add_test(NAME bench COMMAND rwcpp_check ${root_args} --bench)
    add_test(NAME npc_soak COMMAND rwcpp_check ${root_args} --npc-soak)
    set_tests_properties(fuzz bench npc_soak PROPERTIES LABELS data SKIP_RETURN_CODE 77 TIMEOUT 3600)
endif()
