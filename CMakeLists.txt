cmake_minimum_required(VERSION 3.24)
project(wasm-migrate-commands C)

set(CMAKE_C_STANDARD 11)

add_executable(create_command create_main.c)
add_executable(start_command start_main.c)
add_executable(migrate_command migrate_main.c)
add_library(shared_object shared_object.c)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/wasmtime/crates/c-api ${CMAKE_CURRENT_BINARY_DIR}/wasmtime)

target_include_directories(create_command PUBLIC wasmtime)
target_link_libraries(create_command PUBLIC wasmtime)

