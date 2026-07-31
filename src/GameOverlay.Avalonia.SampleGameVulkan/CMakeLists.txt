add_executable(SampleGameVulkan WIN32 main.cpp)

# Vulkan is loaded dynamically from vulkan-1.dll, so only the headers are
# needed at build time - no Vulkan SDK, no import library.
target_include_directories(SampleGameVulkan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/GameOverlay.Avalonia.Native/third_party/vulkan/include
)
