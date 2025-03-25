enable_testing()
find_package(doctest REQUIRED)

add_executable(service_record-tests)
target_sources(service_record-tests PRIVATE ./src/main-tests.cpp ./src/service_record-test.cpp)
target_link_libraries(service_record-tests doctest::doctest service_record::service_record)
install_compile_commands_json(service_record-tests)
install(TARGETS service_record-tests RUNTIME DESTINATION tests)

include(doctest)

if(EMSCRIPTEN)
  message(STATUS "Building Emscripten tests")
  add_test(NAME service_record-tests COMMAND node ${CMAKE_CURRENT_BINARY_DIR}/service_record-tests.js)
else()
  message(STATUS "Building native tests")
  doctest_discover_tests(service_record-tests)
endif()

# enable compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  target_compile_options(service_record-tests PUBLIC -Wall -Wpedantic -Wextra -Werror)
elseif(MSVC)
  target_compile_options(service_record-tests PUBLIC /W4 /WX)
  target_compile_definitions(service_record-tests PUBLIC DOCTEST_CONFIG_USE_STD_HEADERS)
endif()

if(ENABLE_TEST_COVERAGE)
  target_compile_options(service_record-tests PUBLIC -O0 -g -fprofile-arcs -ftest-coverage)
  target_link_options(service_record-tests PUBLIC -fprofile-arcs -ftest-coverage)
endif()
