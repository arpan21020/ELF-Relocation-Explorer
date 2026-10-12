cmake_minimum_required(VERSION 3.10)
project(elf_explorer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(elf-explorer
    src/main.cpp
    src/elf_parser.cpp
    src/relocation_analyzer.cpp
    src/layout.cpp
)
target_include_directories(elf-explorer PRIVATE include)
target_compile_options(elf-explorer PRIVATE -Wall -Wextra -Wpedantic)
