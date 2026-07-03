cmake_minimum_required(VERSION 3.10)

project(at_imhhw_plugin LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(COMMON_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CONFIG_JSON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/metadata.json")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m32 -fPIC")
    set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -m32 -fPIC -Wa,--noexecstack")

    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/note_section.S.in
        ${CMAKE_CURRENT_BINARY_DIR}/resources/note_section.S
        @ONLY
    )
    set(ASM_FILE ${CMAKE_CURRENT_BINARY_DIR}/resources/note_section.S)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/resources/metadata.json" RAW_JSON_CONTENT)
        string(REGEX REPLACE "\n" " " RAW_JSON_CONTENT "${RAW_JSON_CONTENT}")
        string(REGEX REPLACE "[ ]+" " " RAW_JSON_CONTENT "${RAW_JSON_CONTENT}")
        string(REPLACE "\"" "\"\"" CONFIG_JSON_PATH "${RAW_JSON_CONTENT}")

    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/windows.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/resources/windows.rc
        @ONLY
    )
    set(RC_FILE ${CMAKE_CURRENT_BINARY_DIR}/resources/windows.rc)
endif()

add_library(libat-imhhw SHARED
    src/plugin_main.cpp
    src/config.h
    ${COMMON_INCLUDE_DIR}/memory_utils.cpp ${COMMON_INCLUDE_DIR}/memory_utils.h
    ${COMMON_INCLUDE_DIR}/ipc_pipe.h
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(libat-imhhw PRIVATE
        ${ASM_FILE}
    )
else()
    target_sources(libat-imhhw PRIVATE
        ${RC_FILE}
    )
endif()

target_include_directories(libat-imhhw PRIVATE
    ${COMMON_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(libat-imhhw PRIVATE DISABLE_GET_PATCH_LENGTH)

set_target_properties(libat-imhhw PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${PLUGINS_OUTPUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${PLUGINS_OUTPUT_DIR}
    OUTPUT_NAME "libat-imhhw"
    PREFIX ""
)
