# ---------------------------------------------------------------------------
# MinHook (vendored, MIT). Compiled as C into a static lib.
#
# Windows-only: MinHook writes x86/x64 trampolines against the Win32 loader. The
# Linux payload interposes via LD_PRELOAD + dlsym instead and needs no such
# in-place patching, so MinHook is not built there.
# ---------------------------------------------------------------------------
if(WIN32)
    add_library(minhook STATIC
        third_party/minhook/src/buffer.c
        third_party/minhook/src/hook.c
        third_party/minhook/src/trampoline.c
        third_party/minhook/src/hde/hde32.c
        third_party/minhook/src/hde/hde64.c
    )
    target_include_directories(minhook PUBLIC third_party/minhook/include)
    target_include_directories(minhook PRIVATE third_party/minhook/src)
    if(MSVC)
        # Third-party code: don't fail our build on its warnings.
        target_compile_options(minhook PRIVATE /W0)
    endif()
endif()

# ---------------------------------------------------------------------------
# The injected payload.
#
# Almost every translation unit is OS-specific today (the DLL entry point, the
# DXGI/present hooks, the per-API renderers, the WndProc input capture), so the
# source list is selected per platform rather than shared. The OS-abstraction
# seam is platform/platform.h; each platform provides one implementation of it.
# ---------------------------------------------------------------------------
if(WIN32)
    set(payload_sources
        dllmain.cpp
        hooks.cpp
        d3d9_renderer.cpp
        d3d9_hooks.cpp
        d3d10_renderer.cpp
        d3d11_renderer.cpp
        d3d12_renderer.cpp
        vulkan_renderer.cpp
        vulkan_hooks.cpp
        opengl_renderer.cpp
        opengl_hooks.cpp
        input.cpp
        platform/platform_win32.cpp
        # Module-definition file: pins OverlayDetach's exported name undecorated.
        # Without it, the __stdcall export decorates to _OverlayDetach@4 on x86
        # and the host's by-name GetProcAddress lookup fails. MSVC treats a .def
        # in the source list as the linker's /DEF input automatically.
        GameOverlay.Native.def
    )
elseif(UNIX AND NOT APPLE)
    # Linux backend: LD_PRELOAD interposer + CPU-fed GLX renderer, with the
    # platform.h seam implemented over POSIX shm.
    set(payload_sources
        platform/platform_linux.cpp
        linux_payload.cpp
        linux_input.cpp
        opengl_glx_renderer.cpp
    )
else()
    message(FATAL_ERROR "GameOverlay.Native has no payload sources for this platform yet.")
endif()

add_library(GameOverlay.Native SHARED ${payload_sources})

if(WIN32)
    target_link_libraries(GameOverlay.Native PRIVATE minhook d3d10 d3d11 d3d12 dxgi d3dcompiler opengl32)
elseif(UNIX AND NOT APPLE)
    # dl for dlsym/RTLD_NEXT interposition; rt for POSIX shm_open (a no-op link
    # on glibc >= 2.34 where it moved into libc); GL/X11 for the compositing draw
    # and drawable geometry.
    target_link_libraries(GameOverlay.Native PRIVATE ${CMAKE_DL_LIBS} rt GL X11 pthread)
endif()

# Vulkan is resolved through the game's own loader at runtime, so only the
# headers are needed - no SDK, no import library.
target_include_directories(GameOverlay.Native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan/include
)

set_target_properties(GameOverlay.Native PROPERTIES
    OUTPUT_NAME "GameOverlay.Native"
    PREFIX ""
)

# ---------------------------------------------------------------------------
# Linux Vulkan overlay layer (a separate .so + JSON manifest). Vulkan is
# intercepted with a layer rather than LD_PRELOAD, so it is not part of the
# GameOverlay.Native payload above; it shares platform_linux.cpp (shm) and the
# compositor. Uses the system Vulkan headers (vk_layer.h is loader-side).
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    add_library(VkLayer_gameoverlay SHARED
        vk_layer_linux.cpp
        linux_vulkan_renderer.cpp
        linux_input.cpp
        platform/platform_linux.cpp
    )
    # rt for shm; X11 + pthread for the input grab thread shared with the GL path.
    target_link_libraries(VkLayer_gameoverlay PRIVATE rt X11 pthread)
    # Default lib prefix → libVkLayer_gameoverlay.so, matching the manifest.
    # Copy the manifest next to it so VK_LAYER_PATH=<bin> finds both.
    configure_file(
        ${CMAKE_SOURCE_DIR}/tools/linux/VkLayer_gameoverlay.json
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/VkLayer_gameoverlay.json COPYONLY)
endif()
