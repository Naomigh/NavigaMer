cmake_minimum_required(VERSION 3.20)
project(NavigaMer VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
option(NAVIGAMER_NATIVE "Tune optimized builds for the current CPU" OFF)
option(NAVIGAMER_ENABLE_LEAP "Use LEAP for small bounded edit distances" ON)
option(NAVIGAMER_ENABLE_BATCH_MYERS "Use runtime-dispatched AVX2 batch center distances" ON)
option(NAVIGAMER_ENABLE_ARCHIVE "Build disk-backed compressed archive support (requires Zstd)" ON)

add_library(edlib STATIC third_party/edlib/src/edlib.cpp)
target_include_directories(edlib PUBLIC third_party/edlib/include)

if(NAVIGAMER_ENABLE_LEAP AND
   CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
  set(LEAP_SOURCES
    third_party/leap/SIMD_ED.cc
    third_party/leap/SHD.cc
    third_party/leap/bit_convert.c
    third_party/leap/shift.c
    third_party/leap/mask.c
    third_party/leap/popcount.c
    third_party/leap/print.c
  )
  set_source_files_properties(${LEAP_SOURCES} PROPERTIES LANGUAGE CXX)
  add_library(leap STATIC ${LEAP_SOURCES})
  target_include_directories(leap PRIVATE third_party/leap)
  target_compile_options(leap PRIVATE -mavx2 -mbmi -mssse3)
endif()

add_library(navigamer_core
  src/sequence_store.cpp
  src/packed_centers.cpp
  src/edit_distance.cpp
  src/batch_distance.cpp
  src/member_classifier.cpp
  src/center_selector.cpp
  src/index.cpp
  src/builder.cpp
  src/query.cpp
)
target_include_directories(navigamer_core PUBLIC include)
target_link_libraries(navigamer_core PUBLIC edlib Threads::Threads)
if(NAVIGAMER_ENABLE_BATCH_MYERS)
  target_compile_definitions(navigamer_core PRIVATE NAVIGAMER_HAVE_BATCH_MYERS=1)
endif()
if(TARGET leap)
  target_sources(navigamer_core PRIVATE src/leap_distance.cpp)
  target_include_directories(navigamer_core PRIVATE third_party/leap)
  set_source_files_properties(src/leap_distance.cpp PROPERTIES
    COMPILE_OPTIONS "-mavx2;-mbmi;-mssse3"
  )
  target_link_libraries(navigamer_core PUBLIC leap)
  target_compile_definitions(navigamer_core PRIVATE NAVIGAMER_HAVE_LEAP=1)
endif()
target_compile_options(navigamer_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)
if(NAVIGAMER_NATIVE)
  target_compile_options(edlib PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-march=native>
  )
  target_compile_options(navigamer_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-march=native>
  )
endif()

add_executable(navigamer src/main.cpp)
target_link_libraries(navigamer PRIVATE navigamer_core)

if(NAVIGAMER_ENABLE_ARCHIVE)
  find_path(ZSTD_INCLUDE_DIR zstd.h)
  find_library(ZSTD_LIBRARY NAMES zstd)
  if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
    add_library(navigamer_archive src/archive_storage.cpp src/archive_builder.cpp)
    target_include_directories(navigamer_archive PRIVATE ${ZSTD_INCLUDE_DIR})
    target_link_libraries(navigamer_archive PUBLIC navigamer_core PRIVATE ${ZSTD_LIBRARY})
    target_compile_options(navigamer_archive PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
    target_link_libraries(navigamer PRIVATE navigamer_archive)
    target_compile_definitions(navigamer PRIVATE NAVIGAMER_HAVE_ARCHIVE=1)
  else()
    message(WARNING "Zstd headers/library not found: archive commands disabled. Set CMAKE_PREFIX_PATH or ZSTD_INCLUDE_DIR and ZSTD_LIBRARY to enable them.")
  endif()
endif()

include(CTest)
if(BUILD_TESTING)
  if(TARGET navigamer_archive)
    add_executable(navigamer_archive_tests tests/test_archive.cpp)
    target_include_directories(navigamer_archive_tests PRIVATE src ${ZSTD_INCLUDE_DIR})
    target_link_libraries(navigamer_archive_tests PRIVATE navigamer_archive ${ZSTD_LIBRARY})
    target_compile_options(navigamer_archive_tests PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-UNDEBUG>)
    add_test(NAME navigamer_archive_tests COMMAND navigamer_archive_tests ${CMAKE_CURRENT_BINARY_DIR}/archive-test-data)
    set_tests_properties(navigamer_archive_tests PROPERTIES FIXTURES_SETUP archive_data)
    add_test(NAME navigamer_archive_cli_test COMMAND ${CMAKE_COMMAND}
      -DNAVIGAMER=$<TARGET_FILE:navigamer>
      -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/archive-test-data
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/archive_cli_test.cmake)
    set_tests_properties(navigamer_archive_cli_test PROPERTIES FIXTURES_REQUIRED archive_data)
  endif()
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
  add_executable(navigamer_beacon_cover_tests tests/test_beacon_cover.cpp)
  target_link_libraries(navigamer_beacon_cover_tests PRIVATE navigamer_core)
  target_compile_options(navigamer_beacon_cover_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-UNDEBUG>
  )
  add_test(NAME navigamer_beacon_cover_tests COMMAND navigamer_beacon_cover_tests
    ${CMAKE_CURRENT_BINARY_DIR}/beacon-cover-test-data)
endif()
