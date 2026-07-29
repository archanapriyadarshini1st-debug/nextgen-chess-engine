cmake_minimum_required(VERSION 3.20)
project(nextgen_chess_engine LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(BUILD_TESTS "Build tests" ON)

add_library(chesscore
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
target_compile_options(chesscore PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>)
add_executable(chess_engine main.cpp)
target_link_libraries(chess_engine PRIVATE chesscore)
if(BUILD_TESTS)
  enable_testing()
  add_executable(engine_tests tests/test_main.cpp)
  target_link_libraries(engine_tests PRIVATE chesscore)
  add_test(NAME engine_tests COMMAND engine_tests)
endif()
