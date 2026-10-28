message(STATUS "Fetching DataContainer...")

FetchContent_Declare(
	DataContainer
	GIT_REPOSITORY https://github.com/schombert/DataContainer
	GIT_TAG master
	GIT_SHALLOW 1
	GIT_PROGRESS TRUE
)
# there is no CMake project upstream, so this only fetches the sources
FetchContent_MakeAvailable(DataContainer)

add_library(dependency_DataContainer INTERFACE)
target_include_directories(dependency_DataContainer SYSTEM INTERFACE "${datacontainer_SOURCE_DIR}/CommonIncludes/")

# CONTAINER GENERATOR
file(GLOB_RECURSE DCONGENERATOR_SOURCE "${datacontainer_SOURCE_DIR}/DataContainerGenerator/*.cpp")

add_executable(DCONGENERATOR ${DCONGENERATOR_SOURCE})
target_include_directories(DCONGENERATOR PRIVATE "${datacontainer_SOURCE_DIR}/DataContainerGenerator/")
target_compile_features(DCONGENERATOR PRIVATE cxx_std_20)
# ve.hpp uses MSVC's PPL unless told to use oneTBB
target_compile_definitions(dependency_DataContainer INTERFACE PREFER_ONE_TBB)
