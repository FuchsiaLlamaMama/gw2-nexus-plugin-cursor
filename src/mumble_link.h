// GW2 MumbleLink memory layout — the shared-memory block the game publishes and
// Nexus re-exposes as the `DL_MUMBLE_LINK` data resource
// (`AddonAPI_t::DataLink_Get(DL_MUMBLE_LINK)` returns a `MumbleLink*`).
//
// GROUNDING: this layout is transcribed from the public GW2 MumbleLink
// documentation (the standard Mumble `LinkedMem` header + ArenaNet's
// `MumbleContext` in the `context` bytes). The full struct is kept for offset
// correctness; the only field Cursor Finder reads is `ContextData.UiState` (the
// combat bit). The layout is **runtime-unverified until read in-game** (confirm
// combat is UiState bit 6 / 0x40).
//
// Windows/MSVC-only glue; NOT part of cursor-core (the pure combat/visibility
// logic lives in cursor/core/visibility.h and is unit-tested off-game).
#pragma once

#include <cstdint>

namespace cursor {

// The GW2-specific block that lives in the first bytes of LinkedMem::Context.
// #pragma pack(1) to match the game's tightly-packed shared-memory layout.
#pragma pack(push, 1)
struct MumbleContext {
    std::uint8_t  ServerAddress[28]; // sockaddr_in / sockaddr_in6
    std::uint32_t MapId;             // GW2 map id (matches /v2/maps)
    std::uint32_t MapType;
    std::uint32_t ShardId;
    std::uint32_t Instance;
    std::uint32_t BuildId;
    std::uint32_t UiState;           // bitfield (map-open, combat, etc.) — the field we read
    std::uint16_t CompassWidth;      // pixels
    std::uint16_t CompassHeight;     // pixels
    float         CompassRotation;   // radians
    float         PlayerX;           // continent coords
    float         PlayerY;           // continent coords
    float         MapCenterX;        // continent coords
    float         MapCenterY;        // continent coords
    float         MapScale;
    std::uint32_t ProcessId;
    std::uint8_t  MountIndex;
};
#pragma pack(pop)

// The standard Mumble LinkedMem header, with GW2's MumbleContext overlaid on the
// generic `Context` byte array (only the first sizeof(MumbleContext) bytes are
// meaningful to GW2; ContextLen reports how many are used for the Mumble match).
#pragma pack(push, 1)
struct MumbleLink {
    std::uint32_t UiVersion;
    std::uint32_t UiTick;            // increments each frame while the game runs
    float         AvatarPosition[3]; // world/metres
    float         AvatarFront[3];
    float         AvatarTop[3];
    wchar_t       Name[256];
    float         CameraPosition[3];
    float         CameraFront[3];
    float         CameraTop[3];
    wchar_t       Identity[256];     // JSON: name, profession, map_id, fov, ...
    std::uint32_t ContextLen;
    union {
        std::uint8_t  Context[256];
        MumbleContext ContextData;   // GW2 overlay
    };
    wchar_t       Description[2048];
};
#pragma pack(pop)

} // namespace cursor
