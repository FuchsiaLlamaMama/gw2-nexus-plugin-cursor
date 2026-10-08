// cursor-finder-core — the "Show overlay" visibility decision. Pure
// C++17 (no Nexus/ImGui/Windows), so the whole matrix is unit-testable off-game;
// the MumbleLink read that supplies the combat bit and the ImGui draw gate that
// consumes the decision are the Windows-only glue in src/entry.cpp.
//
// The marker's visibility is chosen independently for two game states — Out of
// combat (Always / While moving / Never) and In combat (Always / Never) — with
// combat read from the GW2 MumbleContext UiState bit and "While moving" from
// character motion (NexusLinkData_t::IsMoving). See core/cursor_finder_settings.h
// for the persisted mode fields.
#pragma once

#include <cstdint>

#include "core/cursor_finder_settings.h"

namespace cursor_finder {

// Bit 6 (value 0x40) of the GW2 MumbleContext UiState bitfield == IsInCombat.
// The full MumbleContext layout is runtime-unverified in
// src/mumble_link.h; this mask is consistent with the published layout's
// IsMapOpen at bit 0 / 0x1, and bit 4 / 0x10 being IsInCompetitiveGamemode
// (not combat). Still runtime-unverified — to be confirmed in-game before relied
// on; a correction is this one line.
inline constexpr std::uint32_t kUiStateInCombat = 0x40u;

// True when the GW2 UiState bitfield reports the player is in combat (bit 6).
// Pure so the combat gate is testable off-game against a synthetic UiState.
bool is_in_combat(std::uint32_t ui_state);

// Character-motion linger for the out-of-combat "While moving" mode. Nexus sets
// NexusLinkData_t::IsMoving when MumbleLink AvatarPosition changed, recomputed
// every 100 ms. The marker stays shown for kMoveLingerMs after the last moving
// frame so it does not blink on short pauses (or a missed 100 ms window).
inline constexpr int kMoveLingerMs = 1000;

// Per-frame tracker: the DLL feeds it this frame's IsMoving and a monotonic
// millisecond timestamp; update() reports whether the character is moving now
// or moved within the linger window. Pure state (no Nexus/ImGui/Windows), so
// the linger is tested off-game rather than re-implemented in entry.cpp.
struct MotionLinger {
    bool moved_ever   = false; // a moving frame has been seen
    int  last_move_ms = 0;     // timestamp of the most recent moving frame

    bool update(bool moving_now, int now_ms, int linger_ms = kMoveLingerMs);
};

// The core decision: decide whether to draw the marker this frame.
//   - master `enabled` off  -> never shown.
//   - not in gameplay         -> shown only while the settings window is open
//                                (rather than failing open to always-shown): char
//                                select and loading screens, as reported by Nexus
//                                NexusLinkData_t::IsGameplay. The first-run window
//                                usually opens at char select, so edits stay
//                                visible at the pointer while configuring.
//   - in combat               -> the In-combat column: Always / Never (no
//                                While moving in combat).
//   - out of combat           -> the Out-of-combat column: Always / Never, or
//                                While moving -> `moving` (MotionLinger verdict).
bool should_show_marker(const CursorSettings& s, bool in_gameplay,
                        bool in_combat, bool moving,
                        bool settings_open = false);

} // namespace cursor_finder
