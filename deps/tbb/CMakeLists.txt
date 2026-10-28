message(STATUS "Fetching TBB...")

set(TBB_DISABLE_HWLOC_AUTOMATIC_SEARCH ON)
set(TBB_TEST OFF)
set(TBB_EXAMPLES OFF)
set(TBB_BENCH OFF)
set(TBB_CPF OFF)
set(TBBMALLOC_BUILD OFF)
set(TBBMALLOC_PROXY_BUILD OFF)
set(TBB4PY_BUILD OFF)
set(TBB_SIGNTOOL OFF)
set(TBB_DYNAMIC_LOAD_ENABLED OFF)
set(TBB_ENABLE_IPO OFF)
set(TBB_STRICT OFF)

FetchContent_Declare(tbb
	GIT_REPOSITORY "https://github.com/oneapi-src/oneTBB"
	GIT_TAG v2021.12.0
	GIT_SHALLOW 1
	GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(tbb)

add_library(dependency_tbb INTERFACE)
target_link_libraries(dependency_tbb INTERFACE tbb)
