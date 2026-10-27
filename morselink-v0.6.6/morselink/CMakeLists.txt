cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Only build what we use (keeps build times down).
idf_build_set_property(MINIMAL_BUILD ON)
project(morselink)
