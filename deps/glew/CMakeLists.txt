message(STATUS "Fetching GLEW...")

# glew-cmake links X11::Xext but GLEW never calls into Xext (it only needs
# GLX and the Xlib headers); stand in for it when libXext-devel is missing
find_package(X11)
if(NOT TARGET X11::Xext)
	add_library(X11::Xext INTERFACE IMPORTED)
endif()

set(glew-cmake_BUILD_SHARED OFF)
set(glew-cmake_BUILD_STATIC ON)
set(ONLY_LIBS ON)
FetchContent_Declare(
	glew
	GIT_REPOSITORY https://github.com/Perlmint/glew-cmake.git
	GIT_TAG d06782b910213d675925e6e51a69ad0fd1fe1f23
	GIT_PROGRESS TRUE
	EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(glew)

add_library(dependency_glew INTERFACE)
target_link_libraries(dependency_glew INTERFACE libglew_static)
target_include_directories(dependency_glew SYSTEM INTERFACE "${glew_SOURCE_DIR}/include")
