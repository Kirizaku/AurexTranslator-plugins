cmake_minimum_required(VERSION 3.10)

project(at_example_plugin LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(COMMON_INCLUDE_DIR ../include)
set(EXAMPLE_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/output")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(BUILD_32_BIT)
        set(IS_32_BIT ON)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m32")
        set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -m32")
    else()
        set(IS_32_BIT OFF)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m64")
        set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -m64")
    endif()
elseif(WIN32)
    if (CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(IS_32_BIT ON)
    else()
        set(IS_32_BIT OFF)
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CONFIG_JSON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/metadata.json")
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

        string(REGEX MATCH "\"executable\"[ \t]*:[ \t]*\"([^\"]*)\""
                   _MATCH "${RAW_JSON_CONTENT}")
        set(_EXEC_NAME "${CMAKE_MATCH_1}")

        if(NOT _EXEC_NAME MATCHES "\\.exe$")
            set(_EXEC_NAME "${_EXEC_NAME}.exe")
        endif()

        string(REGEX REPLACE
            "\"executable\"[ \t]*:[ \t]*\"[^\"]*\""
            "\"executable\": \"${_EXEC_NAME}\""
            RAW_JSON_CONTENT "${RAW_JSON_CONTENT}")

        string(REPLACE "\"" "\"\"" CONFIG_JSON_PATH "${RAW_JSON_CONTENT}")

    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/windows.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/resources/windows.rc
        @ONLY
    )
    set(RC_FILE ${CMAKE_CURRENT_BINARY_DIR}/resources/windows.rc)
endif()

if (IS_32_BIT)
    set(HDE_SRC
        ${COMMON_INCLUDE_DIR}/hde/hde32.cpp
        ${COMMON_INCLUDE_DIR}/hde/hde32.h
        ${COMMON_INCLUDE_DIR}/hde/table32.h
    )
else()
    set(HDE_SRC
        ${COMMON_INCLUDE_DIR}/hde/hde64.cpp
        ${COMMON_INCLUDE_DIR}/hde/hde64.h
        ${COMMON_INCLUDE_DIR}/hde/table64.h
    )
endif()

# Test plugin
add_library(libat_example SHARED
    src/plugin_main.cpp
    ${COMMON_INCLUDE_DIR}/memory_utils.cpp ${COMMON_INCLUDE_DIR}/memory_utils.h
    ${COMMON_INCLUDE_DIR}/ipc_pipe.h
    ${HDE_SRC}
)

target_include_directories(libat_example PRIVATE
    ${COMMON_INCLUDE_DIR}
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_options(libat_example PRIVATE -Wa,--noexecstack -fPIC)
    target_sources(libat_example PRIVATE
        ${ASM_FILE}
    )
else()
    target_sources(libat_example PRIVATE
        ${RC_FILE}
    )
endif()

set_target_properties(libat_example PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${EXAMPLE_OUTPUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${EXAMPLE_OUTPUT_DIR}
    OUTPUT_NAME "libat-example"
    PREFIX ""
)

##################################################################################

# Test target
add_executable(test_target
    src/test_target.cpp
    resources/metadata.json
)

set_target_properties(test_target PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${EXAMPLE_OUTPUT_DIR}
)
