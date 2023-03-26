cmake_minimum_required(VERSION 3.13)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_CONFIGURATION_TYPES Debug Release)

project(open_video_matrix)

set_property(GLOBAL PROPERTY OS_FOLDERS ON)

add_subdirectory(src)
