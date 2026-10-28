message(STATUS "Fetching dr_libs...")
# dr_mp3: the game's songs (music/songs/*.mp3)
FetchContent_Declare(
    dr_libs
    GIT_REPOSITORY https://github.com/mackron/dr_libs
    # pinned commit: a shallow clone cannot check out arbitrary hashes
    GIT_TAG a466dbe88fe2576d46c43877d602f3d66b8561a7
    GIT_PROGRESS TRUE
    # headers only: don't add the repository's own test/build scripts
    SOURCE_SUBDIR headers-only
)
FetchContent_MakeAvailable(dr_libs)
add_library(dependency_dr_libs INTERFACE)
target_include_directories(dependency_dr_libs SYSTEM INTERFACE "${dr_libs_SOURCE_DIR}")
