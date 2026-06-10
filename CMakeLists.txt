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

# Deploy to production after build (環境依存パスはコミットしない)
#   有効化: 環境変数 VMT_NUKE_DEPLOY_DIR を設定、または -DDEPLOY_DIR=... を渡す
if(NOT DEPLOY_DIR AND DEFINED ENV{VMT_NUKE_DEPLOY_DIR})
    set(DEPLOY_DIR "$ENV{VMT_NUKE_DEPLOY_DIR}")
endif()
if(WIN32 AND DEPLOY_DIR AND EXISTS "${DEPLOY_DIR}")
    get_target_property(_ndk_inc Nuke::NDK INTERFACE_INCLUDE_DIRECTORIES)
    if(_ndk_inc MATCHES "Nuke16")
        set(_VER_DIR "16.0")
    elseif(_ndk_inc MATCHES "Nuke15")
        set(_VER_DIR "15.1")
    else()
        set(_VER_DIR "")
    endif()
    if(_VER_DIR)
        add_custom_command(TARGET HexTile POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:HexTile>"
                "${DEPLOY_DIR}/${_VER_DIR}/$<TARGET_FILE_NAME:HexTile>"
            COMMENT "Deploying to ${DEPLOY_DIR}/${_VER_DIR}/"
        )
        message(STATUS "Auto-deploy enabled: ${DEPLOY_DIR}/${_VER_DIR}/")
    endif()
endif()
