set(OpenGL_GL_PREFERENCE LEGACY)
find_package(Vulkan REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EGL egl IMPORTED_TARGET REQUIRED)

set(AT_OVERLAY_LINUX_INJECT "${AT_OVERLAY_LINUX}/inject")
set(AT_OVERLAY_LINUX_BACKENDS "${AT_OVERLAY_LINUX}/backends")

set(AT_OVERLAY_LINUX_INCLUDES
    ${AT_OVERLAY_COMMON}
    ${AT_OVERLAY_LINUX}
    ${AT_OVERLAY_LINUX_BACKENDS}
    ${AT_OVERLAY_LINUX_INJECT}
    ${AT_PLUGINS_INCLUDE}
)

function(at_overlay_add_note_section out_var metadata_name bitness)
    set(json_path "${CMAKE_CURRENT_SOURCE_DIR}/resources/metadata_linux_${metadata_name}.json")
    set(note_out "${CMAKE_CURRENT_BINARY_DIR}/note_section_${metadata_name}_${bitness}.S")
    set(CONFIG_JSON_PATH "${json_path}")
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/note_section.S.in
        ${note_out}
        @ONLY
    )
    set_source_files_properties(${note_out}
        PROPERTIES COMPILE_FLAGS "-fPIC -Wa,--noexecstack"
        OBJECT_DEPENDS "${json_path}")
    set(${out_var} ${note_out} PARENT_SCOPE)
endfunction()

