if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(AT_OVERLAY_ARCH "x64")
else()
    set(AT_OVERLAY_ARCH "x86")
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

if(NOT EXISTS "${IMGUI_DIR}/imgui.h")
    message(FATAL_ERROR "Dear ImGui not found at ${IMGUI_DIR}")
endif()

if(NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR "libat-overlay needs the vcpkg toolchain file (for Detours) - configure with -DCMAKE_TOOLCHAIN_FILE=<vcpkg root>/scripts/buildsystems/vcpkg.cmake")
endif()
get_filename_component(AT_OVERLAY_VCPKG_ROOT "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
get_filename_component(AT_OVERLAY_VCPKG_ROOT "${AT_OVERLAY_VCPKG_ROOT}" DIRECTORY)
get_filename_component(AT_OVERLAY_VCPKG_ROOT "${AT_OVERLAY_VCPKG_ROOT}" DIRECTORY)
set(AT_OVERLAY_DETOURS_ROOT "${AT_OVERLAY_VCPKG_ROOT}/installed/${AT_OVERLAY_ARCH}-windows-static")

find_path(DETOURS_INCLUDE_DIRS "detours/detours.h"
          PATHS "${AT_OVERLAY_DETOURS_ROOT}/include" NO_DEFAULT_PATH)
find_library(DETOURS_LIBRARY detours
          PATHS "${AT_OVERLAY_DETOURS_ROOT}/lib" NO_DEFAULT_PATH)
if(NOT DETOURS_INCLUDE_DIRS OR NOT DETOURS_LIBRARY)
    message(FATAL_ERROR "Microsoft Detours (static CRT) not found under ${AT_OVERLAY_DETOURS_ROOT} - run: "
                        "vcpkg install detours:${AT_OVERLAY_ARCH}-windows-static")
endif()

add_library(at_overlay_imgui_dx STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx9.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx10.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(at_overlay_imgui_dx PUBLIC ${IMGUI_DIR})

if(AT_OVERLAY_BUILD_VULKAN)
    find_package(Vulkan)
    if(Vulkan_FOUND)
        set(AT_OVERLAY_HAVE_VULKAN ON)
        add_library(at_overlay_imgui_vk STATIC
            ${IMGUI_DIR}/imgui.cpp
            ${IMGUI_DIR}/imgui_draw.cpp
            ${IMGUI_DIR}/imgui_tables.cpp
            ${IMGUI_DIR}/imgui_widgets.cpp
            ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
        )
        target_include_directories(at_overlay_imgui_vk PUBLIC ${IMGUI_DIR})
        target_compile_definitions(at_overlay_imgui_vk PUBLIC
            IMGUI_IMPL_VULKAN_NO_PROTOTYPES=1
            VK_NO_PROTOTYPES=1
        )
        target_link_libraries(at_overlay_imgui_vk PUBLIC Vulkan::Vulkan)
    else()
        message(STATUS "libat-overlay: Vulkan SDK not found, skipping the Vulkan layer")
    endif()
endif()

add_library(libat-overlay SHARED
    ${AT_OVERLAY_WIN}/inject/dllmain.cpp
    ${AT_OVERLAY_WIN}/backends/hook_dxgi.cpp
    ${AT_OVERLAY_WIN}/backends/overlay_d3d12.cpp
    ${AT_OVERLAY_WIN}/backends/overlay_d3d10.cpp
    ${AT_OVERLAY_WIN}/backends/overlay_d3d9.cpp
    ${AT_OVERLAY_WIN}/backends/overlay_gl.cpp
    ${AT_OVERLAY_WIN}/win_capture.cpp
    ${AT_OVERLAY_COMMON}/overlay_text.cpp
    ${AT_OVERLAY_COMMON}/overlay_ui.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/windows.rc
)
target_include_directories(libat-overlay PRIVATE
    ${AT_OVERLAY_COMMON}
    ${AT_OVERLAY_WIN}
    ${AT_OVERLAY_WIN}/backends
    ${AT_OVERLAY_WIN}/layer
    ${AT_PLUGINS_INCLUDE}
    ${DETOURS_INCLUDE_DIRS}
)
target_link_libraries(libat-overlay PRIVATE
    ${DETOURS_LIBRARY} at_overlay_imgui_dx
    d3d9 d3d10 d3d11 d3d12 dxgi dxguid opengl32
)
set_target_properties(libat-overlay PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${PLUGINS_OUTPUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${PLUGINS_OUTPUT_DIR}
    OUTPUT_NAME "libat-overlay_${AT_OVERLAY_ARCH}"
    PREFIX ""
)

set(AT_OVERLAY_TARGETS libat-overlay at_overlay_imgui_dx)

if(AT_OVERLAY_HAVE_VULKAN)
    target_sources(libat-overlay PRIVATE
        ${AT_OVERLAY_WIN}/layer/vklayer_win.cpp
        ${AT_OVERLAY_WIN}/layer/vk_capture_win.cpp
        ${AT_OVERLAY_COMMON}/overlay_vk.cpp
        ${AT_OVERLAY_WIN}/layer/vklayer.def
    )
    target_compile_definitions(libat-overlay PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(libat-overlay PRIVATE at_overlay_imgui_vk)
    list(APPEND AT_OVERLAY_TARGETS at_overlay_imgui_vk)
endif()

if(MSVC)
    foreach(t ${AT_OVERLAY_TARGETS})
        set_property(TARGET ${t} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
endif()
