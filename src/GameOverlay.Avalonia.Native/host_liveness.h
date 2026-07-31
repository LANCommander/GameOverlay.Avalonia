// host_liveness.h - detects a dead overlay host on the payload side.
//
// The host bumps SharedState::hostHeartbeat every frame. If it stops advancing
// the host process has crashed or exited, and the payload must stop compositing
// and hand input back to the game - otherwise a crashed host would leave a stale
// overlay frozen on screen and, if it was capturing, the game permanently deaf.
// This mirrors HostIsAlive/ForceReleaseCapture on Windows.
#pragma once

#include "shared_state.h"

namespace overlay {

struct HostLiveness {
    uint32_t last = 0;
    int      stall = 0;
    bool     everAlive = false;

    // Returns false once the heartbeat has been frozen for deadFrames presents
    // (~2s at 60 fps). Never reports "dead" before the host has been seen alive
    // once, so a not-yet-started host is not mistaken for a crashed one.
    bool Alive(const SharedState* state, int deadFrames = 120) {
        const uint32_t hb = state->hostHeartbeat;
        if (hb != last) {
            last = hb;
            stall = 0;
            everAlive = true;
            return true;
        }
        if (!everAlive) return true;
        return ++stall < deadFrames;
    }
};

}  // namespace overlay