function(at_overlay_add_linux BITNESS)
    if(BITNESS STREQUAL "32")
        set(ARCH_FLAG "-m32")
        set(AT_OVERLAY_ARCH "x86")
        set(MULTIARCH_DIRS /usr/lib/i386-linux-gnu /usr/lib32)
        find_library(AT_OVERLAY_VULKAN_LIB_32 NAMES vulkan PATHS ${MULTIARCH_DIRS} NO_DEFAULT_PATH)
        find_library(AT_OVERLAY_GL_LIB_32 NAMES GL PATHS ${MULTIARCH_DIRS} NO_DEFAULT_PATH)
        set(VULKAN_LIB "${AT_OVERLAY_VULKAN_LIB_32}")
        set(GL_LIB "${AT_OVERLAY_GL_LIB_32}")
        if(NOT VULKAN_LIB OR NOT GL_LIB)
            return()
        endif()
    else()
        set(ARCH_FLAG "-m64")
        set(AT_OVERLAY_ARCH "x64")
        set(VULKAN_LIB "Vulkan::Vulkan")
        set(GL_LIB "OpenGL::GL")
    endif()

    at_overlay_add_note_section(NOTE_SHIM shim ${BITNESS})
    at_overlay_add_note_section(NOTE_OPENGL opengl ${BITNESS})
    at_overlay_add_note_section(NOTE_VULKAN vulkan ${BITNESS})

    set(INCLUDES ${AT_OVERLAY_LINUX_INCLUDES})

    add_library(at_overlay_imgui_vk_${BITNESS} STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    )
    target_include_directories(at_overlay_imgui_vk_${BITNESS} PUBLIC ${IMGUI_DIR} ${Vulkan_INCLUDE_DIRS})
    target_compile_definitions(at_overlay_imgui_vk_${BITNESS} PUBLIC
        IMGUI_IMPL_VULKAN_NO_PROTOTYPES=1
        VK_NO_PROTOTYPES=1
    )

    add_library(at_overlay_imgui_gl_${BITNESS} STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(at_overlay_imgui_gl_${BITNESS} PUBLIC ${IMGUI_DIR})
    set_target_properties(at_overlay_imgui_vk_${BITNESS} at_overlay_imgui_gl_${BITNESS} PROPERTIES
        POSITION_INDEPENDENT_CODE ON)

    set(ARCH_TARGETS at_overlay_imgui_vk_${BITNESS} at_overlay_imgui_gl_${BITNESS})

    add_library(at_overlay_vklayer_linux_${BITNESS} SHARED
        ${AT_OVERLAY_LINUX}/vklayer.c
        ${AT_OVERLAY_LINUX}/capture.c
        ${AT_OVERLAY_COMMON}/overlay_vk.cpp
        ${AT_OVERLAY_COMMON}/overlay_text.cpp
        ${AT_OVERLAY_COMMON}/overlay_ui.cpp
        ${NOTE_VULKAN}
    )
    target_include_directories(at_overlay_vklayer_linux_${BITNESS} PRIVATE ${INCLUDES} ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(at_overlay_vklayer_linux_${BITNESS} PRIVATE
        ${VULKAN_LIB} at_overlay_imgui_vk_${BITNESS} Threads::Threads
    )
    set_target_properties(at_overlay_vklayer_linux_${BITNESS} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${PLUGINS_OUTPUT_DIR}
        OUTPUT_NAME "at-overlay-vulkan_${AT_OVERLAY_ARCH}"
        PREFIX "lib"
        LINK_FLAGS "-Wl,--version-script=\"${AT_OVERLAY_LINUX}/vklayer.version\""
    )
    list(APPEND ARCH_TARGETS at_overlay_vklayer_linux_${BITNESS})

    add_library(at_overlay_opengl_linux_${BITNESS} SHARED
        ${AT_OVERLAY_LINUX_BACKENDS}/glinject.c
        ${AT_OVERLAY_LINUX_BACKENDS}/overlay_gl.cpp
        ${AT_OVERLAY_LINUX}/capture.c
        ${AT_OVERLAY_LINUX_INJECT}/dlsym.c
        ${AT_OVERLAY_LINUX_INJECT}/elfhacks.c
        ${AT_OVERLAY_COMMON}/overlay_text.cpp
        ${AT_OVERLAY_COMMON}/overlay_ui.cpp
        ${NOTE_OPENGL}
    )
    target_include_directories(at_overlay_opengl_linux_${BITNESS} PRIVATE
        ${INCLUDES} ${Vulkan_INCLUDE_DIRS}
    )
    target_link_libraries(at_overlay_opengl_linux_${BITNESS} PRIVATE
        ${CMAKE_DL_LIBS} ${GL_LIB} Threads::Threads at_overlay_imgui_gl_${BITNESS}
    )
    set_target_properties(at_overlay_opengl_linux_${BITNESS} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${PLUGINS_OUTPUT_DIR}
        OUTPUT_NAME "at-overlay-opengl_${AT_OVERLAY_ARCH}"
        PREFIX "lib"
        LINK_FLAGS "-Wl,--version-script=\"${AT_OVERLAY_LINUX_BACKENDS}/glinject.version\""
    )
    list(APPEND ARCH_TARGETS at_overlay_opengl_linux_${BITNESS})

    add_library(at_overlay_shim_linux_${BITNESS} SHARED
        ${AT_OVERLAY_LINUX_INJECT}/shim.cpp
        ${AT_OVERLAY_LINUX_INJECT}/elfhacks.c
        ${NOTE_SHIM}
    )
    target_include_directories(at_overlay_shim_linux_${BITNESS} PRIVATE ${INCLUDES})
    target_link_libraries(at_overlay_shim_linux_${BITNESS} PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
    target_compile_options(at_overlay_shim_linux_${BITNESS} PRIVATE -fvisibility=hidden)
    set_target_properties(at_overlay_shim_linux_${BITNESS} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${PLUGINS_OUTPUT_DIR}
        OUTPUT_NAME "at-overlay-shim_${AT_OVERLAY_ARCH}"
        PREFIX "lib"
        LINK_FLAGS "-Wl,--version-script=\"${AT_OVERLAY_LINUX_INJECT}/shim.version\" -Wl,--enable-new-dtags -Wl,-rpath,\\$ORIGIN"
    )
    list(APPEND ARCH_TARGETS at_overlay_shim_linux_${BITNESS})

    foreach(t ${ARCH_TARGETS})
        target_compile_options(${t} PRIVATE ${ARCH_FLAG})
        target_link_options(${t} PRIVATE ${ARCH_FLAG})
    endforeach()

endfunction()

at_overlay_add_linux(64)
at_overlay_add_linux(32)
