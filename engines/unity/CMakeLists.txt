cmake_minimum_required(VERSION 3.10)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    project(at_unity_plugin LANGUAGES CXX ASM)
else()
    project(at_unity_plugin LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMMON_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)

set(COMMON_SOURCES
    src/plugin_main.cpp
    src/config.h

    src/runtime_module.cpp src/runtime_module.h
    src/settings.cpp src/settings.h
    src/hook_engine.cpp src/hook_engine.h
    src/text_sink.cpp src/text_sink.h

    src/managed_hooks.cpp src/managed_hooks.h
    src/unity_targets.cpp
    
    src/mono_runtime.cpp src/mono_runtime.h
    src/il2cpp_runtime.cpp src/il2cpp_runtime.h

    resources/metadata.json
    ${COMMON_INCLUDE_DIR}/hook/memory_utils.cpp ${COMMON_INCLUDE_DIR}/hook/memory_utils.h
    ${COMMON_INCLUDE_DIR}/hook/code_patch.cpp ${COMMON_INCLUDE_DIR}/hook/code_patch.h
    ${COMMON_INCLUDE_DIR}/hook/trampoline.cpp ${COMMON_INCLUDE_DIR}/hook/trampoline.h
    ${COMMON_INCLUDE_DIR}/host/ipc_pipe.h
)

# Linux

function(add_linux_plugin BITNESS)
    if(BITNESS STREQUAL "32")
        set(TARGET_NAME "libat-unity-x86")
        set(OUTPUT_NAME "libat-unity_x86")
        set(ARCH_FLAG "-m32")
        set(HDE_SOURCES
            ${COMMON_INCLUDE_DIR}/hook/hde/hde32.cpp
            ${COMMON_INCLUDE_DIR}/hook/hde/hde32.h
            ${COMMON_INCLUDE_DIR}/hook/hde/table32.h
        )
        set(ARCH_DEF BUILD_X32)
    else()
        set(TARGET_NAME "libat-unity-x64")
        set(OUTPUT_NAME "libat-unity_x64")
        set(ARCH_FLAG "-m64")
        set(HDE_SOURCES
            ${COMMON_INCLUDE_DIR}/hook/hde/hde64.cpp
            ${COMMON_INCLUDE_DIR}/hook/hde/hde64.h
            ${COMMON_INCLUDE_DIR}/hook/hde/table64.h
        )
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
        ${CMAKE_CURRENT_BINARY_DIR}/note_section_${BITNESS}.S
    )

    target_include_directories(${TARGET_NAME} PRIVATE
        ${COMMON_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_compile_options(${TARGET_NAME} PRIVATE ${ARCH_FLAG} -fPIC)
    target_link_options(${TARGET_NAME}    PRIVATE ${ARCH_FLAG})
    target_compile_definitions(${TARGET_NAME} PRIVATE ${ARCH_DEF})
    target_link_libraries(${TARGET_NAME} PRIVATE pthread dl)

    set_source_files_properties(
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
        set(TARGET_NAME "libat-unity-x64")
        set(OUTPUT_NAME "libat-unity_x64")
        set(HDE_SOURCES
            ${COMMON_INCLUDE_DIR}/hook/hde/hde64.cpp
            ${COMMON_INCLUDE_DIR}/hook/hde/hde64.h
            ${COMMON_INCLUDE_DIR}/hook/hde/table64.h
        )
        set(ARCH_DEF BUILD_X64)
    else()
        set(TARGET_NAME "libat-unity-x86")
        set(OUTPUT_NAME "libat-unity_x86")
        set(HDE_SOURCES
            ${COMMON_INCLUDE_DIR}/hook/hde/hde32.cpp
            ${COMMON_INCLUDE_DIR}/hook/hde/hde32.h
            ${COMMON_INCLUDE_DIR}/hook/hde/table32.h
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

    add_library(${TARGET_NAME} SHARED
        ${COMMON_SOURCES}
        ${HDE_SOURCES}
        ${CMAKE_CURRENT_BINARY_DIR}/windows.rc
    )

    target_include_directories(${TARGET_NAME} PRIVATE
        ${COMMON_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
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
