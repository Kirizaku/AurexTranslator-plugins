cmake_minimum_required(VERSION 3.10)

project(at_injector_executable LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMMON_SOURCES
    src/main.cpp
    src/memory/memory.cpp
    src/memory/memory.h
    src/license.h
)

function(add_linux_executable BITNESS)
    set(TARGET_NAME "at-injector_${BITNESS}")

    add_executable(${TARGET_NAME} ${COMMON_SOURCES})

    target_include_directories(${TARGET_NAME} PRIVATE ${COMMON_INCLUDE_DIR})
    target_compile_options(${TARGET_NAME} PRIVATE -Wa,--noexecstack -fPIC)

    if(${BITNESS} STREQUAL "32")
        target_compile_options(${TARGET_NAME} PRIVATE -m32)
        target_link_options(${TARGET_NAME} PRIVATE -m32)
        set(OUTPUT_NAME "at-injector_x86")
    else()
        target_compile_options(${TARGET_NAME} PRIVATE -m64)
        target_link_options(${TARGET_NAME} PRIVATE -m64)
        set(OUTPUT_NAME "at-injector_x64")
    endif()

    set(CONFIG_JSON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/metadata.json")
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/note_section.S.in
        ${CMAKE_CURRENT_BINARY_DIR}/note_section_${BITNESS}.S
        @ONLY
    )
    target_sources(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/note_section_${BITNESS}.S)

    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${PLUGINS_BIN_DIR}
        OUTPUT_NAME ${OUTPUT_NAME}
    )

    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PLUGINS_BIN_DIR}
        COMMENT "Creating bin directory for ${TARGET_NAME}"
    )
endfunction()

function(add_windows_executable)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(CURRENT_ARCH "x64")
    else()
        set(CURRENT_ARCH "x86")
    endif()

    add_executable(at-injector ${COMMON_SOURCES})

    target_include_directories(at-injector PRIVATE ${COMMON_INCLUDE_DIR})

    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/resources/metadata.json" RAW_JSON_CONTENT)
    string(REGEX REPLACE "\n" " " RAW_JSON_CONTENT "${RAW_JSON_CONTENT}")
    string(REGEX REPLACE "[ ]+" " " RAW_JSON_CONTENT "${RAW_JSON_CONTENT}")
    string(REPLACE "\"" "\"\"" CONFIG_JSON_PATH "${RAW_JSON_CONTENT}")

    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/windows.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/windows.rc
        @ONLY
    )
    target_sources(at-injector PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/windows.rc)

    set_target_properties(at-injector PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${PLUGINS_BIN_DIR}
        OUTPUT_NAME "at-injector_${CURRENT_ARCH}"
    )

    add_custom_command(TARGET at-injector POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PLUGINS_BIN_DIR}
        COMMENT "Creating bin directory"
    )
endfunction()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_linux_executable(32)
    add_linux_executable(64)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_windows_executable()
endif()
