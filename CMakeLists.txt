cmake_minimum_required(VERSION 3.24...3.30)

project(
  service_record
  VERSION 0.0.1
  DESCRIPTION "C++ starter kit"
  HOMEPAGE_URL "https://github.com/shaulnv/cpp-starter-kit"
  LANGUAGES CXX C)

# project options
option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)
option(ENABLE_TEST_COVERAGE "Enable test coverage" OFF)
option(CTEST_OUTPUT_ON_FAILURE "On test failure, print out its full output" ON)
option(CMAKE_EXPORT_COMPILE_COMMANDS "Export compile commands" ON)

# set(CMAKE_CXX_STANDARD 17) # this is set at env/profiles/cpp.profile
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ${BUILD_SHARED_LIBS})

# cmake modules
include(CTest)
include(cmake/CPM.cmake)
include(cmake/PreventInSourceBuilds.cmake)
include(cmake/tools.cmake)
include(cmake/helpers.cmake)

# deps
# use CPM for import of internal cmake based projects
# use conanfile.py/requires for import of 3rd party projects
# Example:
# CPMAddPackage(
#   NAME ibis
#   GITHUB_REPOSITORY mellanox/ibis_tools
#   GIT_TAG release.v16.0.0
# )

# Create config.h from config.h.in
configure_file(./src/config.h.in ${PROJECT_BINARY_DIR}/service_record/config.h)
# Configure version.h
configure_file(./src/version.h.in ${PROJECT_BINARY_DIR}/service_record/version.h)

add_subdirectory(src)
add_subdirectory(cli)
add_subdirectory(tests)
