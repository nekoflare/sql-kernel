cmake_minimum_required(VERSION 3.16)
project(sql_os CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The SQL front end (lexer/parser/AST/printer) lives in the sibling
# sql-parser project; reuse it as a library.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../sql-parser sql-parser)

# Validator + codegen library.
add_library(sqlos
    src/catalog.cpp
    src/positions.cpp
    src/validate.cpp
    src/codegen.cpp
)
target_include_directories(sqlos PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sqlos PUBLIC sql_parser)
target_compile_options(sqlos PRIVATE -Wall -Wextra)

# CLI: sqlos [--check] [FILE]
add_executable(sqlos_cli src/main.cpp)
target_link_libraries(sqlos_cli PRIVATE sqlos)
target_compile_options(sqlos_cli PRIVATE -Wall -Wextra)
set_target_properties(sqlos_cli PROPERTIES OUTPUT_NAME sqlos)

enable_testing()
add_executable(sqlos_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../sql-parser/tests/main.cpp
    tests/test_validate.cpp
    tests/test_codegen.cpp
)
target_compile_definitions(sqlos_tests PRIVATE
    SQLOS_TEST_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sqlos_tests PRIVATE sqlos)
target_include_directories(sqlos_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../sql-parser/tests)
target_compile_options(sqlos_tests PRIVATE -Wall -Wextra)
add_test(NAME sqlos_tests COMMAND sqlos_tests)
