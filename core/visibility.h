// cursor-core — the "Show overlay" visibility decision. Pure
// C++17 (no Nexus/ImGui/Windows), so the whole matrix is unit-testable off-game;
// the MumbleLink read that supplies the combat bit and the ImGui draw gate that
// consumes the decision are the Windows-only glue in cursor/src/entry.cpp.
//
// The marker's visibility is chosen independently for two game states — Out of
// combat (Always / While moving / Never) and In combat (Always / Never) — with
// combat read from the GW2 MumbleContext UiState bit and "While moving" derived
// from frame-to-frame pointer motion. See cursor/core/cursor_settings.h for the
// persisted mode fields.
#pragma once

#include <cstdint>

#include "core/cursor_settings.h"

namespace cursor {

// Bit 6 (value 0x40) of the GW2 MumbleContext UiState bitfield == IsInCombat.
// The full MumbleContext layout is runtime-unverified in
// cursor/src/mumble_link.h; this mask is consistent with the published layout's
// IsMapOpen at bit 0 / 0x1, and bit 4 / 0x10 being IsInCompetitiveGamemode
// (not combat). Still runtime-unverified — to be confirmed in-game before relied
// on; a correction is this one line.
inline constexpr std::uint32_t kUiStateInCombat = 0x40u;

// True when the GW2 UiState bitfield reports the player is in combat (bit 6).
// Pure so the combat gate is testable off-game against a synthetic UiState.
bool is_in_combat(std::uint32_t ui_state);

// Pointer-motion tuning for the out-of-combat "While moving" mode. No API
// exposes "cursor is moving", so it is derived from GetMousePos() deltas: a move
// is a frame-to-frame displacement above kPointerMoveThresholdPx, and the marker
// lingers (stays shown) for kPointerLingerMs after the last such move so it does
// not strobe on and off between micro-pauses. Values chosen conservative: a few
// pixels ignores sub-pixel jitter, ~1s reads as "the pointer is actively in use".
inline constexpr float kPointerMoveThresholdPx = 3.0f;
inline constexpr int   kPointerLingerMs        = 1000;

// The pure movement decision: active when the pointer moved above `threshold`
// this frame (dx, dy) OR moved within the last `linger_ms` (ms_since_last_move).
// `ms_since_last_move` is measured from the last above-threshold move; a value
// >= linger_ms with no current motion reads inactive. Testable against synthetic
// delta + elapsed sequences.
bool pointer_active(float dx, float dy, int ms_since_last_move,
                    float threshold, int linger_ms);

// Frame-to-frame pointer tracker for the out-of-combat "While moving" mode. Pure
// state (no ImGui/Windows): the DLL feeds it this frame's pointer position and a
// monotonic millisecond timestamp each frame; update() reports whether the pointer
// is currently "active" (moving now, or within the linger window since the last
// move). Kept in cursor-core so the linger logic is tested off-game rather than
// re-implemented in entry.cpp.
struct PointerMotion {
    bool  has_last      = false; // a prior sample has been recorded
    bool  moved_ever    = false; // an above-threshold move has occurred
    float last_x        = 0.0f;
    float last_y        = 0.0f;
    int   last_move_ms  = 0;      // timestamp of the most recent above-threshold move

    // Feed one frame; returns the current active/inactive verdict. Defaults use
    // the tuning constants above.
    bool update(float x, float y, int now_ms,
                float threshold = kPointerMoveThresholdPx,
                int linger_ms = kPointerLingerMs);
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
//                                While moving -> `pointer_is_active`.
bool should_show_marker(const CursorSettings& s, bool in_gameplay,
                        bool in_combat, bool pointer_is_active,
                        bool settings_open = false);

} // namespace cursor
