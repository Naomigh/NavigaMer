cmake_minimum_required(VERSION 3.20)
project(NavigaMer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
  edlib
  GIT_REPOSITORY https://github.com/Martinsos/edlib.git
  GIT_TAG 42aa8fa7051fb2aa1c4f144c5eda22a0e753f026
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(edlib)

find_package(Threads REQUIRED)

add_library(navigamer_core
  src/sequence_store.cpp
  src/edit_distance.cpp
  src/metric_tree.cpp
  src/index.cpp
  src/builder.cpp
  src/query.cpp
)
target_include_directories(navigamer_core PUBLIC include)
target_link_libraries(navigamer_core PUBLIC edlib Threads::Threads)
target_compile_options(navigamer_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

add_executable(navigamer src/main.cpp)
target_link_libraries(navigamer PRIVATE navigamer_core)

include(CTest)
if(BUILD_TESTING)
  add_executable(navigamer_tests tests/test_navigamer.cpp)
  target_link_libraries(navigamer_tests PRIVATE navigamer_core)
  add_test(NAME navigamer_tests COMMAND navigamer_tests)
endif()
