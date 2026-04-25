cmake_minimum_required(VERSION 3.10)
project(at_renpy_plugin LANGUAGES CXX ASM)

if(MSVC)
    enable_language(ASM_MASM)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMMON_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)

set(COMMON_SOURCES
    src/plugin_main.cpp
    src/python_runtime.cpp src/python_runtime.h
    src/unicode_api.cpp src/unicode_api.h
    src/renpy_text.cpp src/renpy_text.h
    src/config.h
    resources/metadata.json
    ${COMMON_INCLUDE_DIR}/memory_utils.cpp ${COMMON_INCLUDE_DIR}/memory_utils.h
    ${COMMON_INCLUDE_DIR}/shared_memory.h
)

# Linux

function(add_linux_plugin BITNESS)
    if(BITNESS STREQUAL "32")
        set(TARGET_NAME "libat-renpy-x86")
        set(OUTPUT_NAME "libat-renpy_x86")
        set(ARCH_FLAG "-m32")
        set(HDE_SOURCES
            ${COMMON_INCLUDE_DIR}/hde/hde32.cpp
            ${COMMON_INCLUDE_DIR}/hde/hde32.h
            ${COMMON_INCLUDE_DIR}/hde/table32.h
        )
    set(HOOK_ASM src/hook/hook_x86.S)
        set(ARCH_DEF BUILD_X32)
    else()
        set(TARGET_NAME "libat-renpy-x64")
        set(OUTPUT_NAME "libat-renpy_x64")
        set(ARCH_FLAG "-m64")
        set(HDE_SOURCES
            ${COMMON_INCLUDE_DIR}/hde/hde64.cpp
            ${COMMON_INCLUDE_DIR}/hde/hde64.h
            ${COMMON_INCLUDE_DIR}/hde/table64.h
        )
    set(HOOK_ASM src/hook/hook_x64.S)
        set(ARCH_DEF BUILD_X64)
    endif()

    set(CONFIG_JSON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/metadata.json")
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/note_section.S.in
        ${CMAKE_CURRENT_BINARY_DIR}/note_section_${BITNESS}.S
        @ONLY
    )

    add_library(${TARGET_NAME} SHARED
        ${COMMON_SOURCES}
        ${HDE_SOURCES}
        ${HOOK_ASM}
        ${CMAKE_CURRENT_BINARY_DIR}/note_section_${BITNESS}.S
    )

    target_include_directories(${TARGET_NAME} PRIVATE
        ${COMMON_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/python
    )

    target_compile_options(${TARGET_NAME} PRIVATE ${ARCH_FLAG} -fPIC)
    target_link_options(${TARGET_NAME}    PRIVATE ${ARCH_FLAG})
    target_compile_definitions(${TARGET_NAME} PRIVATE ${ARCH_DEF})

    set_source_files_properties(
        ${HOOK_ASM}
        ${CMAKE_CURRENT_BINARY_DIR}/note_section_${BITNESS}.S
        PROPERTIES COMPILE_FLAGS "${ARCH_FLAG} -fPIC -Wa,--noexecstack"
    )

    set_target_properties(${TARGET_NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${PLUGINS_OUTPUT_DIR}
        OUTPUT_NAME ${OUTPUT_NAME}
        PREFIX ""
    )
endfunction()

# Windows

function(add_windows_plugin)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(TARGET_NAME "libat-renpy-x64")
        set(OUTPUT_NAME "libat-renpy_x64")
        set(HDE_SOURCES
            ${COMMON_INCLUDE_DIR}/hde/hde64.cpp
            ${COMMON_INCLUDE_DIR}/hde/hde64.h
            ${COMMON_INCLUDE_DIR}/hde/table64.h
        )
        set(ARCH_DEF BUILD_X64)
    else()
        set(TARGET_NAME "libat-renpy-x86")
        set(OUTPUT_NAME "libat-renpy_x86")
        set(HDE_SOURCES
            ${COMMON_INCLUDE_DIR}/hde/hde32.cpp
            ${COMMON_INCLUDE_DIR}/hde/hde32.h
            ${COMMON_INCLUDE_DIR}/hde/table32.h
        )
        set(ARCH_DEF BUILD_X32)
    endif()

    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/resources/metadata.json" RAW_JSON_CONTENT)
    string(REGEX REPLACE "\n"    " " RAW_JSON_CONTENT "${RAW_JSON_CONTENT}")
    string(REGEX REPLACE "[ ]+" " " RAW_JSON_CONTENT "${RAW_JSON_CONTENT}")
    string(REPLACE "\"" "\"\"" CONFIG_JSON_PATH "${RAW_JSON_CONTENT}")
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/windows.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/windows.rc
        @ONLY
    )

    if(MSVC)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(ASM_SOURCE src/hook/hook_x64.asm)
        else()
            set(ASM_SOURCE src/hook/hook_x86.asm)
        endif()
    else()
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(ASM_SOURCE src/hook/hook_x64.S)
        else()
            set(ASM_SOURCE src/hook/hook_x86.S)
        endif()
    endif()

    add_library(${TARGET_NAME} SHARED
        ${COMMON_SOURCES}
        ${HDE_SOURCES}
        ${ASM_SOURCE}
        ${CMAKE_CURRENT_BINARY_DIR}/windows.rc
    )

    target_include_directories(${TARGET_NAME} PRIVATE
        ${COMMON_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/python
    )

    target_compile_definitions(${TARGET_NAME} PRIVATE ${ARCH_DEF})

    set_target_properties(${TARGET_NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${PLUGINS_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY ${PLUGINS_OUTPUT_DIR}
        OUTPUT_NAME ${OUTPUT_NAME}
        PREFIX ""
    )
endfunction()

# Instantiate

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_linux_plugin(32)
    add_linux_plugin(64)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_windows_plugin()
endif()