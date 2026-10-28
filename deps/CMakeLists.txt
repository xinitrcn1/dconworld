include(FetchContent)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
# some pinned dependencies predate CMake 3.5, which CMake 4 refuses by default
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

add_subdirectory(DataContainerGenerator)
add_subdirectory(tbb)
add_subdirectory(glm)
add_subdirectory(glew)
add_subdirectory(stb)
add_subdirectory(dr_libs)
add_subdirectory(vgmstream_fsb)
