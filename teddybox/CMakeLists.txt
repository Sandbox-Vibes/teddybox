cmake_minimum_required(VERSION 3.16)

# ESP-ADF needs jsmn, nghttp and zlib from the Espressif component registry.
# Normally the component manager fetches them; without access to the
# registry put them into a folder and set IDF_EXTRA_COMPONENTS (and
# IDF_COMPONENT_MANAGER=0), see setup_libs.sh.
set(EXTRA_COMPONENT_DIRS "components")
if(DEFINED ENV{IDF_EXTRA_COMPONENTS})
    list(APPEND EXTRA_COMPONENT_DIRS "$ENV{IDF_EXTRA_COMPONENTS}")
endif()
# only what main needs (and what that needs), not every ADF component
set(COMPONENTS main)
include($ENV{ADF_PATH}/CMakeLists.txt)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(teddybox)
