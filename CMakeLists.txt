cmake_minimum_required(VERSION 3.20)
project(nextgen_chess_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

option(BUILD_TESTS "Build tests" ON)
option(USE_PEXT "Use BMI2 PEXT for magic bitboards" ON)

# Fathom (real Syzygy probing) - tbprobe.c #includes tbchess.c
add_library(fathom STATIC tb/tbprobe.c)
target_include_directories(fathom PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tb)

add_library(chesscore
    core/bitboard.cpp
    core/position.cpp
    core/movegen.cpp
    core/zobrist.cpp
    search/search.cpp
    search/tt.cpp
    search/heuristics.cpp
    evaluation/eval.cpp
    evaluation/stockfish_nnue.cpp
    nnue/nnue.cpp
    uci/uci.cpp
    logging/game_log.cpp
    tb/syzygy.cpp
    opening/book.cpp
)

target_include_directories(chesscore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(chesscore PUBLIC fathom Threads::Threads)

find_package(Threads REQUIRED)

if(USE_PEXT)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-mbmi2" HAS_BMI2)
  if(HAS_BMI2)
    target_compile_options(chesscore PUBLIC -mbmi2)
    target_compile_definitions(chesscore PUBLIC USE_PEXT)
  endif()
endif()

target_compile_options(chesscore PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra>
)

add_executable(chess_engine main.cpp)
target_link_libraries(chess_engine PRIVATE chesscore)

if(BUILD_TESTS)
  enable_testing()
  add_executable(engine_tests tests/test_main.cpp)
  target_link_libraries(engine_tests PRIVATE chesscore)
  add_test(NAME engine_tests COMMAND engine_tests)
endif()
