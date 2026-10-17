project(at_overlay_plugin LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(AT_OVERLAY_BUILD_VULKAN "Build the Vulkan implicit layer (requires the Vulkan SDK)" ON)

set(AT_OVERLAY_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/src/common")
set(AT_OVERLAY_WIN "${CMAKE_CURRENT_SOURCE_DIR}/src/windows")
set(AT_OVERLAY_LINUX "${CMAKE_CURRENT_SOURCE_DIR}/src/linux")
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
set(AT_PLUGINS_INCLUDE "${CMAKE_SOURCE_DIR}/include")

if(WIN32)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/overlay_windows.cmake)
else()
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/overlay_linux.cmake)
endif()
