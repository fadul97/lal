cmake_minimum_required(VERSION 3.16)
project(lal)

# set(CMAKE_VERBOSE_MAKEFILE on )
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(include)
add_subdirectory(src)
add_subdirectory(examples)
