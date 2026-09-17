cmake_minimum_required(VERSION 3.20)
project(NavigaMer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
option(NAVIGAMER_NATIVE "Tune optimized builds for the current CPU" OFF)

add_library(navigamer_core
  src/sequence_store.cpp
  src/edit_distance.cpp
  src/index.cpp
  src/builder.cpp
  src/query.cpp
)
target_include_directories(navigamer_core PUBLIC include)
target_link_libraries(navigamer_core PUBLIC Threads::Threads)
target_compile_options(navigamer_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)
if(NAVIGAMER_NATIVE)
  target_compile_options(navigamer_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-march=native>
  )
endif()

add_executable(navigamer src/main.cpp)
target_link_libraries(navigamer PRIVATE navigamer_core)

include(CTest)
if(BUILD_TESTING)
  add_executable(navigamer_tests tests/test_navigamer.cpp)
  target_link_libraries(navigamer_tests PRIVATE navigamer_core)
  target_compile_options(navigamer_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-UNDEBUG>
  )
  add_test(NAME navigamer_tests COMMAND navigamer_tests ${CMAKE_CURRENT_BINARY_DIR}/test-data)
  add_test(NAME navigamer_cli_test COMMAND ${CMAKE_COMMAND}
    -DNAVIGAMER=$<TARGET_FILE:navigamer>
    -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-test-data
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.cmake)
endif()
