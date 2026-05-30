#pragma once
#include <cstdint>

// Brute-force camera struct locator.
// Scans process memory for valid 4x4 rotation matrices (the view matrix is a
// very distinctive pattern: 3 mutually-orthogonal unit vectors).  Logs every
// candidate base address together with whatever is at +0x44 (expected position
// X/Y/Z).  Runs on a background thread so it doesn't freeze the game.
namespace CamFinder {
    void Scan();       // start background scan (no-op if already running)
    bool IsScanning(); // true while scan in progress
}
