cmake_minimum_required(VERSION 3.20)
project(GameOverlay.Avalonia LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    # Static CRT everywhere. The injected DLL lands in an arbitrary game process
    # and must not depend on a VC++ redistributable being installed there.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # We call the wide Win32 APIs exclusively; without this, macros like
    # IDC_ARROW resolve to their ANSI form and refuse to bind to the W entry
    # points. Windows-only: these macros mean nothing on other platforms.
    add_compile_definitions(UNICODE _UNICODE)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /Zi)
    add_link_options(/DEBUG)
elseif(UNIX)
    # The payload is a shared object injected via LD_PRELOAD; position-
    # independent code and hidden-by-default visibility keep only the exported
    # interposers visible.
    add_compile_options(-Wall -Wextra -fPIC -fvisibility=hidden)
endif()

# Everything lands in one flat bin/<config> dir so build.ps1 and the injector
# can find the payload next to the host without probing config-specific subpaths.
foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${cfg} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY${cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

add_subdirectory(src/GameOverlay.Avalonia.Native)

# The sample "games" are per-API injection test targets. Today they are all
# Win32 (D3D9-12, plus Win32-windowed Vulkan/OpenGL); a Linux GLX/EGL sample
# arrives with the Linux backend in a later phase.
if(WIN32)
    add_subdirectory(src/GameOverlay.Avalonia.SampleGame)
    add_subdirectory(src/GameOverlay.Avalonia.SampleGameD3D9)
    add_subdirectory(src/GameOverlay.Avalonia.SampleGameD3D10)
    add_subdirectory(src/GameOverlay.Avalonia.SampleGameD3D12)
    add_subdirectory(src/GameOverlay.Avalonia.SampleGameVulkan)
    add_subdirectory(src/GameOverlay.Avalonia.SampleGameOpenGL)
endif()
