#include "core/visibility.h"

namespace cursor {

bool is_in_combat(std::uint32_t ui_state)
{
    return (ui_state & kUiStateInCombat) != 0u;
}

bool pointer_active(float dx, float dy, int ms_since_last_move,
                    float threshold, int linger_ms)
{
    // Moving THIS frame (displacement above the threshold, compared squared so no
    // sqrt) reads active regardless of the timer.
    if ((dx * dx + dy * dy) > (threshold * threshold)) { return true; }
    // Otherwise it stays active only while still inside the linger window since the
    // last real move. A negative/absent elapsed is treated as "no recent move".
    return ms_since_last_move >= 0 && ms_since_last_move < linger_ms;
}

bool PointerMotion::update(float x, float y, int now_ms, float threshold, int linger_ms)
{
    // Frame-to-frame displacement (zero on the very first sample, before a prior
    // position exists — so the first frame never counts as a move).
    const float dx = has_last ? (x - last_x) : 0.0f;
    const float dy = has_last ? (y - last_y) : 0.0f;
    const bool moving_now =
        has_last && (dx * dx + dy * dy) > (threshold * threshold);

    if (moving_now)
    {
        last_move_ms = now_ms;
        moved_ever   = true;
    }

    // Elapsed since the last real move — or a value at/over the linger window when
    // nothing has ever moved, so pointer_active reads inactive on the opening
    // still frames.
    const int elapsed = moved_ever ? (now_ms - last_move_ms) : linger_ms;

    last_x   = x;
    last_y   = y;
    has_last = true;

    return pointer_active(dx, dy, elapsed, threshold, linger_ms);
}

bool should_show_marker(const CursorSettings& s, bool in_gameplay,
                        bool in_combat, bool pointer_is_active,
                        bool settings_open)
{
    if (!s.enabled) { return false; }
    // Outside gameplay (char select, loading screens) the matrix doesn't apply:
    // the marker draws only while the settings window is open, so edits made
    // there are visible at the pointer (rather than failing open to always-shown).
    if (!in_gameplay) { return settings_open; }

    if (in_combat)
    {
        switch (s.in_combat_mode)
        {
            case InCombatMode::Always: return true;
            case InCombatMode::Never:  return false;
        }
        return true; // defensive default: shown
    }

    switch (s.out_of_combat_mode)
    {
        case OutOfCombatMode::Always:      return true;
        case OutOfCombatMode::WhileMoving: return pointer_is_active;
        case OutOfCombatMode::Never:       return false;
    }
    return true; // defensive default: shown
}

} // namespace cursor
