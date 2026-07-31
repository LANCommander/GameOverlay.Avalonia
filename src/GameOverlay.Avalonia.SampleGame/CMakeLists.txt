add_executable(SampleGame WIN32 main.cpp)
target_link_libraries(SampleGame PRIVATE d3d11 dxgi d3dcompiler)
