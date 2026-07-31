// platform/platform.h - the payload's OS-abstraction seam.
//
// Everything the injected payload needs from the operating system that differs
// between Windows, Linux and macOS is declared here and implemented once per
// platform (platform_win32.cpp, platform_linux.cpp, ...). The rest of the
// payload - the renderers, the compositing math, the shared-state contract -
// stays platform-neutral and calls only through these functions.
//
// This first cut covers the shared control-block mapping and process identity.
// Present-hook installation, input capture and game-window discovery are
// abstracted here as they are ported off their current Win32 implementations.
#pragma once

#include <cstddef>
#include <cstdint>

namespace overlay::platform {

// An OS-mapped shared-memory region plus whatever handle the OS needs to keep
// it alive. `base` is the mapped address the payload reads and writes;
// `osHandle` is opaque (a HANDLE on Windows, a file descriptor on POSIX) and is
// only ever interpreted by the matching platform implementation.
struct SharedMapping {
    void* base = nullptr;
    void* osHandle = nullptr;
};

// Creates or opens the named control-block section for a game process and maps
// it read/write. Uses create-or-open semantics so it does not matter whether
// the host or the payload arrives first. Returns a zeroed mapping on failure.
SharedMapping MapSharedBlock(uint32_t gamePid, std::size_t size);

// Unmaps and releases a region returned by MapSharedBlock. Safe to call with a
// zeroed mapping; clears the mapping's fields on return.
void UnmapSharedBlock(SharedMapping& mapping, std::size_t size);

// The id of the process the payload is running inside.
uint32_t CurrentProcessId();

}  // namespace overlay::platform
