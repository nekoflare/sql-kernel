cmake_minimum_required(VERSION 3.16)
project(sql_parser CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(sql_parser
    src/lexer.cpp
    src/parser_expr.cpp
    src/parser_stmt.cpp
    src/printer.cpp
    src/ast.cpp
)
target_include_directories(sql_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(sql_parser PRIVATE -Wall -Wextra)

add_executable(sql_parse src/main.cpp)
target_link_libraries(sql_parse PRIVATE sql_parser)
target_compile_options(sql_parse PRIVATE -Wall -Wextra)

enable_testing()
add_executable(parser_tests
    tests/main.cpp
    tests/test_insert.cpp
    tests/test_lexer.cpp
    tests/test_expr.cpp
    tests/test_select.cpp
    tests/test_dml.cpp
    tests/test_ddl.cpp
)
target_link_libraries(parser_tests PRIVATE sql_parser)
target_compile_options(parser_tests PRIVATE -Wall -Wextra)
add_test(NAME parser_tests COMMAND parser_tests)
