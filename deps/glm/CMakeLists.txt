message(STATUS "Fetching glm...")
FetchContent_Declare(
	glm
	GIT_REPOSITORY https://github.com/g-truc/glm.git
	GIT_TAG 1.0.2
	GIT_SHALLOW 1
	GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(glm)
add_library(dependency_glm INTERFACE)
target_include_directories(dependency_glm SYSTEM INTERFACE "${glm_SOURCE_DIR}")
target_link_libraries(dependency_glm INTERFACE glm::glm)
# zero initialised vectors, movement code relies on value semantics like Unity's
target_compile_definitions(dependency_glm INTERFACE GLM_FORCE_CTOR_INIT)
