cmake_minimum_required(VERSION 3.10 FATAL_ERROR)
project(HexTile LANGUAGES CXX)

# Allow cmake/ to be found for NukeConfig.cmake
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHa")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} /OPT:NOREF")
endif()

# Locate Nuke NDK (sets Nuke::NDK target and add_nuke_plugin helper)
find_package(Nuke REQUIRED)

# Build the plugin
add_nuke_plugin(HexTile HexTile.cpp)
