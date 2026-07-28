
cmake_minimum_required(VERSION 3.20)
project(nextgen_chess_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build tests" ON)
option(USE_NATIVE "Tune for the host CPU (-march=native)" ON)
option(USE_PEXT "Use BMI2 PEXT for slider indexing instead of magic multiply" OFF)

# A chess engine with no optimisation flags is not a chess engine. Default to
# Release so an unqualified `cmake ..` does not silently produce a -O0 binary.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

# Detect BMI2. PEXT is a win on Intel but was microcoded and slow on AMD Zen 1-2,
# so it stays opt-in rather than automatic.
include(CheckCXXSourceCompiles)
find_package(Threads REQUIRED)

add_library(chesscore
    core/bitboard.cpp
    core/position.cpp
    core/movegen.cpp
    core/zobrist.cpp
    search/search.cpp
    search/tt.cpp
    search/heuristics.cpp
    evaluation/eval.cpp
    nnue/nnue.cpp
    uci/uci.cpp
    logging/game_log.cpp
    tb/syzygy.cpp
    opening/book.cpp
)

target_include_directories(chesscore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(chesscore PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>
)

# popcount/ctz/pext must be enabled at the ISA level or __builtin_popcountll
# degrades to a libgcc call in the hottest loop in the engine.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
  if(USE_NATIVE)
    target_compile_options(chesscore PUBLIC -march=native)
  else()
    target_compile_options(chesscore PUBLIC -mpopcnt -msse4.2)
  endif()
endif()

if(USE_PEXT)
  target_compile_definitions(chesscore PUBLIC USE_PEXT)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    target_compile_options(chesscore PUBLIC -mbmi2)
  endif()
endif()

target_link_libraries(chesscore PUBLIC Threads::Threads)

add_executable(chess_engine main.cpp)
target_link_libraries(chess_engine PRIVATE chesscore)

add_executable(perft tests/perft_main.cpp)
target_link_libraries(perft PRIVATE chesscore)

if(BUILD_TESTS)
  enable_testing()
  add_test(NAME perft COMMAND perft)
  add_executable(engine_tests tests/test_main.cpp)
  target_link_libraries(engine_tests PRIVATE chesscore)
  add_test(NAME engine_tests COMMAND engine_tests)
endif()
