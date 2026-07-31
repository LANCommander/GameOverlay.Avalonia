// platform/platform_linux.cpp - Linux implementation of the payload's
// OS-abstraction seam (see platform.h).
//
// The control block is a POSIX named shared-memory object (shm_open + mmap),
// the direct analogue of the Windows named file mapping. Both host and payload
// derive the same name from the game pid, so create-or-open converges on one
// object regardless of who arrives first.

#include "platform.h"

#include "../log.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace overlay::platform {

namespace {

// Mirrors the managed side's logical name with the POSIX shm prefix. A POSIX
// shared-memory name is a single leading slash followed by a name with no
// further slashes.
void FormatShmName(char (&buffer)[64], uint32_t gamePid) {
    std::snprintf(buffer, sizeof(buffer), "/AvaloniaOverlay.State.%u", gamePid);
}

}  // namespace

SharedMapping MapSharedBlock(uint32_t gamePid, std::size_t size) {
    char name[64];
    FormatShmName(name, gamePid);

    // 0600: only this user's processes (host + game) ever touch it.
    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        OVERLAY_LOG("shm_open('%s') failed: %s", name, std::strerror(errno));
        return {};
    }

    // A freshly created object is zero-length; size it. Harmless (idempotent) if
    // the other side already did so.
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        OVERLAY_LOG("ftruncate('%s', %zu) failed: %s", name, size, std::strerror(errno));
        close(fd);
        return {};
    }

    void* base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        OVERLAY_LOG("mmap('%s') failed: %s", name, std::strerror(errno));
        close(fd);
        return {};
    }

    // The mapping stays valid after the fd is closed, but we keep the fd so the
    // object can be released deterministically on detach.
    return SharedMapping{base, reinterpret_cast<void*>(static_cast<intptr_t>(fd))};
}

void UnmapSharedBlock(SharedMapping& mapping, std::size_t size) {
    if (mapping.base) {
        munmap(mapping.base, size);
        mapping.base = nullptr;
    }
    if (mapping.osHandle) {
        close(static_cast<int>(reinterpret_cast<intptr_t>(mapping.osHandle)));
        mapping.osHandle = nullptr;
    }
}

uint32_t CurrentProcessId() {
    return static_cast<uint32_t>(getpid());
}

}  // namespace overlay::platform
