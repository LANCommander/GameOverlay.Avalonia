// platform/platform_win32.cpp - Windows implementation of the payload's
// OS-abstraction seam (see platform.h).

#include "platform.h"

#include "../log.h"
#include "../shared_state.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace overlay::platform {

SharedMapping MapSharedBlock(uint32_t gamePid, std::size_t size) {
    wchar_t name[64];
    FormatMappingName(name, gamePid);

    // Create-or-open: the host may have created the section before injecting,
    // or we may get here first. Either way both sides converge on one mapping.
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, static_cast<DWORD>(size), name);
    if (!mapping) {
        OVERLAY_LOG("CreateFileMapping('%ls') failed: %lu", name, GetLastError());
        return {};
    }

    void* base = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!base) {
        OVERLAY_LOG("MapViewOfFile failed: %lu", GetLastError());
        CloseHandle(mapping);
        return {};
    }

    return SharedMapping{base, mapping};
}

void UnmapSharedBlock(SharedMapping& mapping, std::size_t) {
    if (mapping.base) {
        UnmapViewOfFile(mapping.base);
        mapping.base = nullptr;
    }
    if (mapping.osHandle) {
        CloseHandle(static_cast<HANDLE>(mapping.osHandle));
        mapping.osHandle = nullptr;
    }
}

uint32_t CurrentProcessId() {
    return ::GetCurrentProcessId();
}

}  // namespace overlay::platform
