cmake_minimum_required(VERSION 3.24)
project(migrating_wasm_linux C)

set(CMAKE_C_STANDARD 11)

add_executable(create_command create_main.c)

target_include_directories(create_command PUBLIC $ENV{HOME}/opt/wasmtime-v34.0.1-aarch64-linux-c-api/include/wasmtime)
target_link_directories(create_command PUBLIC $ENV{HOME}/opt/wasmtime-v34.0.1-aarch64-linux-c-api/include/wasmtime)
target_include_directories(create_command PUBLIC $ENV{HOME}/opt/wasmtime-v34.0.1-aarch64-linux-c-api/include/)
target_link_directories(create_command PUBLIC $ENV{HOME}/opt/wasmtime-v34.0.1-aarch64-linux-c-api/include/)

include_directories("$ENV{HOME}/opt/wasmtime-v34.0.1-aarch64-linux-c-api/include/wasmtime")
include_directories("$ENV{HOME}/opt/wasmtime-v34.0.1-aarch64-linux-c-api/include")
