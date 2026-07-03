cmake_minimum_required(VERSION 3.10)

project(at_injector_plugin LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets LinguistTools)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_library(LIBCAP_STATIC_PATH NAMES libcap.a cap REQUIRED)
endif()

set(COMMON_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)

add_library(libat-injector SHARED
    src/plugin_main.h src/plugin_main.cpp
    src/plugin_worker.h src/plugin_worker.cpp
    ${COMMON_INCLUDE_DIR}/ipc_pipe.h
    ${COMMON_INCLUDE_DIR}/plugininterface.h
)

target_include_directories(libat-injector PRIVATE
    ${COMMON_INCLUDE_DIR}
    ${LIBCAP_INCLUDE_DIRS}
)

target_link_libraries(libat-injector PRIVATE Qt6::Core Qt6::Widgets ${LIBCAP_STATIC_PATH})

qt_add_translations(libat-injector
    TS_FILES
        translations/libat-injector_en_US.ts
        translations/libat-injector_ru_RU.ts
)

set_target_properties(libat-injector PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${PLUGINS_OUTPUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${PLUGINS_OUTPUT_DIR}
    OUTPUT_NAME "libat-injector"
    PREFIX ""
)
